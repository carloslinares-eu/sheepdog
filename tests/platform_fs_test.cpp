// The read side of the platform layer against real temp folders, on both OSes.

#include "core/platform/fs.hpp"
#include "support/temp_tree.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pf = sheepdog::platform;
using sheepdog::test::make_dir;
using sheepdog::test::make_dir_link;
using sheepdog::test::rename_path;
using sheepdog::test::TempDir;
using sheepdog::test::write_file;

namespace {

std::vector<std::string> names_of(const std::vector<pf::DirEntry>& entries) {
    std::vector<std::string> names;
    for (const auto& e : entries) {
        names.push_back(e.name);
    }
    return names;
}

std::string read_all(const pf::NativePath& path) {
    auto reader = pf::FileReader::open(path);
    REQUIRE(reader.has_value());
    std::string out;
    std::vector<std::byte> buf(3); // small on purpose: several reads per file
    for (;;) {
        auto n = reader->read(buf);
        REQUIRE(n.has_value());
        if (*n == 0) {
            break;
        }
        out.append(reinterpret_cast<const char*>(buf.data()), *n);
    }
    return out;
}

constexpr std::int64_t kYear2020Ns = 1'577'836'800LL * 1'000'000'000LL;

} // namespace

TEST_CASE("list_dir returns sorted entries with their details", "[fs]") {
    const TempDir tmp;
    write_file(tmp / "b.txt", "12345");
    write_file(tmp / "a.txt", "");
    make_dir(tmp / "c");
    write_file(tmp / "Z.txt", "x");

    const auto entries = pf::list_dir(tmp.path());
    REQUIRE(entries.has_value());
    CHECK(names_of(*entries) == std::vector<std::string>{"Z.txt", "a.txt", "b.txt", "c"});
    const auto& b = (*entries)[2];
    CHECK(b.details.kind == pf::EntryKind::file);
    CHECK(b.details.size == 5);
    CHECK(b.details.mtime_ns > kYear2020Ns);
    CHECK_FALSE(b.details.cloud_only);
    CHECK_FALSE(b.error);
    CHECK((*entries)[3].details.kind == pf::EntryKind::directory);
    CHECK((*entries)[3].details.size == 0);

    const auto single = pf::file_details(tmp / "b.txt");
    REQUIRE(single.has_value());
    CHECK(single->kind == pf::EntryKind::file);
    CHECK(single->size == 5);
    CHECK(single->mtime_ns == b.details.mtime_ns);
    CHECK(single->attributes == b.details.attributes);
}

TEST_CASE("list_dir and file_details report missing paths", "[fs]") {
    const TempDir tmp;
    const auto listing = pf::list_dir(tmp / "nope");
    REQUIRE_FALSE(listing.has_value());
    CHECK(pf::is_not_found(listing.error().code));
    CHECK(listing.error().path == tmp / "nope");
    CHECK_FALSE(listing.error().message().empty());

    const auto details = pf::file_details(tmp / "nope");
    REQUIRE_FALSE(details.has_value());
    CHECK(pf::is_not_found(details.error().code));

    write_file(tmp / "f", "x");
    CHECK_FALSE(pf::list_dir(tmp / "f").has_value());
    CHECK_FALSE(pf::FileReader::open(tmp / "nope").has_value());
}

TEST_CASE("a file keeps its identity across a rename", "[fs]") {
    const TempDir tmp;
    write_file(tmp / "before.txt", "same content");
    write_file(tmp / "other.txt", "same content");
    make_dir(tmp / "sub");

    const auto before = pf::file_identity(tmp / "before.txt");
    const auto other = pf::file_identity(tmp / "other.txt");
    REQUIRE(before.has_value());
    REQUIRE(other.has_value());
    CHECK_FALSE(*before == *other);
    CHECK(before->volume == other->volume);

    rename_path(tmp / "before.txt", tmp / "sub" / "after.txt");
    const auto after = pf::file_identity(tmp / "sub" / "after.txt");
    REQUIRE(after.has_value());
    CHECK(*after == *before);
    CHECK(pf::is_not_found(pf::file_identity(tmp / "before.txt").error().code));
}

