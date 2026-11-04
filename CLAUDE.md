# CLAUDE.md

Sheepdog: a Windows/Linux file cataloger and organiser. C++20, Qt 6 Widgets GUI, Qt-free core.
Plan: `docs/RELEASE_PLAN.md` (single source of truth; one task = one commit `<id>: <title>`).
Code map and APIs: `docs/ARCHITECTURE.md`. Design reasons: `docs/DESIGN.md` (36 KB, read only the
section you need). Machine setup: `docs/DEVELOPMENT.md`.

## Build and test (Windows, MinGW; there is no MSVC)

```powershell
scripts\check-env.ps1                  # toolchain check
scripts\build.ps1 [-Preset win-debug]  # configure + build (win-debug, win-release, win-ci)
scripts\test.ps1                       # build win-debug + ctest
scripts\test.ps1 -Filter version       # one test: ctest -R version
scripts\test.ps1 -Label unit           # one group: ctest -L unit
scripts\test.ps1 -Preset win-ci        # -Werror, as CI
```

- Toolchain: Qt 6.12.0 MinGW kit `C:\Qt\6.12.0\mingw_64`, GCC 13.1 `C:\Qt\Tools\mingw1310_64\bin`,
  CMake 3.30 and Ninja from `C:\Qt\Tools`. `scripts\env.ps1` puts them on PATH; ignore the
  CMake 3.25 on the system PATH.
- vcpkg manifest mode (`vcpkg.json`, `builtin-baseline` pinned), triplet `x64-mingw-static` for
  target and host. Installs into `build\vcpkg_installed`.
- The scripts are Windows PowerShell 5.1: no `&&`, no `?:`; never redirect native stderr with `2>&1`.
  Write long logs to a file and tail/grep them.
- Running a built exe by hand needs the toolchain on PATH: `. scripts\env.ps1` first.
- Catch2 tags are ctest labels: `unit fs gui fault slow win linux`. GUI tests run offscreen.
- Linux: build and test only inside containers on `ssh carbon` (Debian 13, 4 CPUs, no cmake/Qt on
  the host), in `~/src/sheepdog`. Copy the tree with tar to a temp file + scp; never pipe binary
  data through PowerShell. `scripts/linux-test.ps1 <service>` arrives with 0.1-T3.

## Code map

| Directory | Responsibility | Key types |
|---|---|---|
| `src/core/` | `sheepdog_core`, the Qt-free engine | `version()` |
| `src/cli/` | `sheepdog-cli`, links only the core | `main` |
| `src/gui/` | `sheepdog_gui` lib + `sheepdog` exe | `gui::MainWindow` |
| `tests/` | Catch2 tests (`sheepdog_tests`, `sheepdog_gui_tests`) | |
| `cmake/` | warnings, sanitizers, git version, Windows rc + manifest | |
| `scripts/` | build, test, env, check-env (PowerShell 5.1) | |

## Conventions

- C++20. `sheepdog_core` never includes Qt; only `platform/` uses `#ifdef _WIN32`.
- Platform calls return `Result<T>` (value or `std::error_code` + path); no exceptions cross threads.
- Numbers via `std::from_chars`. No `std::format`, no `<chrono>` time zones (AppImage uses GCC 12).
- Files under ~500 lines. Every target calls `sheepdog_set_warnings()`; code must build with
  `WARNINGS_AS_ERRORS=ON`.
- Include project headers as `"core/..."`, `"gui/..."` (include root is `src/`).
- Every change: a line under *Unreleased* in `CHANGELOG.md`; interface changes update
  `docs/ARCHITECTURE.md`.

## Safety rules

- Files are changed only through `FsOps` (from v0.2). The executor runs only under `SHEEPDOG_SANDBOX`.
- Never run `apply` or any file operation on real user folders; tests use temp folders.
- Never commit build output, `.idea/`, `CMakeUserPresets.json` or logs.

## Do not read

`build*/`, `cmake-build-*/`, `vcpkg_installed/`, binary test files, `.ts` translation files.
