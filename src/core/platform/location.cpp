// Network-location checks: is a path on a share (UNC, mapped drive, NFS, SMB, ...)?
// The catalog uses them to refuse network locations, where SQLite's locking is unreliable.

#include "core/platform/fs.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace sheepdog::platform {

bool is_remote_fs_type(std::string_view fs_type) noexcept {
    static constexpr std::array<std::string_view, 10> kPrefixes = {
        "nfs", "cifs", "smb",       "fuse.sshfs",  "9p",
        "afs", "ceph", "glusterfs", "fuse.rclone", "davfs"};
    return std::ranges::any_of(
        kPrefixes, [fs_type](std::string_view prefix) { return fs_type.starts_with(prefix); });
}

namespace {

bool is_sep(char c) noexcept {
    return c == '\\' || c == '/';
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) {
                   return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
               };
               return lower(x) == lower(y);
           });
}

} // namespace

bool is_unc_path(std::string_view bytes) noexcept {
    if (bytes.size() < 3 || !is_sep(bytes[0]) || !is_sep(bytes[1])) {
        return false;
    }
    if ((bytes[2] == '?' || bytes[2] == '.') && (bytes.size() == 3 || is_sep(bytes[3]))) {
        // \\?\... or \\.\...: a UNC path only as \\?\UNC\server\share.
        return bytes.size() >= 8 && iequals_ascii(bytes.substr(4, 3), "UNC") && is_sep(bytes[7]);
    }
    return !is_sep(bytes[2]);
}

Result<bool> is_network_location(const NativePath& path) {
#ifdef _WIN32
    if (is_unc_path(path.bytes())) {
        return true;
    }
#endif
    // The nearest existing folder: the catalog file itself may not exist yet.
    NativePath dir = path.empty() ? NativePath(".") : path;
    for (;;) {
        const auto details = file_details(dir);
        // A link to a folder counts: volume_info() follows it to the share it points to.
        if (details &&
            (details->kind == EntryKind::directory || details->kind == EntryKind::link)) {
            break;
        }
        if (!details && !is_not_found(details.error().code)) {
            return details.error();
        }
        NativePath up = dir.parent();
        if (up.empty()) {
            up = NativePath(".");
        }
        if (up == dir) {
            return Error{.code = std::make_error_code(std::errc::no_such_file_or_directory),
                         .path = path};
        }
        dir = std::move(up);
    }
    const auto volume = volume_info(dir);
    if (!volume) {
        return volume.error();
    }
    return volume->remote;
}

} // namespace sheepdog::platform
