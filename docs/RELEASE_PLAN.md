# Sheepdog release plan (working name)

Created 2026-10-31. This file is the single source of truth for the work: change the plan here, not in chat, and tick tasks off as they are committed. Design details: [DESIGN.md](DESIGN.md).

> **Toolchain change (2026-10-31):** Windows builds use Qt 6.12.0's MinGW kit (`C:\Qt\6.12.0\mingw_64`) with MinGW-w64 GCC 13.1, CMake 3.30 and Ninja 1.12 from `C:\Qt\Tools`, not MSVC 2022: the Qt install contains the MinGW kit, and Build Tools 2022 has no C++ workload. The code stays free of compiler-specific extensions, so an MSVC build can be added later.


## Decisions needed before 0.1-T2

- [x] Which files matter most: photos (HEIC/RAW), phone videos, music or documents? The answer sets the order within v0.5 and how much thumbnail work is worth it.
  **Answer (2026-11-04):** mostly documents and photos. The thumbnail work in v0.7 stays. v0.5 is reordered to photos and videos (0.5-T1), then documents with XMP/IPTC keywords (0.5-T2), then audio (0.5-T3).
- [x] Roughly how many files (100k or several million), and where are they stored: internal SSD/HDD, external drives, NAS/SMB, OneDrive? The catalog itself has to stay on a local disk.
  **Answer (2026-11-04):** around 100k files. The 100k criteria stay, and the 1M test in v0.8 stays as headroom. Stored on internal disks and external drives (no NAS/SMB or OneDrive), so offline volumes matter more than network shares.
- [x] Should tags belong to each file record (the default), or to the content, so that duplicates share their tags automatically?
  **Answer (2026-11-04):** to each file record (the default).
- [x] One central catalog (the default), or an optional portable catalog on each external drive?
  **Answer (2026-11-04):** one central catalog (the default).
- [x] If move mode needs more hardening, are copy mode and smart folders enough for 1.0?
  **Answer (2026-11-04):** yes.
- [x] Are the name Sheepdog, the MIT licence and an English + German UI OK?
  **Answer (2026-11-04):** yes.
- [x] Do you want code signing (a free open-source signing programme or a paid certificate), and Linux formats beyond AppImage (Flatpak, .deb)?
  **Answer (2026-11-04):** a free open-source signing programme (for example the SignPath Foundation; apply once v0.1.0 is public), and yes to Flatpak and .deb. Both go into 1.0: new task 0.9-T2.
- [x] Should tags ever leave the catalog? Recommendation: XMP sidecars and user.xdg.tags as opt-in exports after 1.0, and never writing into the files themselves.
  **Answer (2026-11-04):** follow the recommendation.

## Review follow-ups

Optional notes from the task reviews that no task card covers yet. Fold each into the task that touches the code, or tick it with a reason.

- [ ] Catalog backups (from 0.1-T5): if `sqlite3_backup_init` or `sqlite3_backup_step` fails (disk full, I/O error), the partial file keeps a valid `<catalog>.bak-<stamp>-v<N>` name, and `newest_backup()` would offer it for a restore. Write to a temporary name and rename it on success, or remove the target on failure. Do this before the first real migration (v0.2).
- [ ] CatalogWriter (from 0.1-T5): an error raised in the destructor, or one never collected by `flush()`, is dropped silently. Log it, or document that callers must `flush()` before destruction.
- [ ] Catalog path (from 0.1-T5): `Database::open` passes WTF-8 bytes to `sqlite3_open_v2` as UTF-8, so a Windows catalog path with unpaired surrogates, or one longer than MAX_PATH, may fail inside SQLite's win32 VFS. Unlikely under `%LOCALAPPDATA%`; at least add a comment.
- [ ] Network refusal tests (from 0.1-T5): the Linux path is unit-tested only on fs-type strings, and the Windows mapped-drive path (GetDriveTypeW) has no test. Inject a VolumeInfo with `remote = true` into `check_catalog_location`.
- [ ] Backup slots (from 0.1-T5): every new catalog keeps a backup of an empty v0 database, which uses one of the 10 slots. Harmless; consider skipping the backup when v0 is empty.
- [ ] Path decoder (from 0.1-T4): `utf16_from_wtf8` accepts a surrogate pair encoded as two 3-byte sequences (non-canonical WTF-8), so two NativePath byte strings can reach the same file. A strict decoder would keep the catalog's BLOB keys unique.
- [ ] Includes (from 0.1-T4): `native_path.hpp` uses `std::move` without `#include <utility>`.
- [ ] Windows listing (from 0.1-T4): `list_dir` checks that a folder is not a link through a handle, then enumerates by path with FindFirstFileExW; enumerating through the handle (`GetFileInformationByHandleEx` with FileIdBothDirectoryInfo) closes the gap.
- [x] CI on GitHub (from 0.1-T4): the test 'relative paths and drive roots work' asserts `media != Media::unknown` for C:\, which the windows-2025 runner's virtual disk may not answer. Check on the first CI run after 0.1-T4. Done: it did not, so on CI (`test::on_ci()`) the test no longer asserts this; `volume_limit()` treats unknown as an HDD.
- [x] CI timing (from 0.1-T5): the 100k-row writer test takes about 1.3 s on carbon against the 2 s limit (about 0.8 s on Windows), so it may get flaky on a loaded CI runner. Done: it took 2.9 s on the Windows runner, so CI allows 10 s; the 2 s limit still holds elsewhere.
- [ ] File-change lock (from 0.1-T6): `FileChangeLock` covers one process, so `sheepdog-cli apply` can still run next to the GUI's own batch. The executor (v0.2) needs a lock across processes as well, for example a lock file next to the catalog.
- [ ] Shared queue (from 0.1-T6): all volumes share the queue of workers x 4 tasks, so a slow HDD can fill it and hold the producer of a fast SSD back to the HDD's pace. Tasks of the SSD still overtake those of the HDD once they are queued. If jobs on several volumes turn out to run side by side, give each volume a share of the queue (0.8 performance work).
- [ ] Stop wording (from 0.1-T6): the comment on `TaskGroup::submit` says it returns false once a stop was requested, but a stop that lands just before the wait in `BoundedPool::submit` can still queue the task and return true. This is harmless, because the task is skipped and `wait()` stays correct; only the comment is too strong.
- [ ] HDD limit test (from 0.1-T6): 'tasks of one volume stay within its limit' asserts peak <= limit only, and the exact limit has a test of its own only for SSDs ('an SSD gets eight tasks'). Nothing asserts directly that an HDD volume reaches 2 under load; 'a volume at its limit holds back only its own tasks' covers it indirectly.

