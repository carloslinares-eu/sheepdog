# Sheepdog architecture

This file is the working memory of the code: module APIs, the schema version and the thread
model, kept current by every task that changes them. The reasoning behind the design is in
[DESIGN.md](DESIGN.md).

## Build targets

| Target | Kind | Links | Notes |
|---|---|---|---|
| `sheepdog_core` (`sheepdog::core`) | static library | sqlite3 (private), Threads | The engine. No Qt, ever. Include root is `src/`, so headers are included as `"core/..."`. No public header includes `sqlite3.h`. |
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
    location.cpp     is_unc_path, is_remote_fs_type, is_network_location (OS-neutral)
    win_util.hpp     internal Windows helpers (Handle, open_for_attributes, read_file_id)
  catalog/       the SQLite catalog (Qt-free, no #ifdef)
    errors.hpp       CatalogErrc + catalog_category()
    sqlite.hpp       Database, Statement, Transaction (RAII), sqlite_category()
    schema.hpp       kSchemaVersion, migrations(), check_identity(), migrate()
    backup.hpp       backup_catalog, list_backups, newest_backup, backup_stamp
    catalog.hpp      open_catalog, open_catalog_read_only, check_integrity, check_catalog_location
    writer.hpp       CatalogWriter (the DB-writer thread), FileRow
src/cli/       sheepdog-cli (main.cpp: --version, --help)
src/gui/       sheepdog_gui + sheepdog (MainWindow, window_title())
tests/         Catch2 tests (platform_win_test.cpp only on Windows, platform_linux_test.cpp
               only on Linux; catalog_test.cpp, catalog_writer_test.cpp on both)
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

- **Errors.** `Error{std::error_code code; NativePath path; std::string detail}` with `message()`
  (`"<code message>: <detail> (<path>)"`; `detail` is optional, e.g. SQLite's own message).
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
  **Size rule (both OSes):** `size` is the byte count for `EntryKind::file` only and 0 for
  folders, links and other entries. Linux would report a symlink's `st_size` (the length of its
  target path), Windows reports 0; Sheepdog stores 0, so the catalog does not depend on the OS.
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
- **Network locations.** `is_network_location(path)` is true for a UNC path (Windows; recognised
  from its text without network access: `\\server\share`, `//server/share`, `\\?\UNC\...`),
  a mapped network drive (`GetDriveTypeW`) or a remote filesystem on Linux (`is_remote_fs_type`:
  nfs*, cifs, smb*, fuse.sshfs, 9p, afs, ceph, glusterfs, fuse.rclone, davfs). A path that does
  not exist yet is judged by its nearest existing folder.
- **Own files.** `remove_app_file(path)` deletes one of Sheepdog's own files (old catalog
  backups). Never for user files: those change only through `FsOps` (v0.2).
- **Reading.** `FileReader::open(path)` (Linux `O_NOFOLLOW`, sequential advice; Windows
  `FILE_FLAG_SEQUENTIAL_SCAN`, follows links, so open only `EntryKind::file` entries), then
  `read(span<byte>)` → bytes read, 0 at the end. Move-only.

### core/catalog (0.1-T5)

Namespace `sheepdog::catalog`. Errors are `platform::Result<T>`: SQLite failures carry the
extended result code in `sqlite_category()` and `sqlite3_errmsg` as `detail`; the catalog's own
refusals use `CatalogErrc` (`corrupt`, `network_location`, `newer_schema`, `not_migrated`,
`not_a_catalog`, `wal_unavailable`) in `catalog_category()`, comparable as
`error.code == CatalogErrc::corrupt`.

- **SQLite wrapper.** `Database::open(path, OpenMode::{read_only, read_write, create})`
  (extended codes, 5 s busy timeout, `SQLITE_OPEN_NOMUTEX`: one thread at a time, movable
  between threads); `exec`, `prepare` -> `Statement`, `query_int`, `query_text`,
  `last_insert_rowid`. `Statement`: chained `bind(i, int64|string_view)`, `bind_blob`,
  `bind_null` (1-based; a failed bind surfaces at the next `step`), `step()` -> `Result<bool>`
  (true = row), `run()`, `reset()`, `column_int64/text/blob`, `is_null` (0-based).
  `Transaction::begin(db, Kind::immediate)` -> `commit()`; the destructor rolls back.
- **Opening.** `open_catalog(path)` is the only read-write entry point: refuses network
  locations, opens (creating the file; the folder must exist), runs `PRAGMA integrity_check(3)`,
  then `check_identity()` (so a foreign file is refused before anything is written to it), then
  sets `journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`, `trusted_schema=OFF`,
  `wal_autocheckpoint=10000` (fewer checkpoint fsyncs: a 100k-row scan writes about twice as
  fast) and `journal_size_limit=64 MiB`, then `migrate()`. Damage (integrity findings,
  `SQLITE_CORRUPT`, `SQLITE_NOTADB`) becomes `CatalogErrc::corrupt` with a detail naming the
  finding and `restore_advice()`: the newest backup's path and "close Sheepdog and copy it over
  the catalog", or "No backup was found". `open_catalog_read_only(path)` opens with
  `query_only=ON`, runs the same `check_identity()` and needs a catalog already at
  `kSchemaVersion` (else `not_migrated`); it runs no integrity check.
