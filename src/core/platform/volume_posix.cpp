// Linux volume information: /proc/self/mountinfo, /dev/disk/by-uuid and by-label, sysfs.

#include "core/platform/fs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <vector>

namespace sheepdog::platform {

namespace {

struct Mount {
    std::string mount_point;
    std::string fs_type;
    std::string source;
};

// mountinfo escapes space, tab, newline and backslash as \ooo (octal).
std::string unescape_octal(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            unsigned value = 0;
            const auto* first = s.data() + i + 1;
            const auto [ptr, ec] = std::from_chars(first, first + 3, value, 8);
            if (ec == std::errc() && ptr == first + 3) {
                out += static_cast<char>(value);
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// udev escapes unsafe characters in /dev/disk/by-label names as \xNN (hex).
std::string unescape_hex(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
            unsigned value = 0;
            const auto* first = s.data() + i + 2;
            const auto [ptr, ec] = std::from_chars(first, first + 2, value, 16);
            if (ec == std::errc() && ptr == first + 2) {
                out += static_cast<char>(value);
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string real_path(const std::string& path) {
    const std::unique_ptr<char, decltype(&std::free)> resolved(::realpath(path.c_str(), nullptr),
                                                               &std::free);
    return resolved ? std::string(resolved.get()) : std::string();
}

bool is_under(std::string_view path, std::string_view mount_point) {
    if (mount_point == "/") {
        return true;
    }
    return path.substr(0, mount_point.size()) == mount_point &&
           (path.size() == mount_point.size() || path[mount_point.size()] == '/');
}

// The mount holding `canonical` on device maj:min; the longest matching mount point wins.
std::optional<Mount> find_mount(const std::string& canonical, unsigned maj, unsigned min) {
    std::ifstream in("/proc/self/mountinfo");
    const std::string dev = std::to_string(maj) + ':' + std::to_string(min);
    std::optional<Mount> best;
    bool best_dev_match = false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string id;
        std::string parent;
        std::string majmin;
        std::string root;
        std::string mount_point;
        fields >> id >> parent >> majmin >> root >> mount_point;
        std::string token;
        while (fields >> token && token != "-") {
        }
        Mount m;
        fields >> m.fs_type >> m.source;
        m.mount_point = unescape_octal(mount_point);
        m.source = unescape_octal(m.source);
        if (!is_under(canonical, m.mount_point)) {
            continue;
        }
        const bool dev_match = majmin == dev;
        // Later lines mount over earlier ones, so ">=" keeps the topmost of equal length.
        if (!best || (dev_match && !best_dev_match) ||
            (dev_match == best_dev_match && m.mount_point.size() >= best->mount_point.size())) {
            best = std::move(m);
            best_dev_match = dev_match;
        }
    }
    return best;
}

// The name of the entry in `dir` (e.g. /dev/disk/by-uuid) that resolves to `device`.
std::string link_name_for(const char* dir, const std::string& device) {
    DIR* d = ::opendir(dir);
    if (d == nullptr) {
        return {};
    }
    std::string found;
    while (const dirent* ent = ::readdir(d)) { // NOLINT(concurrency-mt-unsafe): own DIR stream
        const std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (real_path(std::string(dir) + '/' + name) == device) {
            found = name;
            break;
        }
    }
    ::closedir(d);
    return found;
}

std::string hex64(std::uint64_t value) {
    std::array<char, 16> buf{};
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value, 16);
    (void)ec;
    const std::string digits(buf.data(), ptr);
    return std::string(16 - digits.size(), '0') + digits;
}

Media media_of(unsigned maj, unsigned min) {
    if (maj == 0) {
        return Media::unknown; // tmpfs, overlay, network and other virtual filesystems
    }
    const std::string base = "/sys/dev/block/" + std::to_string(maj) + ':' + std::to_string(min);
    for (const char* rel : {"/queue/rotational", "/../queue/rotational"}) {
        std::ifstream in(base + rel);
        char c = 0;
        if (in >> c) {
            return c == '1' ? Media::hdd : Media::ssd;
        }
    }
    return Media::unknown;
}

} // namespace

Result<VolumeInfo> volume_info(const NativePath& path) {
    struct stat st{};
    if (path.bytes().find('\0') != std::string::npos) {
        return Error{.code = std::make_error_code(std::errc::invalid_argument), .path = path};
    }
    if (::stat(path.bytes().c_str(), &st) != 0) {
        return last_error(path);
    }
    const std::string canonical = real_path(path.bytes());
    if (canonical.empty()) {
        return last_error(path);
    }
    const unsigned maj = ::major(st.st_dev);
    const unsigned min = ::minor(st.st_dev);
    VolumeInfo info;
    const auto mount = find_mount(canonical, maj, min);
    if (mount) {
        info.fs_type = mount->fs_type;
        info.mount_root = NativePath(mount->mount_point);
        if (mount->source.starts_with("/dev/")) {
            const std::string device = real_path(mount->source);
            if (!device.empty()) {
                const std::string uuid = link_name_for("/dev/disk/by-uuid", device);
                if (!uuid.empty()) {
                    info.uid = "uuid:" + uuid;
                }
                info.label =
                    display_string(unescape_hex(link_name_for("/dev/disk/by-label", device)));
            }
        }
    } else {
        info.mount_root = NativePath("/");
    }
    if (info.uid.empty()) {
        struct statvfs vfs{};
        if (::statvfs(path.bytes().c_str(), &vfs) == 0 && vfs.f_fsid != 0) {
            info.uid = "fsid:" + hex64(static_cast<std::uint64_t>(vfs.f_fsid));
        } else {
            info.uid = "dev:" + std::to_string(maj) + ':' + std::to_string(min);
        }
    }
    info.media = media_of(maj, min);
    info.remote = is_remote_fs_type(info.fs_type);
    const bool fat_like =
        info.fs_type == "vfat" || info.fs_type == "msdos" || info.fs_type == "exfat";
    info.case_sensitive = probe_case_sensitive(path).value_or(!fat_like);
    return info;
}

} // namespace sheepdog::platform