## How to work through this plan

**One task = one fresh session = one commit.** Run `/clear` between tasks.
- **Start each session with:** *"Read CLAUDE.md, docs/ARCHITECTURE.md and task 0.2-T4 in docs/RELEASE_PLAN.md. Propose a short plan, then implement."*
- **A task is done when:**
  - its acceptance criteria pass through `scripts/test.ps1` (Windows) and `scripts/linux-test.ps1 test` (carbon);
  - CHANGELOG has a line under *Unreleased*;
  - the task is ticked in RELEASE_PLAN.md;
  - there is one commit, e.g. `0.2-T4: planner`.

**What goes in CLAUDE.md** (at most 150 lines, kept current):
- build and test commands, including how to run a single test (`ctest -R planner`, Catch2 tags);
- a code map: directory → responsibility → key types;
- conventions: no Qt in core, `Result<T>`, `from_chars`, no `std::format`, files under 500 lines;
- safety rules: files are changed only through `FsOps`; the executor runs only under `SHEEPDOG_SANDBOX`; `apply` is never run on real folders;
- what not to read: `build*/`, `vcpkg_installed/`, binary test files, `.ts` translation files.

**Keep the docs as the memory**
- **docs/ARCHITECTURE.md** holds module APIs, the schema version and the thread model, so a fresh session doesn't have to re-explore the code. Any task that changes an interface updates it.
- **docs/RELEASE_PLAN.md** is the single source of truth. Change the plan there, not in chat.

**Effort levels**
- *medium* for GUI wiring, packaging and docs.
- *high* for parsers, planner, executor, rename/move detection, the query language and performance.
- *ultracode* only for 0.9-T3.
- A `/code-review high` of `exec/` and `platform/` at the end of v0.2 and of v0.4 is worth the tokens, because those are the modules that could lose data.
- Use **plan mode** for high-effort tasks, so you agree on the approach before any code is written.

**Session discipline**
- **Never mix GUI and engine work** in one session. GUI task cards list the exact widgets and signals, because Qt boilerplate uses many tokens.
- **When a session grows** (context over about 60 %, or the task balloons): stop at the last state where tests pass, commit, write *Remaining:* into the task card and continue in a fresh session. Don't `/compact` your way through a long debugging session.
- **Keep tool output small:** run only the tests you need, write long logs to a file and grep them, and use `gh run view --log-failed` for CI failures.

**Budget.** 39 tasks at 4–5 sessions a week takes about 9–10 weeks. Put the high-effort tasks early in the week, and keep about 15 % of the weekly limit for fixes.

**Closing a milestone** (you, about 10 minutes):
1. Finish the CHANGELOG section.
2. Save `tests/data/schema/catalog-v0.N.db`.
3. Run `scripts/release.ps1 0.N.0`.
4. Check the release files on GitHub.


## Environment setup (task 0.1-T1)

This is **task 0.1-T1**. You do the installs yourself (about an hour); Claude then writes `docs/DEVELOPMENT.md` and `scripts/check-env.ps1`. Nothing Docker- or WSL-related is needed on Windows: Linux containers run on carbon.

