// The DB-writer thread and read-only connections.
#include "core/catalog/catalog.hpp"
#include "core/catalog/schema.hpp"
#include "core/catalog/writer.hpp"
#include "support/temp_tree.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace cat = sheepdog::catalog;
using sheepdog::test::TempDir;

namespace {

constexpr const char* kVolumeAndRoot =
    "INSERT INTO volume(id, uid) VALUES(1, 'test:1');"
    "INSERT INTO root(id, volume_id, path, display, added) VALUES(1, 1, x'2f74', '/t', 0);";

cat::FileRow row(int i, std::uint64_t size = 100) {
    cat::FileRow r;
    r.root_id = 1;
    r.rel_path = "dir/file-" + std::to_string(i) + ".jpg";
    r.display = r.rel_path;
    r.name_key = "file-" + std::to_string(i) + ".jpg";
    r.size = size;
    r.mtime_ns = 1'700'000'000'000'000'000 + i;
    r.fs_id = std::string(16, static_cast<char>(i & 0x7f));
    r.last_seen = 1;
    return r;
}

std::int64_t count(cat::Database& db, const std::string& sql) {
    const auto n = db.query_int(sql);
    REQUIRE(n.has_value());
    return *n;
}

cat::CatalogWriter::Op sql_op(std::string sql) {
    return [sql = std::move(sql)](cat::Database& db) { return db.exec(sql); };
}

} // namespace

TEST_CASE("the writer inserts and updates file rows", "[unit][fs]") {
    const TempDir tmp;
    const auto path = tmp / "catalog.db";
    auto db = cat::open_catalog(path);
    REQUIRE(db.has_value());
    cat::CatalogWriter writer(std::move(*db));
    writer.execute(sql_op(kVolumeAndRoot));
    for (int i = 0; i < 3; ++i) {
        writer.upsert_file(row(i));
    }
    REQUIRE(writer.flush().has_value());

    auto reader = cat::open_catalog_read_only(path);
    REQUIRE(reader.has_value());
    CHECK(count(*reader, "SELECT count(*) FROM file") == 3);
    CHECK(count(*reader, "SELECT count(*) FROM file WHERE status = 'present' AND"
                         " length(fs_id) = 16") == 3);

    // A hash survives a rescan with the same size and mtime, and is dropped when they change.
    writer.execute(sql_op("UPDATE file SET blake3 = x'01', quick_fp = x'02'"));
    writer.upsert_file(row(0));
    writer.upsert_file(row(1, 999));
    REQUIRE(writer.flush().has_value());
    CHECK(count(*reader, "SELECT count(*) FROM file") == 3);
    CHECK(count(*reader, "SELECT count(*) FROM file WHERE blake3 IS NOT NULL") == 2);
    CHECK(count(*reader, "SELECT size FROM file WHERE display = 'dir/file-1.jpg'") == 999);
    CHECK(count(*reader, "SELECT count(*) FROM file WHERE display = 'dir/file-1.jpg' AND"
                         " blake3 IS NULL AND quick_fp IS NULL") == 1);
    CHECK(writer.stats().failed == 0);

    // A single row is committed after max_delay even without a flush.
    const auto before = writer.stats().committed;
    writer.upsert_file(row(7));
    for (int i = 0; i < 500 && writer.stats().committed == before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(writer.stats().committed == before + 1);
    CHECK(count(*reader, "SELECT count(*) FROM file") == 4);
}

TEST_CASE("a failed write is reported by flush and rolls its batch back", "[unit][fs]") {
    const TempDir tmp;
    auto db = cat::open_catalog(tmp / "catalog.db");
    REQUIRE(db.has_value());
    cat::CatalogWriter writer(std::move(*db));
    writer.execute(sql_op(kVolumeAndRoot));
    REQUIRE(writer.flush().has_value());

    writer.execute(sql_op("INSERT INTO nowhere VALUES(1)"));
    const auto failed = writer.flush();
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().detail.find("nowhere") != std::string::npos);
    CHECK(writer.stats().failed == 1);

    // A row for a root that does not exist breaks the foreign key.
    cat::FileRow orphan = row(1);
    orphan.root_id = 42;
    writer.upsert_file(orphan);
    CHECK_FALSE(writer.flush().has_value());

    // The writer keeps going, and the error is not reported twice.
    writer.upsert_file(row(2));
    REQUIRE(writer.flush().has_value());
    writer.execute([](cat::Database& d) -> cat::Result<void> {
        const auto n = d.query_int("SELECT count(*) FROM file");
        if (!n) {
            return n.error();
        }
        return *n == 1 ? cat::Result<void>(cat::Ok{})
                       : cat::Result<void>(cat::Error{.code = {}, .path = {}, .detail = "count"});
    });
    CHECK(writer.flush().has_value());
}

