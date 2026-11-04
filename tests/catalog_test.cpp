// Catalog: migrations, backups, integrity checks and network-location refusal.
#include "core/catalog/backup.hpp"
#include "core/catalog/catalog.hpp"
#include "core/catalog/schema.hpp"
#include "core/platform/fs.hpp"
#include "support/temp_tree.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <string>

namespace cat = sheepdog::catalog;
namespace pf = sheepdog::platform;
using sheepdog::test::remove_tree;
using sheepdog::test::TempDir;
using sheepdog::test::write_file;

namespace {

std::string read_all(const pf::NativePath& path) {
    auto reader = pf::FileReader::open(path);
    REQUIRE(reader.has_value());
    std::string out;
    std::array<std::byte, 65536> buf{};
    for (;;) {
        const auto n = reader->read(buf);
        REQUIRE(n.has_value());
        if (*n == 0) {
            break;
        }
        out.append(reinterpret_cast<const char*>(buf.data()), *n);
    }
    return out;
}

std::int64_t count(cat::Database& db, const char* sql) {
    const auto n = db.query_int(sql);
    REQUIRE(n.has_value());
    return *n;
}

// A catalog with enough rows to fill a few dozen pages, closed so that the WAL is checkpointed.
void make_filled_catalog(const pf::NativePath& path) {
    auto db = cat::open_catalog(path);
    REQUIRE(db.has_value());
    REQUIRE(
        db->exec("INSERT INTO volume(id, uid) VALUES(1, 'test:1');"
                 "INSERT INTO root(id, volume_id, path, display, added)"
                 "  VALUES(1, 1, x'2f74', '/t', 0);"
                 "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000)"
                 "INSERT INTO file(root_id, rel_path, display, name_key, size, mtime_ns, kind,"
                 "                 last_seen)"
                 "  SELECT 1, CAST('file-' || i AS BLOB), 'file-' || i, 'file-' || i, i, i, 0, 0"
                 "  FROM n;")
            .has_value());
}

} // namespace

TEST_CASE("migration: an empty catalog goes to v1 once and leaves a backup", "[unit][fs]") {
    const TempDir tmp;
    const auto path = tmp / "catalog.db";
    {
        auto db = cat::open_catalog(path);
        REQUIRE(db.has_value());
        CHECK(count(*db, "PRAGMA user_version") == cat::kSchemaVersion);
        CHECK(count(*db, "PRAGMA application_id") == cat::kApplicationId);
        CHECK(count(*db, "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name IN"
                         " ('volume', 'root', 'file', 'meta')") == 4);
        CHECK(
            count(*db, "SELECT count(*) FROM sqlite_schema WHERE name IN ('tag', 'batch', 'op')") ==
            0);
        CHECK(*db->query_text("PRAGMA journal_mode") == "wal");
        CHECK(count(*db, "PRAGMA foreign_keys") == 1);

        // Running the migration again on the open catalog does nothing.
        const auto again = cat::migrate(*db);
        REQUIRE(again.has_value());
        CHECK(again->from == cat::kSchemaVersion);
        CHECK(again->to == cat::kSchemaVersion);
        CHECK(again->backup.empty());
    }
    const auto backups = cat::list_backups(path);
    REQUIRE(backups.size() == 1);
    CHECK(std::string(backups[0].filename()).ends_with("-v0"));

    // Reopening runs no migration and makes no further backup.
    {
        auto db = cat::open_catalog(path);
        REQUIRE(db.has_value());
        CHECK(count(*db, "PRAGMA user_version") == cat::kSchemaVersion);
    }
    CHECK(cat::list_backups(path).size() == 1);

    // The backup is the catalog as it was before migrating: a database at version 0.
    auto old = cat::Database::open(backups[0], cat::OpenMode::read_only);
    REQUIRE(old.has_value());
    CHECK(count(*old, "PRAGMA user_version") == 0);
}

TEST_CASE("migrations are numbered 1 to kSchemaVersion", "[unit]") {
    const auto all = cat::migrations();
    REQUIRE(all.size() == static_cast<std::size_t>(cat::kSchemaVersion));
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK(all[i].version == static_cast<int>(i) + 1);
    }
}

TEST_CASE("a catalog from a newer Sheepdog or a foreign database is refused", "[unit][fs]") {
    const TempDir tmp;
    {
        auto raw = cat::Database::open(tmp / "newer.db", cat::OpenMode::create);
        REQUIRE(raw.has_value());
        REQUIRE(raw->exec("PRAGMA application_id = 1397244999; PRAGMA user_version = 99;"));
    }
    const auto newer = cat::open_catalog(tmp / "newer.db");
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().code == cat::CatalogErrc::newer_schema);

    {
        auto raw = cat::Database::open(tmp / "other.db", cat::OpenMode::create);
        REQUIRE(raw.has_value());
        REQUIRE(raw->exec("CREATE TABLE notes(text TEXT);"));
    }
    const auto other = cat::open_catalog(tmp / "other.db");
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().code == cat::CatalogErrc::not_a_catalog);
    CHECK(cat::list_backups(tmp / "other.db").empty());
}