1. **Long paths.** Enable Win32 long paths (registry `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`, `LongPathsEnabled = 1`, needs admin) and run `git config --global core.longpaths true`.
2. **Compiler.** Windows builds use the MinGW-w64 GCC 13.1 toolchain that the Qt installer put in `C:\Qt\Tools\mingw1310_64`. Nothing else to install. (MSVC would need the C++ workload in Build Tools 2022 plus Qt's MSVC kit; it can come later as an extra CI job.)
3. **vcpkg.** Done on 2026-10-31: the clone at `F:\Workspace\vcpkg` is current (tool 2026-07-27) and `VCPKG_ROOT` is set. Sheepdog uses the community triplet `x64-mingw-static` for both target and host, so zebra's MSVC builds are unaffected.
4. **Qt 6.12.0 for MinGW.** Done on 2026-10-31 with the Qt Online Installer into `C:\Qt` (kit `C:\Qt\6.12.0\mingw_64`). Qt SVG, Linguist and windeployqt come with the base package. Add Additional Libraries → Qt Image Formats with the Qt Maintenance Tool before thumbnails (v0.7). The kit path goes into the git-ignored `CMakeUserPresets.json`.
5. **CMake and Ninja.** Use CMake 3.30 and Ninja 1.12 from `C:\Qt\Tools\CMake_64` and `C:\Qt\Tools\Ninja`; the build scripts put them on PATH. The old CMake 3.25 on PATH is not used.
6. **CLion.** Settings → Toolchains → + MinGW → `C:\Qt\Tools\mingw1310_64`, CMake `C:\Qt\Tools\CMake_64\bin\cmake.exe`, moved to the top. Enable the `dev-debug` and `dev-release` presets from the git-ignored `CMakeUserPresets.json` (created by `scripts\build.ps1`; they inherit `win-debug`/`win-release` and add the Qt path) and remove the default profile.
7. **carbon over SSH.** Done on 2026-10-31. This is in `%USERPROFILE%\.ssh\config` so `ssh carbon` works for scripts, Claude and CLion:
   ```
   Host carbon
       HostName carbon
       User carlos
       IdentityFile ~/.ssh/cicero_carbon
       IdentitiesOnly yes
   ```
   Already done on 2026-10-31: `carlos` is in the `docker` group on carbon and `docker run --rm hello-world` works.
8. **Optional: GitHub CLI.** `winget install GitHub.cli` lets Claude read failing CI logs itself.
9. **Optional: Windows Sandbox**, for testing the zip and installer on a clean machine (needs VT-x enabled in the BIOS): `Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All`.


## Overview

| Version | Milestone | You get | Tasks |
|---|---|---|---|
| v0.1.0 | Read-only catalog | Download a zip or AppImage from GitHub, add several folders at once, watch a scan with speed, ETA and a Stop button, and browse a sortable file table with an Inspector. | 10 (1 S, 9 M) |
| v0.2.0 | Organise by copy | Pick a root or a selection, choose a layout such as Photos/{taken:%Y}/{taken:%Y-%m}/{name}{ext}, check the before/after tree and its conflicts, copy into an archive folder, and undo from History. | 8 (2 S, 6 M) |
| v0.3.0 | Tags and queries | Drag files onto tags, see automatic Year, Camera and Type tags, search with tag:Trips/* -tag:Reviewed, save smart folders, and organise with tag:Trips/* -> Trips/{tag:Trips/*}/{name}{ext}. | 5 (0 S, 5 M) |
| v0.4.0 | Move with undo | The Organise wizard offers Move (beta), History can undo and redo moves, and after a crash the app offers Resume, Roll back or Leave. | 4 (0 S, 4 M) |
| v0.5.0 | Rich metadata | Phone videos sort by their real capture date, music by artist and album, documents by creation date, and Lightroom keywords show up as tags. | 3 (0 S, 3 M) |
| v0.6.0 | Reconcile and drives | Files renamed or moved in Explorer keep their tags, unplugged drives show as offline, a Missing view relinks files, and a Duplicates view groups identical files and syncs their tags. | 2 (0 S, 2 M) |
| v0.7.0 | Visual browsing | A zoomable thumbnail grid, a German translation, and a first-run wizard that gets a new user to a preview in minutes. | 2 (0 S, 2 M) |
| v0.8.0 | Scale and beta hardening | A library of 1M files stays responsive, and the beta has no known high-severity bugs. | 2 (0 S, 2 M) |
| v0.9.0 | Release candidate | A Windows installer with an icon, signed Windows files, .deb and Flatpak packages, a complete user guide, and a release candidate tested on a copy of real data. | 3 (0 S, 3 M) |
| v1.0.0 | Sheepdog 1.0 | sheepdog-1.0.0 on GitHub with a signed installer, portable zip, AppImage, .deb, Flatpak bundle and checksums; catalogs from every beta upgrade cleanly. | 1 (1 S, 0 M) |

Total: 39 tasks. S = up to about 300 changed lines, M = about 300-800; nothing bigger, so split a task that grows.


## Milestones

The `docker compose run ...` commands in the task cards run on carbon, through `scripts/linux-test.ps1 <service>`.


### v0.1.0 Read-only catalog

**Goal:** Set up the toolchain, the repo, CI and the Linux test container, then scan folders into a catalog and browse it. Nothing is ever written to user folders.  
**You get:** Download a zip or AppImage from GitHub, add several folders at once, watch a scan with speed, ETA and a Stop button, and browse a sortable file table with an Inspector.

- [x] **0.1-T1 Toolchain on this machine** (S; effort medium; after: none)
  - Scope: Done on 2026-10-31: vcpkg updated and VCPKG_ROOT set; Qt 6.12.0 for MinGW installed in C:\Qt with MinGW 13.1, CMake and Ninja; the `Host carbon` SSH entry added (see Environment setup). Claude turns the drafted docs/RELEASE_PLAN.md and docs/DESIGN.md into the start of the repo (git init, first commit) and adds docs/DEVELOPMENT.md, scripts/check-env.ps1 and a throwaway hello-Qt project that is not committed. Out: the real project structure.
  - Done when:
    - scripts/check-env.ps1 reports every check green: Qt 6.12 MinGW kit, MinGW 13.1, CMake, Ninja, VCPKG_ROOT, and `ssh carbon docker info`
    - the hello-Qt window builds and runs from CLion (MinGW toolchain) and from the terminal
    - the repo exists, with the two plan documents in its first commit
- [x] **0.1-T2 Repo bootstrap** (M; effort medium; after: 0.1-T1)
  - Scope: In: CMake targets (sheepdog_core, sheepdog-cli, a sheepdog GUI stub, tests); CMakePresets.json version 6 (win-debug, win-release, win-ci, linux-debug, linux-release, linux-asan, linux-tsan, linux-fuzz); a git-ignored CMakeUserPresets.json for the Qt path; vcpkg.json with builtin-baseline (sqlite3[fts5], blake3, catch2) and triplets x64-mingw-static (target and host) and x64-linux; warning flags with a WARNINGS_AS_ERRORS option; version from project(VERSION) plus git describe; Windows manifest and version resource; scripts/build.ps1 and scripts/test.ps1 (put MinGW 13.1, CMake and Ninja from C:\Qt\Tools on PATH; Windows PowerShell 5.1); CLAUDE.md; docs skeleton (ARCHITECTURE, USER_GUIDE, CONTRIBUTING, CHANGELOG in Keep a Changelog format, THIRD_PARTY_LICENSES); MIT LICENSE; README; .gitignore (.idea/, cmake-build-*/, build/, out/, CMakeUserPresets.json, inspection results); .clang-format, .clang-tidy, .editorconfig. Out: CI and features.
  - Done when:
    - scripts/test.ps1 configures win-debug, builds, and passes a sample Catch2 test
    - `sheepdog-cli --version` prints a git-describe version such as 0.0.0-3-gabc1234, and the empty main window shows it in the title bar
    - after a CLion build, `git status` lists no .idea, build or user-preset files
- [x] **0.1-T3 Linux test container and CI skeleton** (M; effort medium; after: 0.1-T2)
  - Scope: In: docker/ci.Dockerfile (debian:13, GCC 14, Clang 19, gdb, Qt 6.8.2 development packages with the offscreen plugin, vcpkg, a non-root user); compose.yaml (services test, asan, tsan, tidy, shell; named volumes for builds and the vcpkg cache; a tmpfs at /mnt/xdev); scripts/linux-test.ps1 (copy the working tree to carbon and run a compose service there); .github/workflows/ci.yml (a windows-2025 MinGW job and a Linux job running the container); Qt and vcpkg caching; actions pinned by SHA; an offscreen GUI smoke test. Out: nightly and release workflows.
  - Done when:
    - `scripts/linux-test.ps1 test` passes on carbon, including the offscreen GUI smoke test
    - CI passes on the Windows and Linux jobs with warnings treated as errors
    - a re-run with warm caches finishes in under 10 minutes
  - Status 2026-11-04: `linux-test.ps1 test` and `asan` pass on carbon, also with warnings as errors. GitHub CI run 41894649584 passed: Linux 1 min 22 s, Windows 6 min with cold Qt and vcpkg caches. Windows CI uses Qt 6.10.3 because aqtinstall 3.3 cannot install Qt 6.11+.
