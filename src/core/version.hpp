#pragma once

#include <string_view>

namespace sheepdog {

// The release number from CMake's project(VERSION), e.g. "0.1.0".
std::string_view project_version() noexcept;

// The full version shown in --version, the title bar and the About dialog:
// "0.1.0" for a tagged release, otherwise "0.1.0-N-gSHA" with an optional
// "-dirty" suffix (see cmake/GitVersion.cmake).
std::string_view version() noexcept;

// Abbreviated commit hash of the build, or empty when built outside git.
std::string_view git_commit() noexcept;

} // namespace sheepdog
