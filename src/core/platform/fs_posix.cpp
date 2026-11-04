// Linux implementation of folder listings, details, identity and reading.

#include "core/platform/fs.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace sheepdog::platform {

Error last_error(NativePath path) {
    return Error{.code = std::error_code(errno, std::generic_category()), .path = std::move(path)};
}

bool is_not_found(const std::error_code& code) noexcept {
    return code == std::errc::no_such_file_or_directory || code == std::errc::not_a_directory;
}

namespace {

Error make_error(std::errc code, const NativePath& path) {
    return Error{.code = std::make_error_code(code), .path = path};
}

// Paths go to the kernel as C strings, so an embedded NUL would silently cut them short.
bool has_nul(const NativePath& path) noexcept {
    return path.bytes().find('\0') != std::string::npos;
}

EntryKind kind_of(unsigned mode) noexcept {
    if (S_ISREG(mode)) {
        return EntryKind::file;
    }
    if (S_ISDIR(mode)) {
        return EntryKind::directory;
    }
    if (S_ISLNK(mode)) {
        return EntryKind::link;
    }
    return EntryKind::other;
}

std::int64_t to_ns(std::int64_t sec, std::int64_t nsec) noexcept {
    return (sec * 1'000'000'000) + nsec;
}

// statx without following a final link, falling back to fstatat where statx is missing.
int stat_at(int dirfd, const char* name, FileDetails& out) {
    struct statx sx{};
    constexpr unsigned kMask = STATX_BASIC_STATS | STATX_BTIME;
    if (::statx(dirfd, name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, kMask, &sx) == 0) {
        out.kind = kind_of(sx.stx_mode);
        out.size = out.kind == EntryKind::directory ? 0 : sx.stx_size;
        out.mtime_ns = to_ns(sx.stx_mtime.tv_sec, sx.stx_mtime.tv_nsec);
        out.btime_ns = (sx.stx_mask & STATX_BTIME) != 0U
                           ? to_ns(sx.stx_btime.tv_sec, sx.stx_btime.tv_nsec)
                           : 0;
        out.attributes = sx.stx_mode;
        return 0;
    }
    if (errno != ENOSYS && errno != EPERM) {
        return -1;
    }
    struct stat st{};
    if (::fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return -1;
    }
    out.kind = kind_of(st.st_mode);
    out.size = out.kind == EntryKind::directory ? 0 : static_cast<std::uint64_t>(st.st_size);
    out.mtime_ns = to_ns(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    out.btime_ns = 0;
    out.attributes = st.st_mode;
    return 0;
}

struct DirCloser {
    void operator()(DIR* d) const noexcept { ::closedir(d); }
};

} // namespace

Result<std::vector<DirEntry>> list_dir(const NativePath& dir) {
    if (has_nul(dir)) {
        return make_error(std::errc::invalid_argument, dir);
    }
    const int fd = ::open(dir.bytes().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        Error error = last_error(dir);
        struct stat st{};
        if (errno == ENOTDIR && ::lstat(dir.bytes().c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
            error.code = std::make_error_code(std::errc::too_many_symbolic_link_levels);
        }
        return error;
    }
    const std::unique_ptr<DIR, DirCloser> handle(::fdopendir(fd));
    if (!handle) {
        Error error = last_error(dir);
        ::close(fd);
        return error;
    }
    std::vector<DirEntry> entries;
    for (;;) {
        errno = 0;
        // readdir is safe here: each thread uses its own DIR stream.
        const dirent* ent = ::readdir(handle.get()); // NOLINT(concurrency-mt-unsafe)
        if (ent == nullptr) {
            if (errno != 0) {
                return last_error(dir);
            }
            break;
        }
        const char* name = ent->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
            continue;
        }
        DirEntry entry;
        entry.name = name;
        if (stat_at(::dirfd(handle.get()), name, entry.details) != 0) {
            if (errno == ENOENT) {
                continue; // deleted while we were listing
            }
            entry.error = std::error_code(errno, std::generic_category());
            entry.details = {};
        }
        entries.push_back(std::move(entry));
    }
    std::ranges::sort(entries,
                      [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return entries;
}

Result<FileDetails> file_details(const NativePath& path) {
    if (has_nul(path)) {
        return make_error(std::errc::invalid_argument, path);
    }
    FileDetails details;
    if (stat_at(AT_FDCWD, path.bytes().c_str(), details) != 0) {
        return last_error(path);
    }
    return details;
}

Result<FileId> file_identity(const NativePath& path) {
    if (has_nul(path)) {
        return make_error(std::errc::invalid_argument, path);
    }
    struct stat st{};
    if (::lstat(path.bytes().c_str(), &st) != 0) {
        return last_error(path);
    }
    FileId id;
    id.volume = static_cast<std::uint64_t>(st.st_dev);
    auto ino = static_cast<std::uint64_t>(st.st_ino);
    for (std::size_t i = 0; i < 8; ++i) {
        id.file[i] = static_cast<std::uint8_t>(ino & 0xFFU);
        ino >>= 8U;
    }
    return id;
}

Result<FileReader> FileReader::open(const NativePath& path) {
    if (has_nul(path)) {
        return make_error(std::errc::invalid_argument, path);
    }
    const int fd = ::open(path.bytes().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        return last_error(path);
    }
    (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    return FileReader(path, fd);
}

Result<std::size_t> FileReader::read(std::span<std::byte> buffer) {
    for (;;) {
        const ssize_t n = ::read(static_cast<int>(handle_), buffer.data(), buffer.size());
        if (n >= 0) {
            return static_cast<std::size_t>(n);
        }
        if (errno != EINTR) {
            return last_error(path_);
        }
    }
}

void FileReader::close() noexcept {
    if (handle_ >= 0) {
        ::close(static_cast<int>(handle_));
        handle_ = -1;
    }
}

FileReader::FileReader(FileReader&& other) noexcept
    : path_(std::move(other.path_)), handle_(std::exchange(other.handle_, -1)) {}

FileReader& FileReader::operator=(FileReader&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}

FileReader::~FileReader() {
    close();
}

} // namespace sheepdog::platform