- [x] **0.1-T4 Platform layer: read side** (M; effort high; after: 0.1-T3)
  - Scope: In: the Result<T> error type; path codec (WTF-8 on Windows, raw bytes on Linux, plus a display string); sorted folder listing that never follows symlinks, junctions or reparse points; file details (size, mtime_ns, creation time, attributes, a flag for cloud-only placeholders); file identity (FILE_ID_INFO + volume serial on Windows; st_dev/st_ino + filesystem UUID on Linux); volume info (ID, label, filesystem type, case-sensitivity probe, HDD or SSD); long Windows paths (the \\?\ prefix). Out: every write operation.
  - Done when:
    - file names that are not valid UTF-8 (container) and names with unpaired surrogates (Windows) round-trip through the codec and the folder listing
    - a path longer than 260 characters can be listed and read on Windows
    - a file keeps its identity across a rename on both OSes, and a symlink or junction loop is not followed
- [x] **0.1-T5 Catalog database** (M; effort high; after: 0.1-T3)
  - Scope: In: a SQLite RAII wrapper; schema v1 (volume, root, file, meta); user_version migrations with a sqlite3_backup first; WAL and pragmas; integrity_check on open; refusal of network locations; a DB-writer thread with batched transactions; read-only connections. Out: tag and journal tables, which later migrations add.
  - Done when:
    - migration tests: empty to v1 runs once, can safely run again, and leaves a backup
    - 100k file rows go through the writer in under 2 s (Release build)
    - a corrupted catalog is refused with a clear message offering the newest backup, and a catalog path on a UNC or NFS share is refused
- [x] **0.1-T6 Job framework** (M; effort high; after: 0.1-T3)
  - Scope: In: job runner on std::jthread + stop_token; BoundedPool (queue capped at workers x 4, limits per volume); progress snapshot (phase, files and bytes done and total, speed over a sliding 5 s window, ETA, elapsed, time left in the step) published as shared_ptr<const Snapshot> swapped under a mutex; injected Clock; error collection; a lock so only one job that changes files runs at a time. Out: concrete jobs.
  - Done when:
    - fake-clock tests for the speed, ETA and time-left maths pass
    - cancel takes effect within one 1 MiB chunk (test with a slow fake reader)
    - submitting 1M tasks never exceeds the queue cap (asserted), and `docker compose run --rm tsan` reports nothing
- [ ] **0.1-T7 Scanner and CLI** (M; effort high; after: 0.1-T4, 0.1-T5, 0.1-T6)
  - Scope: In: add/list/remove roots, refusing protected ones (C:/Windows, /usr); scan job (sorted walk; incremental, skipping files whose path, size and mtime are unchanged; type detection from magic bytes; junk files skipped; OneDrive cloud-only files cataloged but never opened; offline volumes); quick fingerprint and BLAKE3 on demand in the pool, with `hash --dupes` hashing only files of equal size; batched writes; a read-only CatalogReader (sort, filter, paging) shared by CLI and GUI; CLI commands root, scan, ls --json and hash. Out: metadata parsers and rename/move detection (v0.6).
  - Done when:
    - two scans of the same temp tree give identical `sheepdog-cli ls --json` output
    - rescanning an unchanged 10k-file tree reads no file contents (a read counter is asserted)
    - `sheepdog-cli hash` matches the BLAKE3 reference vectors, and scanning 200k files keeps memory flat
    - a protected root is refused, and a file flagged as cloud-only is cataloged but never opened (test with a fake platform layer)
  - Review notes (from 0.1-T4):
    - `FileReader::open` on Windows follows reparse points and does not check for recall. Open with FILE_FLAG_OPEN_REPARSE_POINT and check the attributes after opening (GetFileInformationByHandleEx): refuse a name-surrogate reparse point or a file with RECALL_* set, so a path that became a link or a placeholder after the listing is never read through or recalled.
    - `FileReader::open` on Linux uses plain O_RDONLY, so an entry swapped for a FIFO after the listing blocks for ever. Open with O_NONBLOCK, check `fstat` for S_ISREG, then clear O_NONBLOCK.
