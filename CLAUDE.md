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
  the host), in `~/Workspace/sheepdog-ci`, a scratch copy replaced on every run (`~/Workspace/sheepdog`
  is a git clone the scripts leave alone). Copy the tree with tar to a temp file + scp; never pipe binary
  data through PowerShell. `scripts\linux-test.ps1 [test|asan|tsan|tidy]` does all that and runs the
  compose service (`compose.yaml`, image `docker/ci.Dockerfile`: GCC 14, Clang 19, Qt 6.8); full log in
  `~/Workspace/sheepdog-<service>.log` on carbon.

## Code map

| Directory | Responsibility | Key types |
|---|---|---|
| `src/core/` | `sheepdog_core`, the Qt-free engine | `version()` |
| `src/core/platform/` | OS layer, read side (`*_win.cpp` / `*_posix.cpp`) | `Result<T>`, `NativePath`, `list_dir`, `file_identity`, `volume_info`, `FileReader` |
| `src/core/catalog/` | SQLite catalog: wrapper, schema v1 + migrations, backups, DB-writer thread | `Database`, `open_catalog`, `migrate`, `CatalogWriter` |
| `src/core/jobs/` | Job framework: job threads, shared worker pool, progress snapshots, stop-aware chunked reads, file-change lock | `JobRunner`, `Job`, `JobContext`, `TaskGroup`, `BoundedPool`, `Progress`, `Snapshot`, `Clock`, `ErrorLog`, `read_chunks` |
| `src/cli/` | `sheepdog-cli`, links only the core | `main` |
| `src/gui/` | `sheepdog_gui` lib + `sheepdog` exe | `gui::MainWindow` |
| `tests/` | Catch2 tests (`sheepdog_tests`, `sheepdog_gui_tests`) | `test::TempDir` (`tests/support/temp_tree.hpp`), `test::FakeClock` (`tests/support/jobs_support.hpp`) |
| `cmake/` | warnings, sanitizers, git version, Windows rc + manifest | |
| `scripts/` | build, test, env, check-env, linux-test (PowerShell 5.1) | |
| `docker/`, `compose.yaml` | Linux CI image and services (run on carbon) | |
| `.github/workflows/` | CI: Windows MinGW job + Linux container job | |

## Conventions

- C++20. `sheepdog_core` never includes Qt; only `platform/` uses `#ifdef _WIN32`.
- Platform calls return `Result<T>` (value or `std::error_code` + path); no exceptions cross threads.
- Numbers via `std::from_chars`. No `std::format`, no `<chrono>` time zones (AppImage uses GCC 12).
- Files under ~500 lines. Every target calls `sheepdog_set_warnings()`; code must build with
  `WARNINGS_AS_ERRORS=ON`.
- Catch2 assertions are not thread-safe: threads record results in atomics, the test thread checks.
- Include project headers as `"core/..."`, `"gui/..."` (include root is `src/`).
- Every change: a line under *Unreleased* in `CHANGELOG.md`; interface changes update
  `docs/ARCHITECTURE.md`.

## Safety rules

- Files are changed only through `FsOps` (from v0.2). The executor runs only under `SHEEPDOG_SANDBOX`.
- Never run `apply` or any file operation on real user folders; tests use temp folders.
- Never commit build output, `.idea/`, `CMakeUserPresets.json` or logs.

## Do not read

`build*/`, `cmake-build-*/`, `vcpkg_installed/`, binary test files, `.ts` translation files.
