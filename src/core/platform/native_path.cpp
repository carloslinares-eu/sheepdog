#include "core/platform/native_path.hpp"

#include <cstddef>
#include <cstdint>

namespace sheepdog::platform {

namespace {

bool is_sep(char c) noexcept {
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

struct Decoded {
    char32_t cp = 0;
    std::size_t len = 0; // 0: not a valid sequence at this position
};

bool cont(std::string_view s, std::size_t i) noexcept {
    return i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0U) == 0x80U;
}

std::uint32_t byte(std::string_view s, std::size_t i) noexcept {
    return static_cast<unsigned char>(s[i]);
}

// Decodes one generalized UTF-8 sequence: well-formed UTF-8, plus 3-byte encoded surrogates
// (U+D800..U+DFFF) as WTF-8 allows. Overlong forms and values above U+10FFFF are invalid.
Decoded decode(std::string_view s, std::size_t i) noexcept {
    const std::uint32_t b0 = byte(s, i);
    if (b0 < 0x80U) {
        return {.cp = static_cast<char32_t>(b0), .len = 1};
    }
    if (b0 >= 0xC2U && b0 <= 0xDFU) {
        if (!cont(s, i + 1)) {
            return {};
        }
        return {.cp = static_cast<char32_t>(((b0 & 0x1FU) << 6U) | (byte(s, i + 1) & 0x3FU)),
                .len = 2};
    }
    if (b0 >= 0xE0U && b0 <= 0xEFU) {
        if (!cont(s, i + 1) || !cont(s, i + 2)) {
            return {};
        }
        const std::uint32_t b1 = byte(s, i + 1);
        if (b0 == 0xE0U && b1 < 0xA0U) {
            return {}; // overlong
        }
        const auto cp = ((b0 & 0x0FU) << 12U) | ((b1 & 0x3FU) << 6U) | (byte(s, i + 2) & 0x3FU);
        return {.cp = static_cast<char32_t>(cp), .len = 3};
    }
    if (b0 >= 0xF0U && b0 <= 0xF4U) {
        if (!cont(s, i + 1) || !cont(s, i + 2) || !cont(s, i + 3)) {
            return {};
        }
        const std::uint32_t b1 = byte(s, i + 1);
        if ((b0 == 0xF0U && b1 < 0x90U) || (b0 == 0xF4U && b1 > 0x8FU)) {
            return {}; // overlong or above U+10FFFF
        }
        const auto cp = ((b0 & 0x07U) << 18U) | ((b1 & 0x3FU) << 12U) |
                        ((byte(s, i + 2) & 0x3FU) << 6U) | (byte(s, i + 3) & 0x3FU);
        return {.cp = static_cast<char32_t>(cp), .len = 4};
    }
    return {};
}

bool is_surrogate(char32_t cp) noexcept {
    return cp >= 0xD800U && cp <= 0xDFFFU;
}

void append_utf8(std::string& out, char32_t cp) {
    const auto c = static_cast<std::uint32_t>(cp);
    if (c < 0x80U) {
        out += static_cast<char>(c);
    } else if (c < 0x800U) {
        out += static_cast<char>(0xC0U | (c >> 6U));
        out += static_cast<char>(0x80U | (c & 0x3FU));
    } else if (c < 0x10000U) {
        out += static_cast<char>(0xE0U | (c >> 12U));
        out += static_cast<char>(0x80U | ((c >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (c & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (c >> 18U));
        out += static_cast<char>(0x80U | ((c >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((c >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (c & 0x3FU));
    }
}

constexpr char32_t kReplacement = 0xFFFD;

} // namespace

NativePath NativePath::operator/(std::string_view name) const {
    std::string out = bytes_;
    if (!out.empty() && !is_sep(out.back())) {
        out += kSeparator;
    }
    out += name;
    return NativePath(std::move(out));
}

std::string_view NativePath::filename() const noexcept {
    const std::string_view s(bytes_);
    for (std::size_t i = s.size(); i > 0; --i) {
        if (is_sep(s[i - 1])) {
            return s.substr(i);
        }
    }
    return s;
}

NativePath NativePath::parent() const {
    const std::string_view name = filename();
    std::string_view head(bytes_.data(), bytes_.size() - name.size());
    // Drop the separator(s) before the name, but keep a root ("/", "C:\", "\\").
    while (!head.empty() && is_sep(head.back())) {
        const std::string_view shorter = head.substr(0, head.size() - 1);
        if (shorter.empty() || shorter.back() == ':') {
            break;
        }
        head = shorter;
    }
    return NativePath(std::string(head));
}

bool is_valid_utf8(std::string_view bytes) noexcept {
    for (std::size_t i = 0; i < bytes.size();) {
        const Decoded d = decode(bytes, i);
        if (d.len == 0 || is_surrogate(d.cp)) {
            return false;
        }
        i += d.len;
    }
    return true;
}

std::string display_string(std::string_view native_bytes) {
    std::string out;
    out.reserve(native_bytes.size());
    for (std::size_t i = 0; i < native_bytes.size();) {
        const Decoded d = decode(native_bytes, i);
        if (d.len == 0) {
            append_utf8(out, kReplacement);
            ++i;
        } else if (is_surrogate(d.cp)) {
            append_utf8(out, kReplacement);
            i += d.len;
        } else {
            out.append(native_bytes.substr(i, d.len));
            i += d.len;
        }
    }
    return out;
}

std::string wtf8_from_utf16(std::u16string_view utf16) {
    std::string out;
    out.reserve(utf16.size());
    for (std::size_t i = 0; i < utf16.size(); ++i) {
        const char16_t u = utf16[i];
        const bool lead = u >= 0xD800U && u <= 0xDBFFU;
        if (lead && i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00U && utf16[i + 1] <= 0xDFFFU) {
            const auto hi = static_cast<std::uint32_t>(u - 0xD800U);
            const auto lo = static_cast<std::uint32_t>(utf16[i + 1] - 0xDC00U);
            append_utf8(out, static_cast<char32_t>(0x10000U + (hi << 10U) + lo));
            ++i;
        } else {
            append_utf8(out, static_cast<char32_t>(u)); // lone surrogates become 3 bytes
        }
    }
    return out;
}

std::optional<std::u16string> utf16_from_wtf8(std::string_view wtf8) {
    std::u16string out;
    out.reserve(wtf8.size());
    for (std::size_t i = 0; i < wtf8.size();) {
        const Decoded d = decode(wtf8, i);
        if (d.len == 0) {
            return std::nullopt;
        }
        const auto cp = static_cast<std::uint32_t>(d.cp);
        if (cp >= 0x10000U) {
            const std::uint32_t v = cp - 0x10000U;
            out += static_cast<char16_t>(0xD800U + (v >> 10U));
            out += static_cast<char16_t>(0xDC00U + (v & 0x3FFU));
        } else {
            out += static_cast<char16_t>(cp);
        }
        i += d.len;
    }
    return out;
}

} // namespace sheepdog::platform