- [ ] **0.1-T8 GUI shell: library and activity** (M; effort medium; after: 0.1-T7)
  - Scope: In: main window (menus, docks, layout saved with QSettings, light/dark following the system); Library dock (roots with online/offline state); Add Folders (port of Bloodhound's IFileOpenDialog multi-select on Windows, QFileDialog elsewhere); Scan and Stop; Activity bar polling the progress snapshot every 150 ms; About dialog (version, licences). Out: the file table.
  - Done when:
    - offscreen test: add a temp root, scan to completion, and the Activity bar shows 100 %
    - Stop cancels a 100k-file scan within 1 s
    - the window layout comes back after a restart
- [ ] **0.1-T9 GUI: file table and Inspector** (M; effort medium; after: 0.1-T8)
  - Scope: In: a paged QAbstractTableModel over CatalogReader (fetched on a query thread; sort by name, folder, size, date or kind); a name filter box; Inspector dock (path, size, dates, kind, identity, hash); open file and show in folder; dragging files out to Explorer as copies. Out: thumbnails, tags, query language.
  - Done when:
    - QAbstractItemModelTester in Fatal mode reports no problems with the model
    - 200k rows scroll with no event-loop stall over 50 ms (measured in a test)
    - selecting a row shows its metadata in the Inspector
- [ ] **0.1-T10 Packaging and release workflow** (M; effort medium; after: 0.1-T9)
  - Scope: In: portable Windows zip from windeployqt, with the VC++ runtime DLLs next to the exe; docker/appimage.Dockerfile (ubuntu:22.04, GCC 12, Qt 6.12 via aqt, linuxdeploy tools unpacked in the image) and compose services appimage, smoke-debian12 and smoke-fedora; a --selftest flag; release.yml (version check, zip, AppImage, CLI tarball, SHA256SUMS, gh release with CHANGELOG notes); scripts/release.ps1; Qt notices in THIRD_PARTY_LICENSES. Out: installer and icon (v0.9).
  - Done when:
    - `docker compose run --rm appimage` builds an AppImage whose --selftest passes in the debian:12 and fedora smoke services
    - the Windows zip passes --selftest in Windows Sandbox
    - `objdump -T` on the bundled libQt6Core shows no GLIBC symbol version newer than 2.35; if it does, the AppImage base moves to ubuntu:24.04 and the docs say so
    - tag sheepdog-0.1.0 publishes a GitHub release with the zip, AppImage, tarball and SHA256SUMS, and CHANGELOG and USER_GUIDE cover v0.1

### v0.2.0 Organise by copy

**Goal:** First safe organising: preview a layout, copy files into a new folder structure with verification, and undo it. Originals are never touched.  
**You get:** Pick a root or a selection, choose a layout such as Photos/{taken:%Y}/{taken:%Y-%m}/{name}{ext}, check the before/after tree and its conflicts, copy into an archive folder, and undo from History.

- [ ] **0.2-T1 ByteReader and EXIF/TIFF parser** (M; effort high; after: 0.1-T7)
  - Scope: In: a bounds-checked ByteReader with size and nesting limits; JPEG APP1, TIFF/DNG/CR2/NEF/ARW IFDs, PNG eXIf and WebP EXIF, reading DateTimeOriginal + OffsetTimeOriginal + SubSec, make/model/lens, orientation, dimensions, GPS and the embedded preview offset; tools/make_fixtures.py (standard library only); the fuzz_exif target, a compose fuzz service and nightly.yml (fuzzing + ThreadSanitizer). Out: storing metadata.
  - Done when:
    - the generated test files produce their expected JSON
    - `docker compose run --rm -e FUZZ_SECONDS=600 fuzz` finds no crash in fuzz_exif
    - truncated and self-referencing IFD test files return errors, and ASan reports nothing
- [ ] **0.2-T2 Extraction phase and capture date** (S; effort high; after: 0.2-T1)
  - Scope: In: an extraction phase after the scan (parallel; skipped when meta_ver is current); meta rows; date patterns in file names; capture-date resolution with source and confidence, keeping either a UTC offset or a time without zone; `sheepdog-cli meta`. Out: other formats.
  - Done when:
    - `sheepdog-cli meta` prints the capture date, source and confidence for test files dated by EXIF, by file name only and by mtime only
    - a rescan extracts nothing again when meta_ver is current
    - dates and GPS values parse the same under a German (de_DE) locale
- [ ] **0.2-T3 Templates and name cleaning** (M; effort high; after: 0.2-T2)
  - Scope: In: template parser and evaluator ({field[:fmt][|fallback]} with filters lower, upper, trim, max:N); fields from file and meta rows; layout lines 'selector -> template' with a minimal selector (kind:, ext:, *) that v0.3 replaces with the query language; name cleaning (illegal characters, reserved names, trailing dots and spaces, NFC via utf8proc, 255-byte components). Out: planner.
  - Done when:
    - golden template outputs pass, including fallbacks for missing fields
    - CON.jpg, 'a.', 'b ' (trailing space), 300-byte names and NFD input are cleaned to the expected names
    - property test: cleaning random input gives a result that is valid under Windows rules and doesn't change when cleaned again
- [ ] **0.2-T4 Planner** (M; effort high; after: 0.2-T3)
  - Scope: In: a pure plan function (files + layout + destination + mode in; ordered operations + diagnostics out) covering two files for one target (name (2).ext), clashes after case folding, existing targets (skip or rename), identical content reported as 'already present', companion groups, free space, path length, FAT32's 4 GiB limit and no-ops; `sheepdog-cli plan --json`. Out: execution, and temporary names for rename chains (v0.4).
  - Done when:
    - shuffled input produces an identical plan
    - property test with fixed seeds: targets stay unique after case folding, and companion files always share a stem and a folder
    - 20 snapshot scenarios pass, covering conflicts, already-present files, companions and the FAT32 limit
- [ ] **0.2-T5 FsOps write operations and fault injection** (M; effort high; after: 0.1-T4)
  - Scope: In: the FsOps interface and its real implementation: create_new (CREATE_NEW/O_EXCL), chunked copy with BLAKE3 hashing and stop_token, flush, copying timestamps and attributes, rename_noreplace (MoveFileExW without REPLACE_EXISTING; renameat2 with RENAME_NOREPLACE), remove, folder fsync, and 3 retries with backoff on sharing violations; FaultyFs (fail or crash at call k); the sandbox guard (SHEEPDOG_SANDBOX). Out: the journal, and fallbacks for filesystems without RENAME_NOREPLACE (v0.4).
  - Done when:
    - rename_noreplace never overwrites an existing target on either OS
    - copies hash equal to their source, and cancelling mid-file leaves no .sheepdog-partial file
    - a file held open without sharing on Windows is retried 3 times and then reported as locked
    - in a test build, changing a path outside SHEEPDOG_SANDBOX fails with a clear error
- [ ] **0.2-T6 Journal and copy executor** (M; effort high; after: 0.2-T4, 0.2-T5)
  - Scope: In: a migration adding the batch, op and layout tables (with a save/load API for layouts); journal-first execution (up to 100 operations committed as started before the disk is touched); re-checking each source before its operation; partial file, then verify, then rename; the catalog update in the same transaction as done; sqlite3_backup before each batch; clear errors for read-only targets and Controlled Folder Access; the destination registered as a root; `sheepdog-cli apply`. Out: undo, recovery, GUI.
  - Done when:
    - copying a temp tree produces exactly the planned destination manifest, and source hashes are unchanged
    - with FaultyFs failing call k, for every k in a 30-file plan, no partial file is left and a re-run completes the copy
    - a source changed after planning is skipped and reported
    - a read-only destination folder gives a clear error for that operation while the rest of the batch completes
- [ ] **0.2-T7 Undo and recovery for copy batches** (S; effort high; after: 0.2-T6)
  - Scope: In: an undo batch that deletes copies only if their hash, size and mtime still match and their source still verifies, and removes empty folders it created; startup recovery for copy batches (clean up partial files, resume or roll back); `sheepdog-cli undo` and `recover`; a harness that kills the CLI mid-run. Out: undo of moves (v0.4), GUI.
  - Done when:
    - apply then undo returns the destination to its manifest from before the apply
    - killing `sheepdog-cli apply` at 50 random points and then running `recover` leaves no partial files, and no content is lost
    - undo skips a copy that was edited after the apply and reports it
- [ ] **0.2-T8 Organise wizard and History (GUI)** (M; effort medium; after: 0.2-T7, 0.1-T9)
  - Scope: In: the wizard (scope, layout lines with a live sample, destination, copy mode); the preview tree (before/after, filters, totals, free space); Confirm, progress and report; a History dock with Undo; a list of saved layouts. Out: move mode, query scope.
  - Done when:
    - offscreen test: preview counts equal `sheepdog-cli plan --json` output for the same input
    - nothing is created on disk before Confirm (the manifest is unchanged after a preview)
    - Undo in History restores the destination manifest
    - v0.2.0 released: USER_GUIDE section 'Organise by copy', CHANGELOG section, tag and GitHub release

### v0.3.0 Tags and queries

**Goal:** Manual and automatic tags, a query language, smart folders, and layouts driven by tags and queries.  
**You get:** Drag files onto tags, see automatic Year, Camera and Type tags, search with tag:Trips/* -tag:Reviewed, save smart folders, and organise with tag:Trips/* -> Trips/{tag:Trips/*}/{name}{ext}.

- [ ] **0.3-T1 Tag model and manual tagging (core)** (M; effort high; after: 0.1-T7)
  - Scope: In: a migration for tag, tag_alias, file_tag, tag_suppress and tag_op; tags stored with their full path; create, rename, move, merge and colour, each in a single transaction; bulk assign and remove with undo; copying manual tags to every file with the same BLAKE3 hash; JSON export and import of manual tags, plus export on exit; `sheepdog-cli tag`. Out: rules, GUI.
  - Done when:
    - export, then an empty catalog, then import reproduces identical manual tags
    - rename, move and merge keep every assignment (property test)
    - tagging 10k files is one operation that takes under 1 s and can be undone in one step
- [ ] **0.3-T2 Query language** (M; effort high; after: 0.3-T1, 0.2-T4)
  - Scope: In: lexer and parser producing an AST that compiles to parameterised SQL (tag:, -tag:, kind:, ext:, taken:a..b, size:>N, name:, path:, camera:, rating:, OR, parentheses, free text via FTS5); escaping for SQL LIKE; keeping the FTS5 index current; saved queries; cancelling with sqlite3_interrupt; layouts accept full queries; `sheepdog-cli query`. Out: GUI.
  - Done when:
    - 40+ table tests (query to SQL + parameters) pass, including injection strings
    - a query on a synthetic 1M-file catalog returns in under 300 ms (Release build, in the container)
    - layout files from v0.2 still parse and produce identical plans
- [ ] **0.3-T3 Rule engine and default rules** (M; effort high; after: 0.3-T2)
  - Scope: In: the rule table; recomputing one rule by deleting and re-inserting only its rows; running rules incrementally after each scan; a diff shown before applying; suppress and pin; the default rule pack (the Duplicate rule hashes equal-size files on demand); the {tag:Parent/*} template field; `sheepdog-cli rules`. Out: rule editor GUI.
  - Done when:
    - recomputing all rules twice changes nothing, and manual tags never change (property tests)
    - editing a rule reports exactly which tags will be added and removed before applying
    - a layout using {tag:Trips/*|} plans the expected paths
- [ ] **0.3-T4 Tagging GUI** (M; effort medium; after: 0.3-T3, 0.1-T9)
  - Scope: In: a tags tree dock with counts (automatic tags in grey); create, rename, merge and colour; dragging files onto tags; Inspector chips with autocomplete (automatic tags outlined with a tooltip naming the rule, suppress, pin); bulk tagging of the selection; Ctrl+Z/Ctrl+Y for tag edits. Out: query bar.
  - Done when:
    - QAbstractItemModelTester reports no problems with the tag model
    - offscreen test: dropping 1,000 selected files on a tag is one action that can be undone
    - automatic and manual chips look different, and the tooltip names the rule
- [ ] **0.3-T5 Query bar, smart folders and rule editor (GUI)** (M; effort medium; after: 0.3-T4)
  - Scope: In: a query bar with autocomplete and inline errors; live filtering as you type, with a short delay and cancellation of the previous query; a saved-queries dock; a rule editor with match count and diff; a query as the wizard's scope. Out: thumbnails.
  - Done when:
    - typing a query updates a 100k-file table within 300 ms
    - an invalid query shows an inline error and keeps the previous results
    - the rule editor shows the match count and diff before saving
    - v0.3.0 released: USER_GUIDE reference for queries, rules and templates, CHANGELOG, tag and GitHub release

### v0.4.0 Move with undo

**Goal:** Reorganise in place: journaled moves on the same drive and across drives, crash recovery, undo and redo.  
**You get:** The Organise wizard offers Move (beta), History can undo and redo moves, and after a crash the app offers Resume, Roll back or Leave.

- [ ] **0.4-T1 Rename chains and no-overwrite fallbacks** (M; effort high; after: 0.2-T6)
  - Scope: In: what to do when renameat2 returns EINVAL (link + unlink; on vfat/exFAT, check the target is free and then rename); planner ordering for rename chains, swaps and cycles using temporary names; case-only renames through a temporary name; a privileged compose service fstests with vfat, exFAT and ntfs3 loop images. Out: executor changes.
  - Done when:
    - a race test (another thread creates the target) never overwrites, on NTFS, ext4 and vfat
    - swapping A and B and a case-only rename succeed on NTFS (Windows CI) and vfat (fstests)
    - property test: plans with cycles never target an occupied path at any step
- [ ] **0.4-T2 Move executor, undo/redo and recovery** (M; effort high; after: 0.4-T1, 0.2-T7)
  - Scope: In: same-drive move operations; the catalog path update in the same transaction as done; undoing moves only where the disk still matches the journal; redo as the undo of an undo; the recovery decision table; removal of empty folders the batch created. Out: moves across drives, GUI.
  - Done when:
    - executing and then undoing gives a byte-identical tree, modification times included
    - a simulated crash at every operation (FaultyFs) plus 50 random kills: recovery always completes and no content is lost
    - undo skips files changed after the move and reports them
- [ ] **0.4-T3 Moves across drives** (M; effort high; after: 0.4-T2)
  - Scope: In: copy, flush, rename without overwrite, re-read and verify, mark verified, delete the source, mark done; keeping timestamps (including the Windows creation time) and the read-only attribute; preview warnings for lost owner, extended attributes and alternate data streams; handling EXDEV; a second drive on Windows CI from a diskpart VHD. Out: GUI.
  - Done when:
    - when FaultyFs corrupts the copy, the source is left untouched and the operation is marked failed
    - a move from /tmp to the tmpfs plus undo passes in the container, and a move from C: to the VHD plus undo passes on Windows CI
    - a lack of free space is reported at plan time, never mid-run
- [ ] **0.4-T4 Move mode UI and recovery dialog (GUI)** (M; effort medium; after: 0.4-T3, 0.3-T5)
  - Scope: In: a startup recovery dialog (Resume, Roll back, Leave); Move mode in the wizard, labelled beta; preview markers for moves across drives and for metadata that will be lost; Redo in History; a report of failed operations. Out: engine changes.
  - Done when:
    - after a simulated crash the GUI offers Resume, Roll back and Leave (offscreen test)
    - a move followed by undo through the wizard on a temp tree leaves an identical manifest
    - the preview marks moves across drives and shows lost-metadata warnings
    - v0.4.0 released: USER_GUIDE section on safety, undo and recovery, CHANGELOG, tag and GitHub release

### v0.5.0 Rich metadata

**Goal:** Videos, music and documents get metadata, automatic tags and template fields.  
**You get:** Phone videos sort by their real capture date, music by artist and album, documents by creation date, and Lightroom keywords show up as tags.

- [ ] **0.5-T1 ISOBMFF reader (HEIC, MP4, MOV, CR3)** (M; effort high; after: 0.2-T2)
  - Scope: In: a box reader with limits; the Exif item in HEIC/AVIF; creation time from mvhd/tkhd (1904 epoch, UTC, 0 means absent), duration, dimensions and ©xyz GPS; CR3; an updated capture-date order; a fuzz target. Out: MKV and AVI (after 1.0).
  - Done when:
    - the generated test files pass, and a 10-minute fuzz run finds nothing
    - an MP4's UTC creation time is shown as local time, and in HEIC files the EXIF date wins over the QuickTime date
    - a layout for phone videos plans the expected year and month paths
- [ ] **0.5-T2 Documents (Office, ODF, EPUB, PDF) and XMP/IPTC keywords** (M; effort medium; after: 0.5-T1, 0.3-T3)
  - Scope: In: miniz + pugixml reading docProps/core.xml, meta.xml and content.opf; an in-house PDF reader for the Info dictionary and the XMP packet (FlateDecode via miniz; encrypted files skipped); XMP via pugixml from JPEG APP1, PNG iTXt and sidecar files; IPTC-IIM keywords; keywords imported as Imported/...; default rule Rating; fuzz targets. Out: indexing the full text of documents; writing tags into files.
  - Done when:
    - document test files give the expected title, author and dates
    - Lightroom hierarchical keywords appear under Imported/
    - 10-minute fuzz runs of the ZIP and PDF readers find nothing
    - the layout Documents/{doc.created:%Y}/... plans the expected paths
- [ ] **0.5-T3 Audio tags** (M; effort medium; after: 0.5-T2)
  - Scope: In: TagLib 2 via vcpkg (static, with an MPL-1.1 notice) for MP3, FLAC, Ogg, Opus, M4A, WAV, AIFF and WMA; default rule Music/Artist. Out: writing tags into files.
  - Done when:
    - audio test files (ID3v2.3, ID3v2.4 in UTF-16, FLAC, Ogg, M4A) give the expected fields
    - the music layout {audio.artist|Unknown}/{audio.album|Unknown}/... plans the expected paths
    - v0.5.0 released with CHANGELOG, a USER_GUIDE field reference, tag and GitHub release

### v0.6.0 Reconcile and drives

**Goal:** Cope with changes made outside the app, changed drive letters and unplugged disks, and show duplicates.  
**You get:** Files renamed or moved in Explorer keep their tags, unplugged drives show as offline, a Missing view relinks files, and a Duplicates view groups identical files and syncs their tags.

- [ ] **0.6-T1 Rename/move detection and volume matching** (M; effort high; after: 0.3-T1)
  - Scope: In: on rescan, classify each file as unchanged; modified (metadata re-read, tags kept); replaced in place; renamed (same volume, file ID and size); moved (same quick fingerprint and BLAKE3); or missing. Also: 2-second mtime tolerance on FAT; never guessing between candidates; relink suggestions; recognising a volume by its ID after a drive-letter change (offline, not missing); `sheepdog-cli missing` and `relink`. Out: GUI.
  - Done when:
    - tags survive a rename and a move made outside the app, on both OSes, including an editor that saves via temp file + rename
    - two equally good candidates leave the file missing, with both suggested
    - a VHD re-mounted under another letter keeps its root and tags, and a detached one shows as offline, not missing (Windows CI)
- [ ] **0.6-T2 Missing and Duplicates views (GUI)** (M; effort medium; after: 0.6-T1, 0.3-T4)
  - Scope: In: offline roots shown greyed out but still browsable; a Missing dock with relink suggestions; a Duplicates view grouping identical files, with 'apply tags to all copies'. Out: deleting duplicates (that is Bloodhound's job).
  - Done when:
    - QAbstractItemModelTester reports no problems with the Missing and Duplicates models
    - offscreen test: relinking a missing file from the dock restores its path and tags
    - duplicates in the test set are grouped, and tag sync works from the view
    - v0.6.0 released with CHANGELOG, USER_GUIDE, tag and GitHub release

### v0.7.0 Visual browsing

**Goal:** Thumbnails, a German UI, preferences and onboarding.  
**You get:** A zoomable thumbnail grid, a German translation, and a first-run wizard that gets a new user to a preview in minutes.

- [ ] **0.7-T1 Thumbnails and grid view** (M; effort medium; after: 0.1-T9, 0.2-T1)
  - Scope: In: a low-priority QThreadPool loader trying, in order, the embedded EXIF preview, Qt's decoders (with qtimageformats) and, on Windows, IShellItemImageFactory; an in-memory LRU cache plus a size-limited disk cache keyed by file ID and mtime; EXIF orientation; a grid view with zoom. Out: decoding HEIC and RAW on Linux.
  - Done when:
    - a grid of 10k images scrolls with no event-loop stall over 50 ms
    - the disk cache stays under its configured limit
    - images with an EXIF rotation display upright
- [ ] **0.7-T2 German UI, preferences and first run** (M; effort medium; after: 0.7-T1)
  - Scope: In: a check that every string goes through tr(); a German translation (de.ts) made with Qt Linguist; a language setting; preferences (catalog location, workers per drive type, cache size); a first-run wizard (add folders, scan, preview a sample layout); a keyboard-shortcuts sheet. Out: other languages.
  - Done when:
    - lupdate reports no unfinished German strings
    - core and GUI tests pass with LANG=de_DE.UTF-8 in the container and on German Windows
    - the first-run wizard reaches a preview of a sample folder
    - v0.7.0 released with CHANGELOG, USER_GUIDE screenshots, tag and GitHub release

### v0.8.0 Scale and beta hardening

**Goal:** Large libraries stay fast, and analysers, sanitizers and fuzzers report nothing. First public beta.  
**You get:** A library of 1M files stays responsive, and the beta has no known high-severity bugs.

- [ ] **0.8-T1 Performance pass with 1M files** (M; effort high; after: 0.6-T2)
  - Scope: In: a synthetic catalog generator; a benchmark suite with a nightly job; index and query tuning; fewer workers on spinning disks (detected with the seek-penalty IOCTL on Windows and /sys/block/*/queue/rotational on Linux). Out: new features.
  - Done when:
    - queries take under 300 ms, a rescan of 1M unchanged files under 60 s on an SSD, and a 50k-operation preview under 2 s
    - memory stays flat while scanning 1M files
    - the measured numbers are recorded in docs/ARCHITECTURE.md
