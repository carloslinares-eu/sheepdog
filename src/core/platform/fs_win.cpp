// Windows implementation of folder listings, details, identity and reading.

#include "core/platform/fs.hpp"
#include "core/platform/win_util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <utility>

namespace sheepdog::platform {

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

Error last_error(NativePath path) {
    const DWORD code = ::GetLastError();
    return Error{.code = std::error_code(static_cast<int>(code), std::system_category()),
                 .path = std::move(path)};
}

bool is_not_found(const std::error_code& code) noexcept {
    if (code.category() == std::system_category()) {
        return code.value() == ERROR_FILE_NOT_FOUND || code.value() == ERROR_PATH_NOT_FOUND;
    }
    return code == std::errc::no_such_file_or_directory;
}

namespace win {

Handle open_for_attributes(const std::wstring& win32_path) noexcept {
    return Handle(::CreateFileW(
        win32_path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

bool read_file_id(HANDLE h, FileId& out) noexcept {
    FILE_ID_INFO info{};
    if (::GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof info) != 0) {
        out.volume = info.VolumeSerialNumber;
        std::memcpy(out.file.data(), &info.FileId, out.file.size());
        return true;
    }
    BY_HANDLE_FILE_INFORMATION old{};
    if (::GetFileInformationByHandle(h, &old) == 0) {
        return false;
    }
    out.volume = old.dwVolumeSerialNumber;
    out.file = {};
    const std::uint64_t index = (std::uint64_t{old.nFileIndexHigh} << 32U) | old.nFileIndexLow;
    for (std::size_t i = 0; i < 8; ++i) {
        out.file[i] = static_cast<std::uint8_t>((index >> (8U * i)) & 0xFFU);
    }
    return true;
}

std::string hex64(unsigned long long value) {
    std::array<char, 16> buf{};
    const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), value, 16);
    std::string digits(buf.data(), result.ptr);
    return std::string(16 - digits.size(), '0') + digits;
}

} // namespace win

namespace {

constexpr std::int64_t kUnixEpochIn100ns = 116'444'736'000'000'000;

std::int64_t ns_from_100ns(std::int64_t ticks) noexcept {
    return ticks == 0 ? 0 : (ticks - kUnixEpochIn100ns) * 100;
}

std::int64_t ns_from_filetime(const FILETIME& ft) noexcept {
    const auto ticks = (std::uint64_t{ft.dwHighDateTime} << 32U) | ft.dwLowDateTime;
    return ns_from_100ns(static_cast<std::int64_t>(ticks));
}

FileDetails details_from(DWORD attrs, DWORD reparse_tag, std::uint64_t size, std::int64_t mtime,
                         std::int64_t btime) noexcept {
    FileDetails d;
    // Only name surrogates (symlinks, junctions, ...) are links. Other reparse points, such as
    // OneDrive placeholders and deduplicated files, are ordinary files and folders.
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0U && IsReparseTagNameSurrogate(reparse_tag)) {
        d.kind = EntryKind::link;
    } else if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        d.kind = EntryKind::directory;
    } else if ((attrs & FILE_ATTRIBUTE_DEVICE) != 0U) {
        d.kind = EntryKind::other;
    } else {
        d.kind = EntryKind::file;
    }
    d.size = d.kind == EntryKind::file ? size : 0;
    d.mtime_ns = mtime;
    d.btime_ns = btime;
    d.attributes = attrs;
    d.cloud_only = (attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                             FILE_ATTRIBUTE_OFFLINE)) != 0U;
    return d;
}

struct FindCloser {
    explicit FindCloser(HANDLE handle) noexcept : h(handle) {}
    HANDLE h;
    FindCloser(const FindCloser&) = delete;
    FindCloser& operator=(const FindCloser&) = delete;
    ~FindCloser() { ::FindClose(h); }
};

Result<FileDetails> details_of_handle(HANDLE h, const NativePath& path) {
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (::GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic) == 0 ||
        ::GetFileInformationByHandleEx(h, FileStandardInfo, &standard, sizeof standard) == 0) {
        return last_error(path);
    }
    if (::GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag) == 0) {
        tag.ReparseTag = 0;
    }
    return details_from(basic.FileAttributes, tag.ReparseTag,
                        static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
                        ns_from_100ns(basic.LastWriteTime.QuadPart),
                        ns_from_100ns(basic.CreationTime.QuadPart));
}

} // namespace

