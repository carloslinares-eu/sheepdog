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
  jobs/          the job framework (Qt-free, no #ifdef)
    clock.hpp        Clock, system_clock()
    errors.hpp       JobErrc + job_category(), ErrorLog, catch_exceptions()
    chunks.hpp       kChunkSize, ChunkSource, read_chunks()
    progress.hpp     Work, JobState, Snapshot, SpeedWindow, Progress
    pool.hpp         BoundedPool, TaskGroup, volume_limit(), default_worker_count()
    job.hpp          JobKind, JobContext, JobBody, FileChangeLock, Job, JobRunner
src/cli/       sheepdog-cli (main.cpp: --version, --help)
src/gui/       sheepdog_gui + sheepdog (MainWindow, window_title())
tests/         Catch2 tests (platform_win_test.cpp only on Windows, platform_linux_test.cpp
               only on Linux; catalog_*_test.cpp and jobs_*_test.cpp on both)
  support/temp_tree.hpp  TempDir (unique folder under the system temp, removed without
                         following links), make_dir, write_file, rename_path, make_dir_link
                         (symlink / junction), remove_tree; native calls, NativePath names
  support/jobs_support.hpp  FakeClock, eventually() (wait for another thread), Gate, volume(uid,
                         media), start_job()
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

### core/jobs (0.1-T6)

Namespace `sheepdog::jobs`: the framework around long-running work. Concrete jobs (scan, hash,
apply) arrive with their subjects. Failures are `platform::Result<T>`; the framework's own codes
are `JobErrc` (`cancelled`, `busy`, `exception`) in `job_category()`, comparable as
`error.code == JobErrc::busy`.

```cpp
JobRunner runner;
auto job = runner.start("Hash", JobKind::read_only, [&](JobContext& context) -> Result<void> {
    context.progress().begin_step("Hashing", {.files = files.size(), .bytes = total});
    auto tasks = context.tasks(); // after what the tasks use: its destructor waits for them
    for (const File& file : files) {
        const bool queued = tasks.submit(file.volume, [&context, &file]() -> Result<void> {
            auto reader = FileReader::open(file.path);
            if (!reader) {
                return reader.error(); // goes into the job's ErrorLog; the job carries on
            }
            Hasher hasher;
            auto read = read_chunks(*reader, context.stop_token(), [&](auto chunk) -> Result<void> {
                hasher.update(chunk);
                context.progress().add({.files = 0, .bytes = chunk.size()});
                return Ok{};
            });
            ...
        });
        if (!queued) {
            break; // stopped
        }
    }
    return Ok{};
});
```

- **Jobs.** `JobRunner(clock = system_clock(), workers = default_worker_count())` starts jobs and
  owns the pool they share; one per application is enough, and the clock must outlive it.
  `start(name, kind, body)` -> `Result<shared_ptr<Job>>` runs
  `JobBody = Result<void>(JobContext&)` on a `std::jthread` of its own. The destructor stops the
  jobs still running and waits for them. Finished jobs are dropped at the next `start()`, but a
  `Job` handle stays valid for as long as it is held. `Job`: `name()`, `kind()`, `snapshot()`,
  `errors()`, `request_stop()` (does not wait), `wait()` (any number of times, from any thread but
  the job's own), which says how it ended: Ok, a `JobErrc::cancelled` error, or the body's error.
- **How a job ends.** `Snapshot::state` goes from `running` to `completed`, `cancelled` or `failed`.
  An error return or an exception (`JobErrc::exception`, with `what()` as detail) means failed and
  goes into the log as well. A body that returns Ok after a stop was requested counts as cancelled,
  as does one returning `JobErrc::cancelled`; an error wins over a stop. Errors of single files do
  not fail a job, so `completed` can come with a non-empty log. When the state is no longer
  `running`, the tasks are done and the file-change lock is free.
- **JobContext.** `stop_token()`, `stop_requested()`, `progress()`, `errors()`, `tasks()`. Its
  accessors may be called from the job's pool tasks. Jobs check for a stop per item, and per
  chunk with `read_chunks()`.
- **File-change lock.** `JobKind::changes_files` jobs hold `FileChangeLock`, a process-wide flag, so
  a second `JobRunner` cannot get around it; `read_only` jobs do not take it. While one runs,
  `start()` refuses another with `JobErrc::busy` rather than queueing it: its plan was checked
  against the disk as it is now, and a double click must not run a batch twice. A second Sheepdog
  process is not covered.
- **Pool.** `BoundedPool(workers)`: worker threads shared by all jobs, so that the limits hold
  across jobs. `queue_capacity()` is workers x `kQueuePerWorker` (4) waiting tasks; a producer
  blocks beyond that, so memory stays flat. At most `volume_limit(media)` tasks of one volume run at
  a time (`kHddLimit` 2, `kSsdLimit` 8; unknown counts as an HDD). The queue is one FIFO: a worker
  takes the first task whose volume has a free slot. So a volume at its limit holds back only its
  own tasks, though a slow volume can still fill the shared queue and slow down the producers of
  the others. `stats()`: `queued`, `running`, `peak_queued`, `completed` (`running` and `completed`
  trail `TaskGroup::wait()` by a moment).
- **TaskGroup.** `TaskGroup(pool, stop, errors)`, or `context.tasks()`, is how work enters the pool.
  `submit(VolumeInfo, Task)` with `Task = Result<void>()` blocks while the queue is full and returns
  false once a stop was requested, also while blocked. A failing or throwing task goes into the log.
  After a stop, queued tasks are skipped; running ones must check the token themselves. `wait()`
  blocks until the group's tasks are over and their captures destroyed; the destructor waits too,
  so declare the group after what its tasks use, and an early return is safe. A task must not
  submit tasks: with every worker waiting for room in the queue nothing would make room.
- **Chunks.** `read_chunks(source, stop, sink)` reads any `ChunkSource` (`FileReader`, a fake) to its
  end in chunks of `kChunkSize` (1 MiB), passes each to `sink(span<const byte>)` -> `Result<void>`,
  and returns the bytes read. The token is checked before every read, so a stop takes effect within
  one chunk: the chunk under way is delivered, the next one is not read, and the result is
  `JobErrc::cancelled`.
- **Progress.** `Progress(clock)`, reached through `context.progress()`. A job is a series of steps,
  each with a phase name and totals: `begin_step(phase, total)`, `add_total(work)` when a step finds
  more, `set_job_total(total)` for a job that knows its later steps; without that, the job's total
  is the work of finished steps plus the current step's total. `add(Work{files, bytes})` reports
  finished work from any thread. `snapshot()` builds a `Snapshot`, swaps it into the
  `shared_ptr<const Snapshot>` under the mutex and returns that, so a job that stalls still shows
  its speed falling; `add()` never builds one. After `finish(state)`, which the runner calls, the
  snapshot is frozen.
- **The maths.** Speed = the work added in the last `kSpeedWindow` (5 s) of the current step, divided
  by 5 s, or by the step's age while that is shorter. Work within `kSpeedResolution` (100 ms) is
  added up into one entry, which leaves the window as a whole, so the window holds about 50 entries
  at most and a steady stream reads up to 2 % low. `begin_step()` starts the speed over, because
  the next step may run at a very different pace. `eta` is (total - done) / speed for the whole job,
  `step_left` the same for the step: in bytes when that total has bytes, else in files. Both are
  empty while the total or the speed is 0, and 0 when nothing is left. A completed job shows 0 for
  both, a cancelled or failed one neither. `elapsed` runs from the job's start to its end. A total
  of 0 means "not known". Times are `Seconds`, a `duration<double>`.
- **Clock.** `Clock::now()` -> steady time point, called from several threads at once.
  `system_clock()` is the real one; `test::FakeClock` (`tests/support/jobs_support.hpp`) moves only
  when a test says so.
- **Errors.** `ErrorLog` is thread-safe: `add(error)` ignores `JobErrc::cancelled`, keeps the first
  `kKeptErrors` (1000) and counts them all; `count()`, `kept()`. `catch_exceptions(fn)` wraps every
  body and task, so no exception crosses a thread.

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
- **Jobs** (0.1-T6): one `std::jthread` per job runs its body; a stop comes through that thread's
  `stop_token`. Only one `changes_files` job runs at a time in the process.
- **Worker pool** (0.1-T6): one `BoundedPool` per `JobRunner`, shared by all its jobs. Job threads
  are the producers and block while the queue is full; a stop wakes them. The mutex guards the
  queue and the counters; tasks run outside it.
- **Progress** (0.1-T6): workers add work under `Progress`'s mutex (arithmetic only). A poller calls
  `Job::snapshot()` from any thread and may keep the immutable `Snapshot` it gets.
- Planned (DESIGN.md, "Architecture"): GUI thread, a query thread.