- [ ] **0.8-T2 Static analysis and sanitizer clean-up** (M; effort high; after: 0.8-T1)
  - Scope: In: clang-tidy (bugprone, cert, performance), cppcheck, GCC -fanalyzer, full ASan/UBSan/TSan runs and a 1-hour fuzz run per parser; findings are fixed, not suppressed; the analysis jobs start blocking merges. Out: refactoring that no finding calls for.
  - Done when:
    - no open high-severity findings
    - analysis and sanitizer jobs block merges in CI
    - v0.8.0 (beta) released with CHANGELOG, tag and GitHub release

### v0.9.0 Release candidate

**Goal:** An installer, an icon, code signing, Linux packages, complete docs, an independent safety review and a trial on real data.  
**You get:** A Windows installer with an icon, signed Windows files, .deb and Flatpak packages, a complete user guide, and a release candidate tested on a copy of real data.

- [ ] **0.9-T1 Installer, icon and docs** (M; effort medium; after: 0.8-T2)
  - Scope: In: a per-user Inno Setup installer; an icon drawn by a Python standard-library script (SVG, PNG, ICO); an About dialog listing every licence; a complete USER_GUIDE (safety model, undo, recovery, reference for queries, templates and rules), ARCHITECTURE and CONTRIBUTING. Out: code signing and Linux packages (0.9-T2).
  - Done when:
    - installing and uninstalling in Windows Sandbox leaves only user data behind
    - About lists every dependency's licence, with source links for Qt and TagLib
    - every query, template and rule element has a documented example
