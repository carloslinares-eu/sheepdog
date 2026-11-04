#pragma once

// The path codec. A NativePath holds a path exactly as the operating system knows it:
// - Linux: the raw bytes, which need not be valid UTF-8;
// - Windows: the UTF-16 path encoded as WTF-8 (UTF-8 that also allows unpaired surrogates),
//   so every name NTFS accepts survives a round trip.
// These bytes go into the catalog's path BLOBs. display_string() gives the valid UTF-8 text
// for the UI and search; it is lossy and never used to reach a file again.

#include <optional>
#include <string>
#include <string_view>

namespace sheepdog::platform {

#ifdef _WIN32
inline constexpr char kSeparator = '\\';
#else
inline constexpr char kSeparator = '/';
#endif

class NativePath {
public:
    NativePath() = default;
    // Takes native bytes as they are (WTF-8 on Windows, raw bytes on Linux).
    explicit NativePath(std::string bytes) : bytes_(std::move(bytes)) {}

    [[nodiscard]] const std::string& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

    // Appends one name, adding a separator unless the path already ends in one.
    [[nodiscard]] NativePath operator/(std::string_view name) const;

    // The last component (after the last separator; on Windows '/' counts too).
    [[nodiscard]] std::string_view filename() const noexcept;
    // Everything before the last component, without the trailing separator ("" if none).
    [[nodiscard]] NativePath parent() const;

    friend bool operator==(const NativePath&, const NativePath&) = default;
    friend auto operator<=>(const NativePath&, const NativePath&) = default;

private:
    std::string bytes_;
};

// Valid UTF-8 for display: every byte that is not part of a valid UTF-8 sequence, and every
// encoded surrogate (from WTF-8), becomes U+FFFD.
[[nodiscard]] std::string display_string(std::string_view native_bytes);
[[nodiscard]] inline std::string display_string(const NativePath& path) {
    return display_string(path.bytes());
}

// True if the bytes are valid UTF-8 (no surrogates, no overlong forms).
[[nodiscard]] bool is_valid_utf8(std::string_view bytes) noexcept;

// WTF-8 codec. These are plain functions on every OS, so both platforms test them.
// Encoding never fails: unpaired surrogates are encoded as 3-byte sequences.
[[nodiscard]] std::string wtf8_from_utf16(std::u16string_view utf16);
// Decoding fails (nullopt) on bytes that are not WTF-8.
[[nodiscard]] std::optional<std::u16string> utf16_from_wtf8(std::string_view wtf8);

} // namespace sheepdog::platform