TEST_CASE("several producers share one writer", "[unit][fs]") {
    const TempDir tmp;
    const auto path = tmp / "catalog.db";
    {
        auto db = cat::open_catalog(path);
        REQUIRE(db.has_value());
        cat::CatalogWriter writer(std::move(*db), {.batch_size = 100, .queue_capacity = 300});
        writer.execute(sql_op(kVolumeAndRoot));
        std::vector<std::jthread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&writer, t] {
                for (int i = 0; i < 2500; ++i) {
                    writer.upsert_file(row((t * 2500) + i));
                }
            });
        }
        producers.clear(); // joins
        // No flush: the destructor commits what is still queued.
    }
    auto reader = cat::open_catalog_read_only(path);
    REQUIRE(reader.has_value());
    CHECK(count(*reader, "SELECT count(*) FROM file") == 10'000);
}

TEST_CASE("100k file rows go through the writer in under 2 s", "[fs]") {
    const TempDir tmp;
    const auto path = tmp / "catalog.db";
    auto db = cat::open_catalog(path);
    REQUIRE(db.has_value());
    constexpr int kRows = 100'000;
    std::vector<cat::FileRow> rows;
    rows.reserve(kRows);
    for (int i = 0; i < kRows; ++i) {
        rows.push_back(row(i));
    }

    cat::CatalogWriter writer(std::move(*db));
    writer.execute(sql_op(kVolumeAndRoot));
    REQUIRE(writer.flush().has_value());
    const auto start = std::chrono::steady_clock::now();
    for (cat::FileRow& r : rows) {
        writer.upsert_file(std::move(r));
    }
    REQUIRE(writer.flush().has_value());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const auto stats = writer.stats();
    CHECK(stats.committed == kRows + 1);
    CHECK(stats.transactions >= kRows / 1000);
    CHECK(stats.transactions < kRows / 100); // batched, not committed row by row

    auto reader = cat::open_catalog_read_only(path);
    REQUIRE(reader.has_value());
    CHECK(count(*reader, "SELECT count(*) FROM file") == kRows);
#if defined(NDEBUG) && !defined(SHEEPDOG_SANITIZED)
    INFO("100k rows took " << ms << " ms");
    CHECK(ms < 2000);
#else
    WARN("100k rows took " << ms
                           << " ms (Debug or sanitized; the 2 s limit is checked in Release)");
#endif
}

TEST_CASE("read-only connections cannot write and need a migrated catalog", "[unit][fs]") {
    const TempDir tmp;
    const auto path = tmp / "catalog.db";
    CHECK_FALSE(cat::open_catalog_read_only(path).has_value()); // does not exist
    {
        auto db = cat::open_catalog(path);
        REQUIRE(db.has_value());
    }
    auto reader = cat::open_catalog_read_only(path);
    REQUIRE(reader.has_value());
    CHECK(count(*reader, "SELECT count(*) FROM file") == 0);
    const auto write = reader->exec("INSERT INTO volume(uid) VALUES('x')");
    CHECK_FALSE(write.has_value());

    // An empty file at version 0 has not been migrated yet. (Tables at version 0 would be a
    // foreign database: migrations set application_id together with the tables.)
    {
        auto raw = cat::Database::open(tmp / "old.db", cat::OpenMode::create);
        REQUIRE(raw.has_value());
        REQUIRE(raw->exec("PRAGMA application_id = 1397244999;"));
    }
    const auto old = cat::open_catalog_read_only(tmp / "old.db");
    REQUIRE_FALSE(old.has_value());
    CHECK(old.error().code == cat::CatalogErrc::not_migrated);
}