- **Migrations.** `PRAGMA user_version` is the schema version; `PRAGMA application_id` =
  `kApplicationId` (`0x53484447`, "SHDG") marks a Sheepdog file. `check_identity(db)` ->
  `Result<int>` reads both without writing and refuses a foreign database (tables but version 0,
  or any version >= 1 without `kApplicationId`) and a newer version. `migrate(db)` calls it, makes a
  backup **before** the first migration (also for a brand-new, empty catalog), then runs each
  pending `Migration{version, sql}` in its own transaction, which also sets `user_version`. On a
  current catalog it does nothing and makes no backup. A new schema version = append to
  `kMigrations` in `schema.cpp` and bump `kSchemaVersion`.
- **Backups.** `backup_catalog(db, version)` copies with `sqlite3_backup` to
  `<catalog file>.bak-<YYYYMMDDTHHMMSSmmm>Z-v<version>` next to the catalog (UTC, fixed width, so
  names sort oldest first; it never overwrites, it steps the stamp instead) and deletes all but
  the newest `kKeepBackups` (10). `list_backups(catalog)` (oldest first), `newest_backup(catalog)`.
- **Writer.** `CatalogWriter(Database db, WriterOptions{batch_size = 1000, queue_capacity = 8000,
  max_delay = 50 ms})` owns the read-write connection on its own `std::jthread`.
  `upsert_file(FileRow)` inserts or updates by `(root_id, rel_path)` (status back to `present`;
  a changed size or mtime clears `quick_fp`, `blake3` and `meta_ver`); `execute(Op)` runs any
  `Result<void>(Database&)` inside the batch transaction (no BEGIN/COMMIT inside). Both block
  while the queue is full. A batch commits when full, when `flush()` waits, or `max_delay` after
  its first item. A failing item rolls its whole batch back; `flush()` waits until everything
  queued is committed and returns (then clears) the first error. `stats()`: committed items,
  transactions, failed items. The destructor commits what is queued, then joins.
  100k rows take about 0.8 s (Windows Release) and 1.3 s (carbon container, Release).

### gui/main_window.hpp

- `sheepdog::gui::window_title()`: `"Sheepdog " + version()`.
- `sheepdog::gui::MainWindow`: empty `QMainWindow` with that title. Menus, docks and the file
  table come with 0.1-T8.

## Catalog schema

**Version 1** (`kSchemaVersion = 1`, `src/core/catalog/schema.cpp`), STRICT tables. Paths are
stored twice: native bytes in a BLOB (WTF-8 on Windows) and UTF-8 `display` text. Times are
nanoseconds since 1970 UTC.

```sql
volume(id INTEGER PRIMARY KEY, uid TEXT NOT NULL UNIQUE, label TEXT, fs_type TEXT,
       case_sensitive INTEGER, rotational INTEGER NULL, online INTEGER DEFAULT 1)
root(id, volume_id -> volume, path BLOB, display TEXT, added INTEGER, UNIQUE(volume_id, path))
file(id, root_id -> root ON DELETE CASCADE, rel_path BLOB, display TEXT, name_key TEXT,
     size INTEGER, mtime_ns, btime_ns DEFAULT 0, fs_id BLOB, quick_fp BLOB, blake3 BLOB,
     kind INTEGER,              -- platform::EntryKind: 0 file, 1 directory, 2 link, 3 other
     ftype TEXT, taken TEXT, taken_off INTEGER, taken_src TEXT, taken_conf INTEGER,
     status TEXT DEFAULT 'present' CHECK(status IN ('present','missing','ignored')),
     last_seen INTEGER, meta_ver INTEGER DEFAULT 0, UNIQUE(root_id, rel_path))
meta(file_id -> file ON DELETE CASCADE, key TEXT, txt TEXT, num REAL,
     PRIMARY KEY(file_id, key)) WITHOUT ROWID
-- indexes: root(volume_id), file(blake3), file(size, quick_fp), file(fs_id),
--          meta(key, num), meta(key, txt)
```

`file.size` follows the platform size rule: 0 for anything but a regular file (links included).
Tag, rule, layout, batch/op (journal) and FTS tables arrive with later migrations.

## Thread model

- **DB writer** (0.1-T5): one `CatalogWriter` thread owns the only read-write connection;
  producers queue work (a bounded queue; they block when it is full) and never call SQLite.
  The mutex guards the queue, the error slot and the stats only; SQLite runs outside it.
- **Readers**: each reading thread opens its own `open_catalog_read_only()` connection (WAL lets
  readers run while the writer commits). A connection is used by one thread at a time.
- Planned (DESIGN.md, "Architecture"): GUI thread, one `std::jthread` per job, a bounded worker
  pool, a query thread.
