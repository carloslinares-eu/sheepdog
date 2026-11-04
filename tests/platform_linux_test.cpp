// Linux-only platform tests: names that are not valid UTF-8, symlinks, a second device.

#include "core/platform/fs.hpp"
#include "support/temp_tree.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace pf = sheepdog::platform;
using sheepdog::test::make_dir;
using sheepdog::test::TempDir;
using sheepdog::test::write_file;

TEST_CASE("names that are not valid UTF-8 round-trip through the codec and the listing",
          "[fs][linux]") {
    const TempDir tmp;
    const std::vector<std::string> names = {
        "latin1-K\xE4se.txt", // ISO-8859-1 'ä'
        "lone-\x80-continuation",      "\xFF\xFE",
        "wtf8-surrogate-\xED\xA0\x80", // valid WTF-8, invalid UTF-8
        "ok-K\xC3\xA4se.txt",
    };
    for (const auto& name : names) {
        write_file(tmp / name, name);
    }
    const auto entries = pf::list_dir(tmp.path());
    REQUIRE(entries.has_value());
    std::vector<std::string> listed;
    for (const auto& e : *entries) {
        listed.push_back(e.name);
        CAPTURE(pf::display_string(e.name));
        CHECK(e.details.kind == pf::EntryKind::file);
        CHECK(e.details.size == e.name.size());
        CHECK(pf::file_details(tmp / e.name)->size == e.name.size());
        CHECK(pf::file_identity(tmp / e.name).has_value());
        CHECK(pf::is_valid_utf8(pf::display_string(e.name)));
        auto reader = pf::FileReader::open(tmp / e.name);
        REQUIRE(reader.has_value());
        std::vector<std::byte> buf(256);
        const auto n = reader->read(buf);
        REQUIRE(n.has_value());
        CHECK(std::string(reinterpret_cast<const char*>(buf.data()), *n) == e.name);
    }
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    CHECK(listed == sorted);
    CHECK_FALSE(pf::is_valid_utf8(names[0]));
    CHECK(pf::display_string(names[0]) == "latin1-K\xEF\xBF\xBDse.txt");
}

TEST_CASE("symlinks to files are links and are not opened through", "[fs][linux]") {
    const TempDir tmp;
    write_file(tmp / "real.txt", "data");
    sheepdog::test::make_dir_link(tmp / "file-link", tmp / "real.txt");
    sheepdog::test::make_dir_link(tmp / "dangling", tmp / "missing");
    const auto entries = pf::list_dir(tmp.path());
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 3);
    for (const auto& e : *entries) {
        CHECK((e.details.kind == pf::EntryKind::link) == (e.name != "real.txt"));
    }
    CHECK_FALSE(pf::FileReader::open(tmp / "file-link").has_value());
    CHECK(pf::file_details(tmp / "dangling")->kind == pf::EntryKind::link);
}

TEST_CASE("embedded NUL bytes are refused, not truncated", "[fs][linux]") {
    const TempDir tmp;
    write_file(tmp / "a", "x");
    const pf::NativePath sneaky(tmp.path().bytes() + "/a" + std::string(1, '\0') + "b");
    CHECK(pf::file_details(sneaky).error().code == std::errc::invalid_argument);
    CHECK_FALSE(pf::file_identity(sneaky).has_value());
    CHECK_FALSE(pf::FileReader::open(sneaky).has_value());
}

TEST_CASE("a second device has another volume and the same case rules", "[fs][linux]") {
    struct stat st{};
    if (::stat("/mnt/xdev", &st) != 0) {
        SKIP("no /mnt/xdev tmpfs (only in the container)");
    }
    const TempDir tmp;
    const auto here = pf::volume_info(tmp.path());
    const auto there = pf::volume_info(pf::NativePath("/mnt/xdev"));
    REQUIRE(here.has_value());
    REQUIRE(there.has_value());
    CHECK(there->fs_type == "tmpfs");
    CHECK(there->mount_root.bytes() == "/mnt/xdev");
    CHECK(there->uid != here->uid);
    CHECK(there->media == pf::Media::unknown);
}
