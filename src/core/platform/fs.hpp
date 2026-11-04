#pragma once

// The read side of the platform layer: folder listings, file details, file identity, volume
// information and reading file contents. Nothing here changes a file (writes come with FsOps).
// Every call takes and returns NativePath bytes; on Windows each call adds the \\?\ prefix
// itself, so paths longer than 260 characters work.

#include "core/platform/native_path.hpp"
#include "core/platform/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sheepdog::platform {

enum class EntryKind : std::uint8_t {
    file,
    directory,
    // A symlink, a junction or another name-surrogate reparse point. Never followed.
    link,
    // Devices, FIFOs, sockets and anything else.
    other,
};

struct FileDetails {
    EntryKind kind = EntryKind::other;
    std::uint64_t size = 0;    // bytes; 0 for folders
    std::int64_t mtime_ns = 0; // last write, nanoseconds since 1970-01-01 UTC
    std::int64_t btime_ns = 0; // creation (birth) time, 0 when the filesystem has none
    // Native attributes: FILE_ATTRIBUTE_* on Windows, st_mode on Linux.
    std::uint32_t attributes = 0;
    // A cloud placeholder (OneDrive files-on-demand etc.) whose contents are not on disk.
    // Opening it would download it, so the scanner must not read it.
    bool cloud_only = false;
};

struct DirEntry {
    std::string name; // native bytes of the name alone
    FileDetails details;
    // Set when the details of this entry could not be read; details is then empty.
    std::error_code error;
};

// The entries of a folder, without "." and "..", sorted by their native bytes. Links are
// reported as EntryKind::link and never followed; a `dir` that is itself a link is refused
// with errc::too_many_symbolic_links_levels. Entries that vanish during the listing are left out.
[[nodiscard]] Result<std::vector<DirEntry>> list_dir(const NativePath& dir);

// Details of one path. A link is described itself, not its target.
[[nodiscard]] Result<FileDetails> file_details(const NativePath& path);

// What identifies a file on its volume while it exists: it stays the same across renames and
// moves within the volume.
struct FileId {
    // The volume as this boot sees it: the 64-bit volume serial on Windows, st_dev on Linux.
    // Persistent references use VolumeInfo::uid instead.
    std::uint64_t volume = 0;
    // FILE_ID_INFO's 128-bit FileId on Windows; st_ino (little-endian, zero-padded) on Linux.
    std::array<std::uint8_t, 16> file{};

    friend bool operator==(const FileId&, const FileId&) = default;
};

// The identity of `path` itself (a link is identified, not its target).
[[nodiscard]] Result<FileId> file_identity(const NativePath& path);

enum class Media : std::uint8_t { unknown, hdd, ssd };

struct VolumeInfo {
    // Stable across reboots and drive letters: "win:<16 hex digits of the volume serial>" on
    // Windows; "uuid:<filesystem UUID>" on Linux, else "fsid:<hex>" from statvfs.
    std::string uid;
    std::string label;   // UTF-8, may be empty
    std::string fs_type; // "NTFS", "ext4", "exfat", ...
    NativePath mount_root;
    // From a read-only probe in the probed folder; falls back to the platform's default.
    bool case_sensitive = false;
    Media media = Media::unknown;
    bool remote = false; // a network share (SMB, NFS, ...)
};

// Describes the volume that holds `path` (a folder). The case-sensitivity probe looks up an
// existing name in `path` with its letter case swapped and compares identities; it writes nothing.
[[nodiscard]] Result<VolumeInfo> volume_info(const NativePath& path);

// Read-only access to a file's contents. Open only entries listed as EntryKind::file.
class FileReader {
public:
    [[nodiscard]] static Result<FileReader> open(const NativePath& path);

    FileReader(FileReader&& other) noexcept;
    FileReader& operator=(FileReader&& other) noexcept;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    ~FileReader();

    // Reads up to buffer.size() bytes; 0 means end of file.
    [[nodiscard]] Result<std::size_t> read(std::span<std::byte> buffer);

    [[nodiscard]] const NativePath& path() const noexcept { return path_; }

private:
    FileReader(NativePath path, std::intptr_t handle) : path_(std::move(path)), handle_(handle) {}
    void close() noexcept;

    NativePath path_;
    std::intptr_t handle_ = -1; // a HANDLE on Windows, a file descriptor on Linux
};

// Swaps the letter case of ASCII letters ("Foo.txt" -> "fOO.TXT").
[[nodiscard]] std::string swap_ascii_case(std::string_view name);

// The case-sensitivity probe used by volume_info(); nullopt when `dir` offers nothing to probe.
[[nodiscard]] std::optional<bool> probe_case_sensitive(const NativePath& dir);

#ifdef _WIN32
// The UTF-16 form of a path for Win32 calls: made absolute with GetFullPathNameW and given the
// \\?\ (or \\?\UNC\) prefix. Fails on bytes that are not WTF-8.
[[nodiscard]] Result<std::wstring> win32_path(const NativePath& path);
// Adds the long-path prefix to an absolute, normalised path; leaves \\?\ and \\.\ paths alone.
[[nodiscard]] std::wstring add_long_path_prefix(std::wstring_view full_path);
// UTF-16 to NativePath (WTF-8), and back without the prefix.
[[nodiscard]] NativePath native_from_wide(std::wstring_view wide);
#endif

} // namespace sheepdog::platform
