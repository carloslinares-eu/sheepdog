#pragma once

// For the few tests whose expectations depend on the machine. CI services such as GitHub Actions
// set CI on their runners: shared virtual machines that are slower and report less about their disks.

#include <cstdlib>

namespace sheepdog::test {

[[nodiscard]] inline bool on_ci() {
    return std::getenv("CI") != nullptr;
}

} // namespace sheepdog::test
