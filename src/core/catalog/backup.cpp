#include "core/catalog/backup.hpp"

#include "core/platform/fs.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>

namespace sheepdog::catalog {

namespace {

constexpr std::string_view kTag = ".bak-";
constexpr std::size_t kStampSize = 19; // 20261104T201500123Z

void append_digits(std::string& out, std::int64_t value, int width) {
    std::array<char, 8> buf{};
    for (int i = width - 1; i >= 0; --i) {
        buf[static_cast<std::size_t>(i)] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    out.append(buf.data(), static_cast<std::size_t>(width));
}

NativePath folder_of(const NativePath& catalog) {
    const NativePath parent = catalog.parent();
    return parent.empty() ? NativePath(".") : parent;
}

NativePath sibling(const NativePath& catalog, std::string_view name) {
    const NativePath parent = catalog.parent();
    return parent.empty() ? NativePath(std::string(name)) : parent / name;
}

// "<catalog>.bak-<stamp>-v<digits>", nothing else (not the -wal/-shm files SQLite may leave).
bool is_backup_name(std::string_view name, std::string_view catalog_name) {
    if (!name.starts_with(catalog_name)) {
        return false;
    }
    name.remove_prefix(catalog_name.size());
    if (!name.starts_with(kTag)) {
        return false;
    }
    name.remove_prefix(kTag.size());
    if (name.size() < kStampSize + 3 || name[kStampSize - 1] != 'Z') {
        return false;
    }
    name.remove_prefix(kStampSize);
    if (!name.starts_with("-v")) {
        return false;
    }
    name.remove_prefix(2);
    return std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

std::string backup_stamp(std::int64_t unix_ms) {
    // Civil date from days since 1970 (Howard Hinnant's algorithm); no time zones involved.
    constexpr std::int64_t kMsPerDay = 86'400'000;
    std::int64_t days = unix_ms / kMsPerDay;
    std::int64_t ms_of_day = unix_ms % kMsPerDay;
    if (ms_of_day < 0) {
        ms_of_day += kMsPerDay;
        --days;
    }
    const std::int64_t z = days + 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const std::int64_t doe = z - (era * 146'097);
    const std::int64_t yoe = (doe - (doe / 1460) + (doe / 36'524) - (doe / 146'096)) / 365;
    const std::int64_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
    const std::int64_t mp = ((5 * doy) + 2) / 153;
    const std::int64_t day = doy - (((153 * mp) + 2) / 5) + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yoe + (era * 400) + (month <= 2 ? 1 : 0);

    std::string out;
    out.reserve(kStampSize);
    append_digits(out, year, 4);
    append_digits(out, month, 2);
    append_digits(out, day, 2);
    out += 'T';
    append_digits(out, ms_of_day / 3'600'000, 2);
    append_digits(out, (ms_of_day / 60'000) % 60, 2);
    append_digits(out, (ms_of_day / 1000) % 60, 2);
    append_digits(out, ms_of_day % 1000, 3);
    out += 'Z';
    return out;
}

std::vector<NativePath> list_backups(const NativePath& catalog) {
    std::vector<NativePath> out;
    const auto entries = platform::list_dir(folder_of(catalog));
    if (!entries) {
        return out;
    }
    const std::string_view catalog_name = catalog.filename();
    for (const platform::DirEntry& entry : *entries) {
        if (!entry.error && entry.details.kind == platform::EntryKind::file &&
            is_backup_name(entry.name, catalog_name)) {
            out.push_back(sibling(catalog, entry.name));
        }
    }
    // list_dir sorts by bytes, and the fixed-width stamps make that oldest first.
    return out;
}

std::optional<NativePath> newest_backup(const NativePath& catalog) {
    auto all = list_backups(catalog);
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.back());
}

Result<NativePath> backup_catalog(Database& db, int schema_version) {
    const NativePath& catalog = db.path();
    std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    // Never overwrite: step the stamp forward until the name is free.
    NativePath target;
    for (int attempt = 0;; ++attempt, ++ms) {
        target = sibling(catalog, std::string(catalog.filename()) + std::string(kTag) +
                                      backup_stamp(ms) + "-v" + std::to_string(schema_version));
        const auto existing = platform::file_details(target);
        if (!existing && platform::is_not_found(existing.error().code)) {
            break;
        }
        if (attempt == 1000) {
            return Error{.code = std::make_error_code(std::errc::file_exists), .path = target};
        }
    }

    {
        auto dest = Database::open(target, OpenMode::create);
        if (!dest) {
            return dest.error();
        }
        sqlite3_backup* copy = sqlite3_backup_init(dest->handle(), "main", db.handle(), "main");
        if (copy == nullptr) {
            return dest->error(sqlite3_extended_errcode(dest->handle()));
        }
        const int rc = sqlite3_backup_step(copy, -1);
        sqlite3_backup_finish(copy);
        if (rc != SQLITE_DONE) {
            return dest->error(rc);
        }
    }

    // Keep the newest kKeepBackups. A backup that cannot be deleted stays; that is harmless.
    const auto all = list_backups(catalog);
    for (std::size_t i = 0; i + kKeepBackups < all.size(); ++i) {
        static_cast<void>(platform::remove_app_file(all[i]));
    }
    return target;
}

} // namespace sheepdog::catalog
