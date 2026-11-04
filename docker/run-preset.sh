#!/bin/sh
# Configure, build and test one CMake preset inside the container.
# The source at /src is read-only, so the build goes to /build/<preset> (a named volume).
# Usage: run-preset <preset> [extra ctest arguments]
set -eu
preset="$1"; shift
bdir="/build/$preset"
jobs="${JOBS:-4}"

cmake --preset "$preset" -S /src -B "$bdir" \
    -DVCPKG_INSTALLED_DIR=/build/vcpkg_installed-"$preset" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ${CMAKE_EXTRA:-} > "$bdir.configure.log" 2>&1 \
    || { tail -n 40 "$bdir.configure.log"; exit 1; }
cmake --build "$bdir" -j "$jobs"
ctest --test-dir "$bdir" --output-on-failure -j "$jobs" "$@"
