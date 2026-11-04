// Smoke tests that the vcpkg dependencies are usable on every triplet.
#include <blake3.h>
#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace {

std::string to_hex(const std::array<std::uint8_t, BLAKE3_OUT_LEN>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}

} // namespace

TEST_CASE("BLAKE3 hashes the empty input to the reference value", "[unit]") {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::array<std::uint8_t, BLAKE3_OUT_LEN> out{};
    blake3_hasher_finalize(&hasher, out.data(), out.size());
    CHECK(to_hex(out) == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST_CASE("SQLite is version 3 and has FTS5", "[unit]") {
    CHECK(sqlite3_libversion_number() >= 3'040'000);
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    char* error = nullptr;
    const int rc = sqlite3_exec(db,
        "CREATE VIRTUAL TABLE names USING fts5(name);"
        "INSERT INTO names VALUES ('holiday photo');",
        nullptr, nullptr, &error);
    const std::string message = error != nullptr ? error : "";
    sqlite3_free(error);
    sqlite3_close(db);
    INFO(message);
    CHECK(rc == SQLITE_OK);
}
