// sheepdog-cli: the scripting interface and test oracle. Links only the core.
#include "core/version.hpp"

#include <cstdio>
#include <span>
#include <string_view>

namespace {

constexpr std::string_view kUsage =
    "Usage: sheepdog-cli [--version | --help]\n"
    "\n"
    "Commands arrive with later releases (root, scan, ls, hash, ...).\n";

void print(std::FILE* out, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), out);
}

} // namespace

int main(int argc, char** argv) {
    const std::span args(argv, static_cast<std::size_t>(argc));
    if (args.size() == 2) {
        const std::string_view arg = args[1];
        if (arg == "--version" || arg == "-V") {
            print(stdout, "sheepdog-cli ");
            print(stdout, sheepdog::version());
            print(stdout, "\n");
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            print(stdout, kUsage);
            return 0;
        }
    }
    print(stderr, kUsage);
    return 2;
}
