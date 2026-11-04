#pragma once

// The catalog schema and its migrations. PRAGMA user_version holds the schema version;
// PRAGMA application_id marks the file as a Sheepdog catalog. Each migration runs in its own
// transaction, and a backup (sqlite3_backup) is made before the first one runs.

#include "core/catalog/sqlite.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace sheepdog::catalog {

// The schema version this build writes and reads.
inline constexpr int kSchemaVersion = 1;
// PRAGMA application_id of a Sheepdog catalog: "SHDG".
inline constexpr std::int64_t kApplicationId = 0x53484447;

struct Migration {
    int version;          // user_version after this migration
    std::string_view sql; // statements run inside one transaction
};

// All migrations, in order: migrations()[i].version == i + 1.
[[nodiscard]] std::span<const Migration> migrations() noexcept;

struct MigrationResult {
    int from = 0;
    int to = 0;
    // The backup made before migrating; empty when nothing was to do. ({} keeps GCC quiet.)
    NativePath backup{}; // NOLINT(readability-redundant-member-init)
};

// Brings `db` to kSchemaVersion. Refuses a newer schema (CatalogErrc::newer_schema) and a
// database Sheepdog did not create (CatalogErrc::not_a_catalog). Running it again on a current
// catalog does nothing and makes no backup.
[[nodiscard]] Result<MigrationResult> migrate(Database& db);

} // namespace sheepdog::catalog