- [ ] **0.9-T2 Code signing, .deb and Flatpak** (M; effort medium; after: 0.9-T1)
  - Scope: In: signing sheepdog.exe, sheepdog-cli.exe and the installer in release.yml through the free open-source signing programme applied for after v0.1.0 (SignPath Foundation or similar); a .deb for Debian 13 built against the system Qt 6.8 (compose service deb, dpkg-deb, lintian); a Flatpak manifest on the KDE runtime with a single-file .flatpak bundle (compose service flatpak); smoke services that install each package and run --selftest; SHA256SUMS covering the new files; USER_GUIDE install notes. Out: Flathub submission and an apt repository (after 1.0).
  - Done when:
    - the release files from a test tag carry a valid Authenticode signature (Get-AuthenticodeSignature reports Valid); if the programme has not approved the project yet, the plan records why and 1.0 ships unsigned
    - the .deb installs in debian:13, passes lintian without errors and its --selftest passes
    - the Flatpak bundle installs and its --selftest passes; if Flatpak cannot build inside a container on carbon, the plan records the alternative used
- [ ] **0.9-T3 Independent safety review and real-data trial** (M; effort ultracode; after: 0.9-T2)
  - Scope: In: a fresh-eyes review (/code-review at high or ultra) of platform/FsOps, exec, recovery, the planner and name cleaning; a coverage report for the fault-injection paths; fixes for high-severity findings; you run scripts/trial.ps1 on a COPY of a real library (Claude never does this). Out: new features.
  - Done when:
    - every high-severity finding is fixed, or recorded with a reason
    - on 50k real files, execute plus undo twice gives zero manifest differences
    - Bloodhound reports the same set of unique contents before and after
    - v0.9.0 released with CHANGELOG, tag and GitHub release