TEST_CASE("a folder link loop is reported as a link and never followed", "[fs]") {
    const TempDir tmp;
    make_dir(tmp / "top");
    write_file(tmp / "top" / "file.txt", "x");
    make_dir_link(tmp / "top" / "loop", tmp / "top"); // top/loop -> top

    const auto entries = pf::list_dir(tmp / "top");
    REQUIRE(entries.has_value());
    REQUIRE(names_of(*entries) == std::vector<std::string>{"file.txt", "loop"});
    CHECK((*entries)[1].details.kind == pf::EntryKind::link);
    CHECK(pf::file_details(tmp / "top" / "loop")->kind == pf::EntryKind::link);
    // One rule on both OSes: a link has size 0 (Linux would otherwise report st_size).
    CHECK((*entries)[1].details.size == 0);
    CHECK(pf::file_details(tmp / "top" / "loop")->size == 0);

    // Listing the link itself is refused rather than followed.
    const auto through = pf::list_dir(tmp / "top" / "loop");
    REQUIRE_FALSE(through.has_value());
    CHECK(through.error().code == std::errc::too_many_symbolic_link_levels);

    // The link has its own identity, not that of its target.
    CHECK_FALSE(*pf::file_identity(tmp / "top" / "loop") == *pf::file_identity(tmp / "top"));

    // A walk that descends only into folders terminates and sees each entry once.
    std::size_t visited = 0;
    std::function<void(const pf::NativePath&, int)> walk = [&](const pf::NativePath& dir,
                                                               int depth) {
        REQUIRE(depth < 4);
        const auto list = pf::list_dir(dir);
        REQUIRE(list.has_value());
        for (const auto& e : *list) {
            ++visited;
            if (e.details.kind == pf::EntryKind::directory) {
                walk(dir / e.name, depth + 1);
            }
        }
    };
    walk(tmp.path(), 0);
    CHECK(visited == 3); // top, top/file.txt, top/loop
}

TEST_CASE("FileReader reads a file's contents", "[fs]") {
    const TempDir tmp;
    write_file(tmp / "data.bin", std::string("hello\0world", 11));
    CHECK(read_all(tmp / "data.bin") == std::string("hello\0world", 11));
    write_file(tmp / "empty", "");
    CHECK(read_all(tmp / "empty").empty());

    auto reader = pf::FileReader::open(tmp / "data.bin");
    REQUIRE(reader.has_value());
    pf::FileReader moved = std::move(reader).value();
    std::vector<std::byte> buf(64);
    CHECK(*moved.read(buf) == 11);
    CHECK(moved.path() == tmp / "data.bin");
}

TEST_CASE("volume_info describes the temp folder's volume", "[fs]") {
    const TempDir tmp;
    write_file(tmp / "Probe.txt", "x");
    const auto info = pf::volume_info(tmp.path());
    REQUIRE(info.has_value());
    CHECK_FALSE(info->uid.empty());
    CHECK_FALSE(info->fs_type.empty());
    CHECK_FALSE(info->mount_root.empty());
    CHECK_FALSE(info->remote);
    CHECK(pf::volume_info(tmp / "Probe.txt").has_value());
    CAPTURE(info->uid, info->fs_type, display_string(info->mount_root), info->label,
            static_cast<int>(info->media));

    // The uid stays the same for another folder on the same volume.
    make_dir(tmp / "sub");
    CHECK(pf::volume_info(tmp / "sub")->uid == info->uid);

    const auto probe = pf::probe_case_sensitive(tmp.path());
    REQUIRE(probe.has_value());
    CHECK(*probe == info->case_sensitive);
#ifdef _WIN32
    CHECK_FALSE(info->case_sensitive); // the temp folder is on NTFS
    CHECK(info->uid.rfind("win:", 0) == 0);
    CHECK(info->uid.size() == 20);
    // A file's FileId.volume is the serial in the uid.
    CHECK(info->uid == "win:" + [&] {
        std::string hex(16, '0');
        auto v = pf::file_identity(tmp / "Probe.txt")->volume;
        for (std::size_t i = 16; i > 0; --i, v >>= 4U) {
            hex[i - 1] = "0123456789abcdef"[v & 0xFU];
        }
        return hex;
    }());
#else
    CHECK(info->case_sensitive);
#endif
}

TEST_CASE("the case probe finds nothing to probe in a folder without letters", "[fs]") {
    const TempDir tmp;
    make_dir(tmp / "123");
    write_file(tmp / "123" / "456", "");
    // "123" has no letters, its entry neither: nothing to learn here.
    CHECK_FALSE(pf::probe_case_sensitive(tmp / "123").has_value());
}
