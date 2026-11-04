#include "support/temp_tree.hpp"

#include "core/platform/fs.hpp"

#include <atomic>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sheepdog::test {

namespace {

[[noreturn]] void fail(std::string_view what, const NativePath& path) {
    throw std::runtime_error(std::string(what) + " failed: " + platform::display_string(path));
}

std::string unique_name() {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    return "sheepdog-test-" + std::to_string(rd()) + '-' + std::to_string(++counter);
}

#ifdef _WIN32
std::wstring wide(const NativePath& path) {
    auto w = platform::win32_path(path);
    if (!w) {
        fail("win32_path", path);
    }
    return std::move(w).value();
}

NativePath temp_root() {
    std::vector<wchar_t> buf(MAX_PATH + 2);
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(buf.size()), buf.data());
    return platform::native_from_wide(std::wstring_view(buf.data(), n));
}
#else
NativePath temp_root() {
    const char* dir = std::getenv("TMPDIR");
    return NativePath(dir != nullptr && *dir != '\0' ? dir : "/tmp");
}
#endif

} // namespace

TempDir::TempDir() : path_(temp_root() / unique_name()) {
    make_dir(path_);
}

TempDir::~TempDir() {
    try {
        remove_tree(path_);
    } catch (...) { // NOLINT(bugprone-empty-catch): a leftover temp folder is not a test failure
    }
}

#ifdef _WIN32

void make_dir(const NativePath& path) {
    if (::CreateDirectoryW(wide(path).c_str(), nullptr) == 0) {
        fail("CreateDirectoryW", path);
    }
}

void write_file(const NativePath& path, std::string_view content) {
    const HANDLE h = ::CreateFileW(wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fail("CreateFileW", path);
    }
    DWORD written = 0;
    const BOOL ok =
        ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    ::CloseHandle(h);
    if (ok == 0 || written != content.size()) {
        fail("WriteFile", path);
    }
}

void rename_path(const NativePath& from, const NativePath& to) {
    if (::MoveFileExW(wide(from).c_str(), wide(to).c_str(), 0) == 0) {
        fail("MoveFileExW", from);
    }
}

void make_dir_link(const NativePath& link, const NativePath& target) {
    make_dir(link);
    // A junction: a mount-point reparse point whose substitute name is the NT path "\??\C:\...".
    std::wstring print = wide(target).substr(4); // drop "\\?\"
    const std::wstring subst = L"\\??\\" + print;
    const auto subst_bytes = static_cast<WORD>(subst.size() * sizeof(wchar_t));
    const auto print_bytes = static_cast<WORD>(print.size() * sizeof(wchar_t));
    const auto data_length = static_cast<WORD>(8 + subst_bytes + 2 + print_bytes + 2);
    std::vector<unsigned char> buf(8 + std::size_t{data_length}, 0);
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    const WORD header[6] = {data_length, 0, 0, subst_bytes, static_cast<WORD>(subst_bytes + 2),
                            print_bytes};
    std::memcpy(buf.data(), &tag, 4);
    std::memcpy(buf.data() + 4, &header[0], 2); // ReparseDataLength
    std::memcpy(buf.data() + 6, &header[1], 2); // Reserved
    std::memcpy(buf.data() + 8, &header[2], 8); // the four name offsets and lengths
    std::memcpy(buf.data() + 16, subst.data(), subst_bytes);
    std::memcpy(buf.data() + 16 + subst_bytes + 2, print.data(), print_bytes);

    const HANDLE h =
        ::CreateFileW(wide(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fail("CreateFileW (junction)", link);
    }
    DWORD returned = 0;
    const BOOL ok =
        ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()),
                          nullptr, 0, &returned, nullptr);
    ::CloseHandle(h);
    if (ok == 0) {
        fail("FSCTL_SET_REPARSE_POINT", link);
    }
}

void remove_tree(const NativePath& path) {
    const auto details = platform::file_details(path);
    if (!details) {
        return;
    }
    const std::wstring w = wide(path);
    if (details->kind == platform::EntryKind::directory) {
        if (auto entries = platform::list_dir(path)) {
            for (const auto& entry : *entries) {
                remove_tree(path / entry.name);
            }
        }
    }
    ::SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
    const bool is_dir = (details->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if ((is_dir ? ::RemoveDirectoryW(w.c_str()) : ::DeleteFileW(w.c_str())) == 0) {
        fail("remove", path);
    }
}

#else

void make_dir(const NativePath& path) {
    if (::mkdir(path.bytes().c_str(), 0700) != 0) {
        fail("mkdir", path);
    }
}

void write_file(const NativePath& path, std::string_view content) {
    const int fd = ::open(path.bytes().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        fail("open", path);
    }
    const ssize_t n = ::write(fd, content.data(), content.size());
    ::close(fd);
    if (n < 0 || static_cast<std::size_t>(n) != content.size()) {
        fail("write", path);
    }
}

void rename_path(const NativePath& from, const NativePath& to) {
    if (::rename(from.bytes().c_str(), to.bytes().c_str()) != 0) {
        fail("rename", from);
    }
}

void make_dir_link(const NativePath& link, const NativePath& target) {
    if (::symlink(target.bytes().c_str(), link.bytes().c_str()) != 0) {
        fail("symlink", link);
    }
}

void remove_tree(const NativePath& path) {
    const auto details = platform::file_details(path);
    if (!details) {
        return;
    }
    if (details->kind == platform::EntryKind::directory) {
        ::chmod(path.bytes().c_str(), 0700);
        if (auto entries = platform::list_dir(path)) {
            for (const auto& entry : *entries) {
                remove_tree(path / entry.name);
            }
        }
        if (::rmdir(path.bytes().c_str()) != 0) {
            fail("rmdir", path);
        }
    } else if (::unlink(path.bytes().c_str()) != 0) {
        fail("unlink", path);
    }
}

#endif

} // namespace sheepdog::test