### v1.0.0 Sheepdog 1.0

**Goal:** A published 1.0 that upgrades safely and is fully documented.  
**You get:** sheepdog-1.0.0 on GitHub with a signed installer, portable zip, AppImage, .deb, Flatpak bundle and checksums; catalogs from every beta upgrade cleanly.

- [ ] **1.0-T1 Upgrade tests and the 1.0 release** (S; effort medium; after: 0.9-T3)
  - Scope: In: migration tests from every saved 0.x catalog, release-candidate fixes, a final docs pass, tag sheepdog-1.0.0. Out: new features (they go on the post-1.0 list in RELEASE_PLAN.md).
  - Done when:
    - catalogs from every 0.x milestone upgrade and pass integrity_check
    - the release files pass smoke tests in Windows Sandbox, Debian 12, Debian 13 (.deb), Fedora and the Flatpak bundle
    - sheepdog-1.0.0 is published with CHANGELOG notes and SHA256SUMS

## After 1.0

- Opt-in tag export: XMP sidecars and `user.xdg.tags` (never writing into the files themselves)
- Live folder watching
- MKV, WebM and AVI metadata; place names from GPS; HEIC and RAW thumbnails on Linux
- Face or place recognition
- A headless `sheepdog-cli` image or NAS mode
- Flathub submission and an apt repository