Result<FileDetails> file_details(const NativePath& path) {
    auto wide = win32_path(path);
    if (!wide) {
        return wide.error();
    }
    const win::Handle h = win::open_for_attributes(*wide);
    if (!h.valid()) {
        return last_error(path);
    }
    return details_of_handle(h.get(), path);
}

Result<void> remove_app_file(const NativePath& path) {
    auto wide = win32_path(path);
    if (!wide) {
        return wide.error();
    }
    if (::DeleteFileW(wide->c_str()) == 0) {
        return last_error(path);
    }
    return Ok{};
}

Result<std::vector<DirEntry>> list_dir(const NativePath& dir) {
    auto wide = win32_path(dir);
    if (!wide) {
        return wide.error();
    }
    {
        // Refuse to list through a link: FindFirstFileW would follow it.
        const win::Handle h = win::open_for_attributes(*wide);
        if (!h.valid()) {
            return last_error(dir);
        }
        auto self = details_of_handle(h.get(), dir);
        if (!self) {
            return self.error();
        }
        if (self->kind == EntryKind::link) {
            return Error{.code = std::make_error_code(std::errc::too_many_symbolic_link_levels),
                         .path = dir};
        }
        if (self->kind != EntryKind::directory) {
            return Error{.code = std::make_error_code(std::errc::not_a_directory), .path = dir};
        }
    }
    std::wstring pattern = *wide;
    if (pattern.back() != L'\\') {
        pattern += L'\\';
    }
    pattern += L'*';
    WIN32_FIND_DATAW data{};
    const HANDLE find =
        ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                           FIND_FIRST_EX_LARGE_FETCH);
    std::vector<DirEntry> entries;
    if (find == INVALID_HANDLE_VALUE) {
        if (::GetLastError() == ERROR_FILE_NOT_FOUND) {
            return entries; // an empty drive root has not even "." and ".."
        }
        return last_error(dir);
    }
    const FindCloser closer(find);
    do {
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        DirEntry entry;
        entry.name = wtf8_from_utf16(
            std::u16string_view(reinterpret_cast<const char16_t*>(name.data()), name.size()));
        const auto size = (std::uint64_t{data.nFileSizeHigh} << 32U) | data.nFileSizeLow;
        entry.details = details_from(data.dwFileAttributes, data.dwReserved0, size,
                                     ns_from_filetime(data.ftLastWriteTime),
                                     ns_from_filetime(data.ftCreationTime));
        entries.push_back(std::move(entry));
    } while (::FindNextFileW(find, &data) != 0);
    if (::GetLastError() != ERROR_NO_MORE_FILES) {
        return last_error(dir);
    }
    std::ranges::sort(entries,
                      [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return entries;
}

Result<FileId> file_identity(const NativePath& path) {
    auto wide = win32_path(path);
    if (!wide) {
        return wide.error();
    }
    const win::Handle h = win::open_for_attributes(*wide);
    FileId id;
    if (!h.valid() || !win::read_file_id(h.get(), id)) {
        return last_error(path);
    }
    return id;
}

Result<FileReader> FileReader::open(const NativePath& path) {
    auto wide = win32_path(path);
    if (!wide) {
        return wide.error();
    }
    HANDLE h = ::CreateFileW(wide->c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return last_error(path);
    }
    return FileReader(path, reinterpret_cast<std::intptr_t>(h));
}

Result<std::size_t> FileReader::read(std::span<std::byte> buffer) {
    constexpr std::size_t kMaxChunk = std::size_t{1} << 30U;
    DWORD got = 0;
    const auto want = static_cast<DWORD>(std::min(buffer.size(), kMaxChunk));
    if (::ReadFile(reinterpret_cast<HANDLE>(handle_), buffer.data(), want, &got, nullptr) == 0) {
        return last_error(path_);
    }
    return std::size_t{got};
}

void FileReader::close() noexcept {
    if (handle_ != -1) {
        ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
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
