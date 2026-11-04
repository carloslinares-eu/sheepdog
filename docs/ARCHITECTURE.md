# Sheepdog architecture

This file is the working memory of the code: module APIs, the schema version and the thread
model, kept current by every task that changes them. The reasoning behind the design is in
[DESIGN.md](DESIGN.md).

## Build targets

| Target | Kind | Links | Notes |
|---|---|---|---|
| `sheepdog_core` (`sheepdog::core`) | static library | nothing yet | The engine. No Qt, ever. Include root is `src/`, so headers are included as `"core/..."`. |
| `sheepdog-cli` | executable | core | Scripting interface and test oracle. |
| `sheepdog_gui` | static library | core, Qt6::Widgets | GUI code, split from `main()` so the offscreen tests can link it. |
| `sheepdog` | executable (WIN32) | sheepdog_gui | The Qt Widgets GUI. Built when `SHEEPDOG_BUILD_GUI=ON` (default). |
| `sheepdog_tests` | Catch2 executable | core, sqlite3, blake3 | Engine tests; Catch2 tags become ctest labels. |
| `sheepdog_gui_tests` | Catch2 executable | sheepdog_gui | Offscreen GUI tests (`QT_QPA_PLATFORM=offscreen`, label `gui`), discovered at test time. |

CMake options: `SHEEPDOG_BUILD_GUI`, `SHEEPDOG_BUILD_TESTS`, `SHEEPDOG_BUILD_FUZZ` (no fuzz targets
yet), `WARNINGS_AS_ERRORS`, `SHEEPDOG_SANITIZE` (e.g. `address,undefined` or `thread`).

CMake helpers in `cmake/`:
- `Warnings.cmake`: `sheepdog_set_warnings(target)`; every project target calls it.
- `Sanitizers.cmake`: applies `SHEEPDOG_SANITIZE` to all targets.
- `Version.cmake` + `GitVersion.cmake`: the `sheepdog_version_info` target regenerates
  `build/<preset>/generated/version_info.cpp` on every build (rewritten only when it changes).
- `WindowsResources.cmake`: `sheepdog_add_windows_resources(target description)` embeds
  `cmake/windows/version.rc.in` and `cmake/windows/sheepdog.manifest` (asInvoker, long paths,
  UTF-8 code page, per-monitor-v2 DPI). No icon until v0.9.

Dependencies come from `vcpkg.json` (manifest mode, `builtin-baseline` pinned). Triplets:
`x64-mingw-static` for target and host on Windows, `x64-linux` on Linux. Qt is not from vcpkg:
the Qt 6.12 MinGW kit on Windows, Debian's Qt 6.8.2 in the Linux container (minimum 6.8).

## Source layout

```
src/core/      sheepdog_core (no Qt)
  version.hpp    project_version(), version(), git_commit()
src/cli/       sheepdog-cli (main.cpp: --version, --help)
src/gui/       sheepdog_gui + sheepdog (MainWindow, window_title())
tests/         Catch2 tests
cmake/         CMake helpers, Windows resources, CMakeUserPresets template
scripts/       env.ps1, build.ps1, test.ps1, check-env.ps1 (Windows PowerShell 5.1)
docs/          DESIGN, RELEASE_PLAN, ARCHITECTURE, DEVELOPMENT, USER_GUIDE, CONTRIBUTING
```

## Module APIs

### core/version.hpp

- `std::string_view project_version()`: `project(VERSION)` from CMake, e.g. `0.1.0`.
- `std::string_view version()`: `X.Y.Z` on a clean checkout of tag `sheepdog-X.Y.Z`, otherwise
  `X.Y.Z-N-gSHA[-dirty]`. N counts commits since the last `sheepdog-*` tag, or all commits when
  there is no tag yet. Without git it is `X.Y.Z`. Shown by `sheepdog-cli --version` and in the
  main window's title (`Sheepdog <version>`).
- `std::string_view git_commit()`: 7-character hash, or empty.

### gui/main_window.hpp

- `sheepdog::gui::window_title()`: `"Sheepdog " + version()`.
- `sheepdog::gui::MainWindow`: empty `QMainWindow` with that title. Menus, docks and the file
  table come with 0.1-T8.

## Catalog schema

No catalog yet (schema v1 arrives with 0.1-T5).

## Thread model

Nothing threaded yet. Planned (DESIGN.md, "Architecture"): GUI thread, one `std::jthread` per job,
a bounded worker pool, one DB-writer thread, a query thread.
