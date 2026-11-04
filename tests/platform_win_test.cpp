// Windows-only platform tests: unpaired surrogates, paths over 260 characters, junctions.

#include "core/platform/fs.hpp"
#include "support/temp_tree.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace pf = sheepdog::platform;
using sheepdog::test::make_dir;
using sheepdog::test::TempDir;
using sheepdog::test::write_file;

namespace {

std::string read_all(const pf::NativePath& path) {
    auto reader = pf::FileReader::open(path);
    REQUIRE(reader.has_value());
    std::string out;
    std::vector<std::byte> buf(4096);
    for (;;) {
        auto n = reader->read(buf);
        REQUIRE(n.has_value());
        if (*n == 0) {
            return out;
        }
        out.append(reinterpret_cast<const char*>(buf.data()), *n);
    }
}

} // namespace

TEST_CASE("names with unpaired surrogates round-trip through the codec and the listing",
          "[fs][win]") {
    const TempDir tmp;
    const std::u16string names16[] = {
        std::u16string{u'a', u'\xD800', u'.', u't', u'x', u't'}, // lone lead surrogate
        std::u16string{u'\xDC01', u'z'},                         // lone trail surrogate
        std::u16string{u'\xDE00', u'\xD83D'},                    // reversed pair: two lone ones
        u"\U0001F600 pair.txt",                                  // a proper pair, for contrast
    };
    std::vector<std::string> expected;
    for (const auto& n16 : names16) {
        const std::string native = pf::wtf8_from_utf16(n16);
        expected.push_back(native);
        write_file(tmp / native, "content of " + pf::display_string(native));
    }
    const auto entries = pf::list_dir(tmp.path());
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == expected.size());
    for (const auto& entry : *entries) {
        CAPTURE(pf::display_string(entry.name));
        // The listed bytes are exactly what we created, and decode back to the same UTF-16.
        CHECK(std::find(expected.begin(), expected.end(), entry.name) != expected.end());
        const auto u16 = pf::utf16_from_wtf8(entry.name);
        REQUIRE(u16.has_value());
        CHECK(pf::wtf8_from_utf16(*u16) == entry.name);
        // And the listed name reaches the file again.
        const auto path = tmp / entry.name;
        CHECK(pf::file_details(path)->size == entry.details.size);
        CHECK(pf::file_identity(path).has_value());
        CHECK(read_all(path) == "content of " + pf::display_string(entry.name));
        CHECK(pf::is_valid_utf8(pf::display_string(entry.name)));
    }
}

TEST_CASE("a path longer than 260 characters can be listed and read", "[fs][win]") {
    const TempDir tmp;
    const std::string segment(60, 'd');
    pf::NativePath deep = tmp.path();
    for (int i = 0; i < 5; ++i) {
        deep = deep / (segment + std::to_string(i));
        make_dir(deep);
    }
    const pf::NativePath file = deep / (std::string(80, 'f') + ".txt");
    write_file(file, "far away");
    REQUIRE(file.bytes().size() > 400);
    REQUIRE(pf::win32_path(file)->size() > 400);

    const auto entries = pf::list_dir(deep);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    CHECK((*entries)[0].name == file.filename());
    CHECK((*entries)[0].details.size == 8);

    CHECK(pf::file_details(file)->size == 8);
    CHECK(pf::file_identity(file).has_value());
    CHECK(read_all(file) == "far away");
    CHECK(pf::volume_info(deep).has_value());
}

TEST_CASE("a junction is a link: listed, not followed, with a reparse attribute", "[fs][win]") {
    const TempDir tmp;
    make_dir(tmp / "target");
    write_file(tmp / "target" / "inside.txt", "x");
    sheepdog::test::make_dir_link(tmp / "junction", tmp / "target");

    const auto details = pf::file_details(tmp / "junction");
    REQUIRE(details.has_value());
    CHECK(details->kind == pf::EntryKind::link);
    CHECK((details->attributes & 0x400U) != 0U); // FILE_ATTRIBUTE_REPARSE_POINT
    CHECK_FALSE(pf::list_dir(tmp / "junction").has_value());
    // Reading the target through the junction is still possible for the OS; Sheepdog just
    // never lists through it.
    CHECK(pf::file_details(tmp / "junction" / "inside.txt").has_value());
}

TEST_CASE("relative paths and drive roots work", "[fs][win]") {
    const auto root = pf::list_dir(pf::NativePath("C:\\"));
    REQUIRE(root.has_value());
    CHECK_FALSE(root->empty());
    const auto here = pf::file_details(pf::NativePath("."));
    REQUIRE(here.has_value());
    CHECK(here->kind == pf::EntryKind::directory);
    const auto vol = pf::volume_info(pf::NativePath("C:\\"));
    REQUIRE(vol.has_value());
    CHECK(vol->mount_root.bytes() == "C:\\");
    CHECK(vol->media != pf::Media::unknown);
}
