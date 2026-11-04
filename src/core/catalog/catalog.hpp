#pragma once

// Opening the catalog. open_catalog() is the one read-write entry point: it refuses network
// locations, runs integrity_check, sets WAL and the pragmas, and migrates (backing up first).
// open_catalog_read_only() gives the query connections used by readers.

#include "core/catalog/errors.hpp"
#include "core/catalog/sqlite.hpp"

namespace sheepdog::catalog {

// Fails with CatalogErrc::network_location when `catalog` lies on a network share (UNC path,
// mapped network drive, NFS, SMB, ...), where SQLite's file locking cannot be trusted.
[[nodiscard]] Result<void> check_catalog_location(const NativePath& catalog);

// PRAGMA integrity_check. On damage (or a file that is not a database) fails with
// CatalogErrc::corrupt; the detail names SQLite's finding and the newest backup to restore.
[[nodiscard]] Result<void> check_integrity(Database& db);

// The message part shown when a catalog is damaged: what to restore, or that no backup exists.
[[nodiscard]] std::string restore_advice(const NativePath& catalog);

// Opens (creating if missing) the catalog read-write, checked and migrated to kSchemaVersion.
// Pragmas: journal_mode=WAL, synchronous=NORMAL, foreign_keys=ON. The folder must exist.
[[nodiscard]] Result<Database> open_catalog(const NativePath& catalog);

// A read-only connection (query_only) to an existing catalog at kSchemaVersion; one that is
// older fails with CatalogErrc::not_migrated, a newer one with CatalogErrc::newer_schema.
[[nodiscard]] Result<Database> open_catalog_read_only(const NativePath& catalog);

} // namespace sheepdog::catalog
