#pragma once

// Test helpers that build real folder trees under the system temp folder. They use native
// calls directly (tests are not product code), take NativePath bytes so that names which are
// not valid UTF-8 (Linux) or contain unpaired surrogates (Windows) can be created, and throw
// std::runtime_error on failure so Catch2 reports it.

#include "core/platform/native_path.hpp"

#include <string_view>

namespace sheepdog::test {

using platform::NativePath;

// A fresh, uniquely named folder under the system temp folder, removed with everything in it
// (links are removed, never followed) when the TempDir goes out of scope.
class TempDir {
public:
    TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir();

    [[nodiscard]] const NativePath& path() const noexcept { return path_; }
    [[nodiscard]] NativePath operator/(std::string_view name) const { return path_ / name; }

private:
    NativePath path_;
};

void make_dir(const NativePath& path);
void write_file(const NativePath& path, std::string_view content);
void rename_path(const NativePath& from, const NativePath& to);
// A link to a folder: a symlink on Linux, a junction on Windows (needs no privileges).
void make_dir_link(const NativePath& link, const NativePath& target);
// Removes a file, a link or a whole folder tree, never following links.
void remove_tree(const NativePath& path);

} // namespace sheepdog::test
