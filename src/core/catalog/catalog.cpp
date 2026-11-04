#include "core/catalog/catalog.hpp"

#include "core/catalog/backup.hpp"
#include "core/catalog/schema.hpp"
#include "core/platform/fs.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <string>

namespace sheepdog::catalog {

namespace {

class CatalogCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "catalog"; }
    [[nodiscard]] std::string message(int code) const override {
        switch (static_cast<CatalogErrc>(code)) {
        case CatalogErrc::corrupt:
            return "the catalog is damaged";
        case CatalogErrc::network_location:
            return "the catalog cannot be kept on a network share";
        case CatalogErrc::newer_schema:
            return "the catalog was made by a newer version of Sheepdog";
        case CatalogErrc::not_migrated:
            return "the catalog has an older schema and must be opened for writing first";
        case CatalogErrc::not_a_catalog:
            return "the file is not a Sheepdog catalog";
        case CatalogErrc::wal_unavailable:
            return "the catalog's filesystem does not support write-ahead logging";
        }
        return "unknown catalog error";
    }
};

bool is_damage(const Error& error) {
    if (error.code.category() != sqlite_category()) {
        return false;
    }
    const int primary = error.code.value() & 0xff;
    return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB;
}

Error corrupt(const NativePath& catalog, const std::string& finding) {
    return Error{.code = make_error_code(CatalogErrc::corrupt),
                 .path = catalog,
                 .detail = finding + ". " + restore_advice(catalog)};
}

// Damage reported by any statement is turned into the same clear refusal.
Error classify(const Error& error, const NativePath& catalog) {
    return is_damage(error) ? corrupt(catalog, error.detail) : error;
}

} // namespace

const std::error_category& catalog_category() noexcept {
    static const CatalogCategory category;
    return category;
}

std::error_code make_error_code(CatalogErrc code) noexcept {
    return {static_cast<int>(code), catalog_category()};
}

std::string restore_advice(const NativePath& catalog) {
    const auto backup = newest_backup(catalog);
    if (!backup) {
        return "No backup was found next to it";
    }
    return "The newest backup is " + platform::display_string(*backup) +
           "; close Sheepdog and copy it over the catalog to restore it";
}

Result<void> check_catalog_location(const NativePath& catalog) {
    const auto remote = platform::is_network_location(catalog);
    if (!remote) {
        return remote.error();
    }
    if (*remote) {
        return Error{.code = make_error_code(CatalogErrc::network_location),
                     .path = catalog,
                     .detail = "choose a folder on a local disk"};
    }
    return Ok{};
}

Result<void> check_integrity(Database& db) {
    // The first three findings are enough to tell what happened; one message, one line.
    auto stmt = db.prepare("PRAGMA integrity_check(3)");
    if (!stmt) {
        return classify(stmt.error(), db.path());
    }
    std::string findings;
    for (;;) {
        auto row = stmt->step();
        if (!row) {
            return classify(row.error(), db.path());
        }
        if (!*row) {
            break;
        }
        std::string line = stmt->column_text(0);
        if (line == "ok") {
            continue;
        }
        std::ranges::replace(line, '\n', ' ');
        if (!findings.empty()) {
            findings += "; ";
        }
        findings += line;
    }
    if (!findings.empty()) {
        return corrupt(db.path(), "integrity_check: " + findings);
    }
    return Ok{};
}

Result<Database> open_catalog(const NativePath& catalog) {
    if (auto local = check_catalog_location(catalog); !local) {
        return local.error();
    }
    auto db = Database::open(catalog, OpenMode::create);
    if (!db) {
        return classify(db.error(), catalog);
    }
    if (auto ok = check_integrity(*db); !ok) {
        return ok.error();
    }
    // journal_mode=WAL is stored in the file, so a foreign database is refused before it.
    if (auto identity = check_identity(*db); !identity) {
        return classify(identity.error(), catalog);
    }
    const auto mode = db->query_text("PRAGMA journal_mode = WAL");
    if (!mode) {
        return classify(mode.error(), catalog);
    }
    if (*mode != "wal") {
        return Error{.code = make_error_code(CatalogErrc::wal_unavailable),
                     .path = catalog,
                     .detail = "journal_mode is " + *mode};
    }
    // NORMAL is durable enough in WAL mode (a power cut may lose the last commits, never the
    // catalog); the organise journal will use FULL for its own commits. In WAL mode the fsyncs
    // happen at checkpoints, so checkpointing every 10000 pages (about 40 MB) instead of 1000
    // halves the time a scan spends writing; the WAL is trimmed back to 64 MB afterwards.
    if (auto ok = db->exec("PRAGMA synchronous = NORMAL;"
                           "PRAGMA foreign_keys = ON;"
                           "PRAGMA trusted_schema = OFF;"
                           "PRAGMA wal_autocheckpoint = 10000;"
                           "PRAGMA journal_size_limit = 67108864;");
        !ok) {
        return classify(ok.error(), catalog);
    }
    if (auto migrated = migrate(*db); !migrated) {
        return classify(migrated.error(), catalog);
    }
    return std::move(*db);
}

Result<Database> open_catalog_read_only(const NativePath& catalog) {
    if (auto local = check_catalog_location(catalog); !local) {
        return local.error();
    }
    auto db = Database::open(catalog, OpenMode::read_only);
    if (!db) {
        return classify(db.error(), catalog);
    }
    if (auto ok = db->exec("PRAGMA query_only = ON; PRAGMA trusted_schema = OFF;"); !ok) {
        return classify(ok.error(), catalog);
    }
    const auto version = check_identity(*db);
    if (!version) {
        return classify(version.error(), catalog);
    }
    if (*version < kSchemaVersion) {
        return Error{.code = make_error_code(CatalogErrc::not_migrated), .path = catalog};
    }
    return std::move(*db);
}

} // namespace sheepdog::catalog
