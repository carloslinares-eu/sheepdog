#pragma once

// Catalog backups made with sqlite3_backup, kept next to the catalog as
// "<catalog file name>.bak-<YYYYMMDDTHHMMSSmmm>Z-v<schema>" (UTC). The fixed-width stamp makes
// the names sort oldest first. Only the newest kKeepBackups are kept.

#include "core/catalog/sqlite.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sheepdog::catalog {

inline constexpr std::size_t kKeepBackups = 10;

// "20261104T201500123Z" for a time in milliseconds since 1970 UTC.
[[nodiscard]] std::string backup_stamp(std::int64_t unix_ms);

// Copies the whole database to a new backup file (never overwriting one), then deletes the
// oldest backups beyond kKeepBackups. `schema_version` goes into the name.
[[nodiscard]] Result<NativePath> backup_catalog(Database& db, int schema_version);

// The backups of `catalog`, oldest first.
[[nodiscard]] std::vector<NativePath> list_backups(const NativePath& catalog);
[[nodiscard]] std::optional<NativePath> newest_backup(const NativePath& catalog);

} // namespace sheepdog::catalog