TEST_CASE("backup stamps are fixed-width UTC times", "[unit]") {
    CHECK(cat::backup_stamp(0) == "19700101T000000000Z");
    CHECK(cat::backup_stamp(1'793'823'300'123) == "20261104T201500123Z");
    CHECK(cat::backup_stamp(951'868'799'999) == "20000229T235959999Z");
    CHECK(cat::backup_stamp(-1) == "19691231T235959999Z");
}

TEST_CASE("backups never overwrite each other and only the newest 10 are kept", "[unit][fs]") {
    const TempDir tmp;
    const auto path = tmp / "catalog.db";
    auto db = cat::open_catalog(path);
    REQUIRE(db.has_value());
    write_file(tmp / "catalog.db.bak-notes.txt", "not a backup");

    std::set<pf::NativePath> made;
    for (int i = 0; i < 12; ++i) {
        const auto backup = cat::backup_catalog(*db, cat::kSchemaVersion);
        REQUIRE(backup.has_value());
        made.insert(*backup);
    }
    CHECK(made.size() == 12);
    const auto kept = cat::list_backups(path);
    REQUIRE(kept.size() == cat::kKeepBackups);
    CHECK(*cat::newest_backup(path) == *made.rbegin());
    CHECK(pf::file_details(tmp / "catalog.db.bak-notes.txt").has_value());
}

TEST_CASE("a file that is not a database is refused as damaged", "[unit][fs]") {
    const TempDir tmp;
    const auto path = tmp / "catalog.db";
    std::string garbage;
    while (garbage.size() < 8192) {
        garbage += "this is not an SQLite database. ";
    }
    write_file(path, garbage);
    const auto db = cat::open_catalog(path);
    REQUIRE_FALSE(db.has_value());
    CHECK(db.error().code == cat::CatalogErrc::corrupt);
    INFO(db.error().message());
    CHECK(db.error().message().find("the catalog is damaged") != std::string::npos);
    CHECK(db.error().message().find("No backup was found") != std::string::npos);
}

TEST_CASE("a corrupted catalog is refused with the newest backup offered", "[unit][fs]") {
    const TempDir tmp;
    const auto path = tmp / "catalog.db";
    make_filled_catalog(path);
    const auto backup = cat::newest_backup(path);
    REQUIRE(backup.has_value());

    // Overwrite pages 3 to 6 (of the 4 KiB pages) with junk; page 1 stays a valid header.
    std::string bytes = read_all(path);
    REQUIRE(bytes.size() > 7 * 4096);
    for (std::size_t i = 2 * 4096; i < 6 * 4096; ++i) {
        bytes[i] = static_cast<char>(0xA5);
    }
    remove_tree(path);
    write_file(path, bytes);

    const auto db = cat::open_catalog(path);
    REQUIRE_FALSE(db.has_value());
    CHECK(db.error().code == cat::CatalogErrc::corrupt);
    const std::string message = db.error().message();
    INFO(message);
    CHECK(message.find("the catalog is damaged") != std::string::npos);
    CHECK(message.find("The newest backup is " + pf::display_string(*backup)) != std::string::npos);
    // The damaged catalog was not migrated or backed up again.
    CHECK(cat::list_backups(path).size() == 1);
}

TEST_CASE("network locations are recognised", "[unit]") {
    CHECK(pf::is_unc_path(R"(\\server\share\catalog.db)"));
    CHECK(pf::is_unc_path("//server/share/catalog.db"));
    CHECK(pf::is_unc_path(R"(\\?\UNC\server\share\catalog.db)"));
    CHECK(pf::is_unc_path(R"(\\?\unc\server\share)"));
    CHECK_FALSE(pf::is_unc_path(R"(\\?\C:\Users\catalog.db)"));
    CHECK_FALSE(pf::is_unc_path(R"(\\.\PhysicalDrive0)"));
    CHECK_FALSE(pf::is_unc_path(R"(C:\Users\catalog.db)"));
    CHECK_FALSE(pf::is_unc_path("/home/me/catalog.db"));
    CHECK_FALSE(pf::is_unc_path(R"(\\\server)"));

    for (const char* fs : {"nfs", "nfs4", "cifs", "smb3", "fuse.sshfs", "9p"}) {
        INFO(fs);
        CHECK(pf::is_remote_fs_type(fs));
    }
    for (const char* fs : {"ext4", "btrfs", "xfs", "tmpfs", "overlay", "vfat", "NTFS", ""}) {
        INFO(fs);
        CHECK_FALSE(pf::is_remote_fs_type(fs));
    }
}

TEST_CASE("a catalog in a local temp folder passes the location check", "[unit][fs]") {
    const TempDir tmp;
    CHECK(cat::check_catalog_location(tmp / "catalog.db").has_value());
    CHECK(cat::check_catalog_location(tmp / "not-yet" / "catalog.db").has_value());
}

#ifdef _WIN32
TEST_CASE("a catalog on a UNC share is refused without touching the network", "[unit][win]") {
    for (const char* path :
         {R"(\\server.invalid\share\catalog.db)", R"(\\?\UNC\server.invalid\share\catalog.db)"}) {
        INFO(path);
        const auto db = cat::open_catalog(pf::NativePath(path));
        REQUIRE_FALSE(db.has_value());
        CHECK(db.error().code == cat::CatalogErrc::network_location);
        const auto ro = cat::open_catalog_read_only(pf::NativePath(path));
        REQUIRE_FALSE(ro.has_value());
        CHECK(ro.error().code == cat::CatalogErrc::network_location);
    }
}
#endif
