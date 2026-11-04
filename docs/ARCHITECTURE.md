# Sheepdog architecture

This file is the working memory of the code: module APIs, the schema version and the thread
model, kept current by every task that changes them. The reasoning behind the design is in
[DESIGN.md](DESIGN.md).

## Build targets

| Target | Kind | Links | Notes |
|---|---|---|---|
| `sheepdog_core` (`sheepdog::core`) | static library | nothing (system libraries only) | The engine. No Qt, ever. Include root is `src/`, so headers are included as `"core/..."`. |
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
  platform/      the only code with #ifdef _WIN32; *_win.cpp or *_posix.cpp chosen by CMake
    result.hpp       Error, Result<T>, Result<void>/Ok, last_error(), is_not_found()
    native_path.hpp  NativePath, display_string(), WTF-8 codec, is_valid_utf8()
    fs.hpp           list_dir, file_details, file_identity, volume_info, FileReader
    fs_common.cpp    Error::message, swap_ascii_case, probe_case_sensitive (OS-neutral)
    win_util.hpp     internal Windows helpers (Handle, open_for_attributes, read_file_id)
src/cli/       sheepdog-cli (main.cpp: --version, --help)
src/gui/       sheepdog_gui + sheepdog (MainWindow, window_title())
tests/         Catch2 tests (platform_win_test.cpp only on Windows, platform_linux_test.cpp
               only on Linux)
  support/temp_tree.hpp  TempDir (unique folder under the system temp, removed without
                         following links), make_dir, write_file, rename_path, make_dir_link
                         (symlink / junction), remove_tree; native calls, NativePath names
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

### core/platform (read side, 0.1-T4)

Namespace `sheepdog::platform`. Nothing here changes a file; writes arrive with `FsOps` (v0.2).

- **Errors.** `Error{std::error_code code; NativePath path}` with `message()`.
  `Result<T>` holds a value or an `Error` (`has_value()`, `operator bool`, `value()`, `*`, `->`,
  `error()`; using the wrong side calls `std::terminate`). `Result<void>` is built from `Ok{}`.
  POSIX codes use `std::generic_category()`, Windows codes `std::system_category()`
  (GetLastError values); test "not found" with `is_not_found(code)`.
- **Paths.** `NativePath` wraps the OS's bytes: raw bytes on Linux, WTF-8 of the UTF-16 path on
  Windows (unpaired surrogates survive). These are what the catalog stores in path BLOBs.
  `path / name` joins with `kSeparator`; `filename()`, `parent()` (keeps a root such as `/` or
  `C:\`). `display_string()` gives valid UTF-8 (U+FFFD for invalid bytes and surrogates), for the
  UI only. `wtf8_from_utf16()` / `utf16_from_wtf8()` are plain functions on both OSes.
  Windows only: `win32_path()` makes a path absolute (`GetFullPathNameW` for relative ones) and adds
  `\\?\` or `\\?\UNC\`; absolute paths are normalised by `add_long_path_prefix()` itself, so trailing
  dots and spaces in names are kept. `native_from_wide()` goes back and drops the prefix.
- **Listing.** `list_dir(dir)` → `vector<DirEntry{name, FileDetails details, error_code error}>`,
  without `.`/`..`, sorted by bytes. A `dir` that is itself a link fails with
  `errc::too_many_symbolic_link_levels`; links inside are `EntryKind::link` and never followed.
  Linux: `open(O_NOFOLLOW|O_DIRECTORY)` + `readdir` + `statx(AT_SYMLINK_NOFOLLOW)`; entries that
  vanish are skipped, others that fail keep `error`. Windows: `FindFirstFileExW(FindExInfoBasic,
  LARGE_FETCH)`; only name-surrogate reparse points (symlinks, junctions) are links, so OneDrive
  placeholders and dedup files stay files/folders.
- **Details.** `FileDetails{kind, size, mtime_ns, btime_ns (0 if unknown), attributes
  (FILE_ATTRIBUTE_* or st_mode), cloud_only}`; times are ns since 1970 UTC. `cloud_only` is set
  for RECALL_ON_DATA_ACCESS / RECALL_ON_OPEN / OFFLINE (always false on Linux).
  `file_details(path)` describes a link itself (Windows: a handle opened with
  `FILE_FLAG_OPEN_REPARSE_POINT` and `FILE_READ_ATTRIBUTES`, which does not recall placeholders).
- **Identity.** `file_identity(path)` → `FileId{uint64 volume; array<uint8,16> file}`: Windows
  FILE_ID_INFO (64-bit volume serial + 128-bit id, fallback `nFileIndex`); Linux `st_dev` +
  `st_ino` (little-endian). Stable across renames within the volume. `volume` is per boot; the
  catalog should store `file` as `fs_id` and refer to the volume by `VolumeInfo::uid`.
- **Volumes.** `volume_info(path)` → `VolumeInfo{uid, label, fs_type, mount_root, case_sensitive,
  media (unknown/hdd/ssd), remote}`. uid: `win:<16 hex>` (the FILE_ID_INFO volume serial, so it
  matches `FileId::volume`); Linux `uuid:<UUID>` from `/dev/disk/by-uuid`, else `fsid:<hex>` from
  `statvfs`, else `dev:maj:min`. Linux reads `/proc/self/mountinfo` and
  `/sys/dev/block/M:m/queue/rotational`; Windows uses `GetVolumePathNameW`,
  `GetVolumeInformationW`, `GetDriveTypeW` and the seek-penalty IOCTL (no admin rights needed).
  `case_sensitive` comes from `probe_case_sensitive(dir)`: it looks up an existing name with its
  ASCII case swapped and compares identities (read-only); the fallback is insensitive on Windows
  and on vfat/exfat, sensitive elsewhere.
- **Reading.** `FileReader::open(path)` (Linux `O_NOFOLLOW`, sequential advice; Windows
  `FILE_FLAG_SEQUENTIAL_SCAN`, follows links, so open only `EntryKind::file` entries), then
  `read(span<byte>)` → bytes read, 0 at the end. Move-only.

### gui/main_window.hpp

- `sheepdog::gui::window_title()`: `"Sheepdog " + version()`.
- `sheepdog::gui::MainWindow`: empty `QMainWindow` with that title. Menus, docks and the file
  table come with 0.1-T8.

## Catalog schema

No catalog yet (schema v1 arrives with 0.1-T5).

## Thread model

Nothing threaded yet. Planned (DESIGN.md, "Architecture"): GUI thread, one `std::jthread` per job,
a bounded worker pool, one DB-writer thread, a query thread.
