// Platform-neutral parts of the read side: error text and the case-sensitivity probe.

#include "core/platform/fs.hpp"

#include <cstddef>

namespace sheepdog::platform {

std::string Error::message() const {
    std::string text = code.message();
    if (!path.empty()) {
        text += " (";
        text += display_string(path);
        text += ')';
    }
    return text;
}

std::string swap_ascii_case(std::string_view name) {
    std::string out(name);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

namespace {

// true: case-sensitive here, false: insensitive, nullopt: this name tells us nothing.
std::optional<bool> probe_name(const NativePath& parent, std::string_view name) {
    const std::string swapped = swap_ascii_case(name);
    if (swapped == name) {
        return std::nullopt;
    }
    const auto original = file_identity(parent / name);
    if (!original) {
        return std::nullopt;
    }
    const auto other = file_identity(parent / swapped);
    if (other) {
        return !(*original == *other);
    }
    if (is_not_found(other.error().code)) {
        return true;
    }
    return std::nullopt;
}

} // namespace

std::optional<bool> probe_case_sensitive(const NativePath& dir) {
    constexpr std::size_t kMaxTries = 8;
    if (auto entries = list_dir(dir)) {
        std::size_t tries = 0;
        for (const DirEntry& entry : *entries) {
            if (entry.error || tries == kMaxTries) {
                continue;
            }
            if (swap_ascii_case(entry.name) == entry.name) {
                continue;
            }
            ++tries;
            if (const auto answer = probe_name(dir, entry.name)) {
                return answer;
            }
        }
    }
    // Nothing usable inside: try the folder's own name in its parent.
    const NativePath parent = dir.parent();
    if (!parent.empty() && parent != dir) {
        return probe_name(parent, dir.filename());
    }
    return std::nullopt;
}

} // namespace sheepdog::platform
