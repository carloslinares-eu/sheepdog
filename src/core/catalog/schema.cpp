#include "core/catalog/schema.hpp"

#include "core/catalog/backup.hpp"
#include "core/catalog/errors.hpp"

#include <array>
#include <string>

namespace sheepdog::catalog {

namespace {

// Schema v1: volumes, roots, files and their metadata. Paths are stored twice: native bytes
// (BLOB; WTF-8 on Windows) and UTF-8 display text. Times are nanoseconds since 1970 UTC.
// file.kind is platform::EntryKind (0 file, 1 directory, 2 link, 3 other).
constexpr std::string_view kSchemaV1 = R"sql(
CREATE TABLE volume(
    id             INTEGER PRIMARY KEY,
    uid            TEXT NOT NULL UNIQUE,
    label          TEXT NOT NULL DEFAULT '',
    fs_type        TEXT NOT NULL DEFAULT '',
    case_sensitive INTEGER NOT NULL DEFAULT 0,
    rotational     INTEGER,
    online         INTEGER NOT NULL DEFAULT 1
) STRICT;

CREATE TABLE root(
    id        INTEGER PRIMARY KEY,
    volume_id INTEGER NOT NULL REFERENCES volume(id),
    path      BLOB NOT NULL,
    display   TEXT NOT NULL,
    added     INTEGER NOT NULL,
    UNIQUE(volume_id, path)
) STRICT;

CREATE TABLE file(
    id         INTEGER PRIMARY KEY,
    root_id    INTEGER NOT NULL REFERENCES root(id) ON DELETE CASCADE,
    rel_path   BLOB NOT NULL,
    display    TEXT NOT NULL,
    name_key   TEXT NOT NULL,
    size       INTEGER NOT NULL,
    mtime_ns   INTEGER NOT NULL,
    btime_ns   INTEGER NOT NULL DEFAULT 0,
    fs_id      BLOB,
    quick_fp   BLOB,
    blake3     BLOB,
    kind       INTEGER NOT NULL,
    ftype      TEXT,
    taken      TEXT,
    taken_off  INTEGER,
    taken_src  TEXT,
    taken_conf INTEGER,
    status     TEXT NOT NULL DEFAULT 'present'
               CHECK(status IN ('present', 'missing', 'ignored')),
    last_seen  INTEGER NOT NULL,
    meta_ver   INTEGER NOT NULL DEFAULT 0,
    UNIQUE(root_id, rel_path)
) STRICT;

CREATE TABLE meta(
    file_id INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    key     TEXT NOT NULL,
    txt     TEXT,
    num     REAL,
    PRIMARY KEY(file_id, key)
) STRICT, WITHOUT ROWID;

CREATE INDEX root_volume ON root(volume_id);
CREATE INDEX file_blake3 ON file(blake3);
CREATE INDEX file_size_fp ON file(size, quick_fp);
CREATE INDEX file_fs_id ON file(fs_id);
CREATE INDEX meta_key_num ON meta(key, num);
CREATE INDEX meta_key_txt ON meta(key, txt);
)sql";

constexpr std::array<Migration, 1> kMigrations = {{
    {.version = 1, .sql = kSchemaV1},
}};

static_assert(kMigrations.back().version == kSchemaVersion);

Result<void> apply(Database& db, const Migration& m) {
    auto tx = Transaction::begin(db);
    if (!tx) {
        return tx.error();
    }
    if (auto done = db.exec(m.sql); !done) {
        return done;
    }
    // PRAGMAs take no parameters; the values are integers from this file.
    const std::string pragmas = "PRAGMA application_id = " + std::to_string(kApplicationId) +
                                "; PRAGMA user_version = " + std::to_string(m.version) + ";";
    if (auto done = db.exec(pragmas); !done) {
        return done;
    }
    return tx->commit();
}

} // namespace

std::span<const Migration> migrations() noexcept {
    return kMigrations;
}

Result<MigrationResult> migrate(Database& db) {
    const auto version = db.query_int("PRAGMA user_version");
    if (!version) {
        return version.error();
    }
    const auto app_id = db.query_int("PRAGMA application_id");
    if (!app_id) {
        return app_id.error();
    }
    MigrationResult result{.from = static_cast<int>(*version), .to = static_cast<int>(*version)};
    if (*version > kSchemaVersion) {
        return Error{.code = make_error_code(CatalogErrc::newer_schema),
                     .path = db.path(),
                     .detail = "schema " + std::to_string(*version) + ", this build reads up to " +
                               std::to_string(kSchemaVersion)};
    }
    if (*version == kSchemaVersion) {
        return result;
    }
    if (*version == 0) {
        // A new catalog is empty; anything else is someone else's database.
        const auto objects = db.query_int("SELECT count(*) FROM sqlite_schema");
        if (!objects) {
            return objects.error();
        }
        if (*objects != 0 || (*app_id != 0 && *app_id != kApplicationId)) {
            return Error{.code = make_error_code(CatalogErrc::not_a_catalog), .path = db.path()};
        }
    } else if (*app_id != kApplicationId) {
        return Error{.code = make_error_code(CatalogErrc::not_a_catalog), .path = db.path()};
    }

    auto backup = backup_catalog(db, result.from);
    if (!backup) {
        return backup.error();
    }
    result.backup = std::move(*backup);
    for (const Migration& m : kMigrations) {
        if (m.version <= result.to) {
            continue;
        }
        if (auto done = apply(db, m); !done) {
            return done.error();
        }
        result.to = m.version;
    }
    return result;
}

} // namespace sheepdog::catalog
