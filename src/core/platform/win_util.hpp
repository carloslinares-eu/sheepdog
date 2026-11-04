#pragma once

// Internal helpers shared by the Windows sources of the platform layer. Not a public header.

#include "core/platform/fs.hpp"

#include <string>
#include <windows.h>

namespace sheepdog::platform::win {

// Owns a HANDLE from CreateFileW.
class Handle {
public:
    explicit Handle(HANDLE h) noexcept : h_(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (valid()) {
            ::CloseHandle(h_);
        }
    }
    [[nodiscard]] bool valid() const noexcept { return h_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }

private:
    HANDLE h_;
};

// Opens a path for reading its attributes only, without following a reparse point and
// without recalling a cloud placeholder. Folders open too (backup semantics).
[[nodiscard]] Handle open_for_attributes(const std::wstring& win32_path) noexcept;

// FILE_ID_INFO, or the older BY_HANDLE_FILE_INFORMATION on filesystems without 128-bit IDs.
[[nodiscard]] bool read_file_id(HANDLE h, FileId& out) noexcept;

// 16 lower-case hex digits.
[[nodiscard]] std::string hex64(unsigned long long value);

} // namespace sheepdog::platform::win
