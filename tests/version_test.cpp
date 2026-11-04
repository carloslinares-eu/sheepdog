#include "core/version.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

TEST_CASE("version starts with the project version", "[unit]") {
    const std::string full(sheepdog::version());
    const std::string base(sheepdog::project_version());
    REQUIRE_FALSE(base.empty());
    CHECK(full.rfind(base, 0) == 0);
}

TEST_CASE("version has the git describe shape", "[unit]") {
    using Catch::Matchers::Matches;
    const std::string full(sheepdog::version());
    // X.Y.Z for a tagged release, else X.Y.Z-N-gSHA with an optional -dirty suffix.
    CHECK_THAT(full, Matches(R"(\d+\.\d+\.\d+(-\d+-g[0-9a-f]{7,}(-dirty)?)?)"));
    if (!sheepdog::git_commit().empty() && full.find("-g") != std::string::npos) {
        CHECK(full.find(std::string(sheepdog::git_commit())) != std::string::npos);
    }
}
