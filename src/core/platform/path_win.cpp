// Windows path conversion: WTF-8 NativePath <-> UTF-16, absolute paths and the \\?\ prefix.

#include "core/platform/fs.hpp"
#include "core/platform/win_util.hpp"

#include <vector>

namespace sheepdog::platform {

namespace {

bool starts_with(std::wstring_view s, std::wstring_view prefix) noexcept {
    return s.substr(0, prefix.size()) == prefix;
}

bool is_drive_absolute(std::wstring_view p) noexcept {
    return p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/') &&
           ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z'));
}

bool is_unc(std::wstring_view p) noexcept {
    return p.size() > 2 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/');
}

} // namespace

std::wstring add_long_path_prefix(std::wstring_view full_path) {
    if (starts_with(full_path, LR"(\\?\)") || starts_with(full_path, LR"(\\.\)")) {
        return std::wstring(full_path);
    }
    // With \\?\ Windows no longer normalises the path, so do it here: unify separators and
    // resolve "." and "..". Trailing dots and spaces in names are kept, unlike GetFullPathNameW.
    std::wstring root;
    std::wstring_view rest;
    std::size_t keep = 0; // components that ".." may not remove (server and share for UNC)
    if (is_unc(full_path)) {
        root = LR"(\\?\UNC\)";
        rest = full_path.substr(2);
        keep = 2;
    } else {
        root = LR"(\\?\)";
        root += full_path.substr(0, 2); // "C:"
        root += L'\\';
        rest = full_path.substr(2);
    }
    std::vector<std::wstring_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= rest.size(); ++i) {
        if (i == rest.size() || rest[i] == L'\\' || rest[i] == L'/') {
            const std::wstring_view part = rest.substr(start, i - start);
            start = i + 1;
            if (part.empty() || part == L".") {
                continue;
            }
            if (part == L"..") {
                if (parts.size() > keep) {
                    parts.pop_back();
                }
                continue;
            }
            parts.push_back(part);
        }
    }
    std::wstring out = root;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += L'\\';
        }
        out += parts[i];
    }
    return out;
}

Result<std::wstring> win32_path(const NativePath& path) {
    const auto utf16 = utf16_from_wtf8(path.bytes());
    if (!utf16) {
        return Error{.code = std::make_error_code(std::errc::illegal_byte_sequence), .path = path};
    }
    std::wstring wide(utf16->begin(), utf16->end());
    if (wide.empty() || wide.find(L'\0') != std::wstring::npos) {
        return Error{.code = std::make_error_code(std::errc::invalid_argument), .path = path};
    }
    if (is_drive_absolute(wide) || is_unc(wide)) {
        return add_long_path_prefix(wide);
    }
    // Relative ("docs\a"), drive-relative ("C:a") or rooted ("\a"): let Windows resolve it.
    const DWORD needed = ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return last_error(path);
    }
    std::wstring full(needed, L'\0');
    const DWORD written = ::GetFullPathNameW(wide.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) {
        return last_error(path);
    }
    full.resize(written);
    return add_long_path_prefix(full);
}

NativePath native_from_wide(std::wstring_view wide) {
    std::wstring_view view = wide;
    std::wstring unc;
    if (starts_with(view, LR"(\\?\UNC\)")) {
        unc = L"\\\\";
        unc += view.substr(8);
        view = unc;
    } else if (starts_with(view, LR"(\\?\)")) {
        view = view.substr(4);
    }
    const std::u16string_view u16(reinterpret_cast<const char16_t*>(view.data()), view.size());
    return NativePath(wtf8_from_utf16(u16));
}

} // namespace sheepdog::platform
