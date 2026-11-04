#pragma once

// Error codes of the catalog itself (as opposed to SQLite's result codes in sqlite_category()).

#include <cstdint>
#include <system_error>

namespace sheepdog::catalog {

enum class CatalogErrc : std::uint8_t {
    corrupt = 1,      // integrity_check failed or the file is not a database
    network_location, // the catalog path is on a network share (UNC, mapped drive, NFS, SMB)
    newer_schema,     // made by a newer Sheepdog: user_version above kSchemaVersion
    not_migrated,     // a read-only open found an older schema; open it read-write first
    not_a_catalog,    // an SQLite database that Sheepdog did not create
    wal_unavailable,  // the filesystem refused journal_mode=WAL
};

[[nodiscard]] const std::error_category& catalog_category() noexcept;
[[nodiscard]] std::error_code make_error_code(CatalogErrc code) noexcept;

} // namespace sheepdog::catalog

template <> struct std::is_error_code_enum<sheepdog::catalog::CatalogErrc> : std::true_type {};
