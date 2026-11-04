# Sheepdog design (working name)

Status: draft from the planning session of 2026-10-31. The tasks are in [RELEASE_PLAN.md](RELEASE_PLAN.md); change decisions in these files, not in chat.

> **Toolchain change (2026-10-31):** Windows builds use Qt 6.12.0's MinGW kit (`C:\Qt\6.12.0\mingw_64`) with MinGW-w64 GCC 13.1, CMake 3.30 and Ninja 1.12 from `C:\Qt\Tools`, not MSVC 2022: the Qt install contains the MinGW kit, and Build Tools 2022 has no C++ workload. The code stays free of compiler-specific extensions, so an MSVC build can be added later.


## Overview

**Sheepdog** (working name) is a tag-driven file organiser built around a catalog. It works in five layers, and only the last one changes anything on disk.

1. **Catalog.** You add root folders on any drive. Sheepdog scans them in sorted order, detects each file's type, reads its metadata and keeps everything in a catalog. Content hashes are computed only when needed: for files of equal size, for files in a plan, and for identity checks. This layer is read-only.
2. **Automatic tags.** Rules turn metadata into hierarchical tags such as `Type/Image`, `Year/2023`, `Camera/Google/Pixel 7`, `Music/Artist/Queen` or `Duplicate`. These tags can always be rebuilt. You can suppress one on a single file or pin it (copy it to manual). The Inspector shows which rule added each tag.
3. **Manual tags.** Hierarchical (`Trips/2023 Japan`), with colours and aliases. You add them by dragging files onto a tag, with tag chips or by bulk edit, and every change can be undone. They are the only data that can't be rebuilt, so they are backed up and exported to JSON.
4. **Queries and smart folders.** Example: `tag:Trips/* kind:image taken:2023-04..2023-06 -tag:Reviewed size:>20MB beach`. A saved query works as a virtual folder, so you get organisation without moving anything.
5. **Layouts.** A layout is a list of lines `query -> template` (the first match wins), plus a destination, a mode (copy, later move) and a conflict policy. Applying it goes: plan → before/after preview → confirm → run with every step journaled → verify → undo or redo.

**Safety rule (asserted in tests).** Every file content that existed when a plan started exists at one or more paths at every moment, and the journal records which paths. Sheepdog deletes a file in two cases only:
- the source of a cross-drive move, after its copy has been re-read and its hash checked;
- a copy being undone, after its original has been verified.

Tagging never modifies files, and 1.0 writes nothing into user files.

**Typical use.** A phone dump lands in an *Inbox* root. You scan it, get the automatic tags, and add `Trips/…` by hand. You apply `kind:image -> Photos/{taken:%Y}/{taken:%Y-%m}/{name}{ext}` in copy mode into *Archive* and check the result. Later you reorganise in place with move mode, with undo available.

**Not in 1.0:** writing tags into files, live folder watching, face or place recognition, MKV metadata, a NAS/server mode.


## Key decisions and why

**Base: risk** (33 of 40; polish and lean score 31 each), rebalanced so that safe organising ships early.

**Why risk.** A file mover built by one developer in short AI sessions stands or falls on engine safety and testability, and risk is strongest there:
- **Qt-free core.** The C++20 core never passes paths through QString. That keeps Linux file names lossless and lets the core be tested and sanitized without Qt.
- **Safe execution.** The journal is written before any file is touched, renames never overwrite, and a source is deleted only after its copy is verified.
- **Tests and guard rails.** A fake filesystem (FaultyFs) crashes at every call in turn, kill tests stop the process mid-run, and a sandbox guard keeps every session away from real folders.
- **Staged rollout.** Read-only first, then copy, then move.

Risk uses Qt 6 Widgets, the same toolkit as polish, so the GUI can be just as good. lean's ImGui falls short of a nice GUI for a file-manager-style app: no dragging files out to Explorer, no screen-reader support, and many widgets drawn by hand. lean also brings its own audio and PDF parsers.

**What changes from risk**
- **Order.** Organise-by-copy with preview and undo comes in **v0.2**, using dates from EXIF, file names or modification time. Tags and queries follow in v0.3, moves in v0.4.
- **Toolkit pinned.** Releases use **Qt 6.12 LTS** (released 30 Sep 2026). The code must still build with Qt 6.8, which the Debian 13 container checks. Compiler on Windows: **MinGW-w64 GCC 13.1** from the Qt installer (see the toolchain note at the top).
- **PDF and video.** A small in-house PDF reader replaces qpdf. MKV/AVI support comes after 1.0.
- **Grafts.** From lean: layouts as `query -> template` lines. Tag implications become ordinary rules. From polish: drives tracked as volumes with an offline state, Explorer thumbnails, the model tester on every list, and cancelling a running query when a new one is typed.
- **Docker** test image in task 3.
- **One choice each**: one test framework (Catch2 v3), one hash (BLAKE3), one implementation per format family.

**On "minimal dependencies".** There are seven runtime libraries. All except Qt are small, permissively licensed and statically linked. Qt (about 35 MB, LGPL, linked dynamically) is the price of a polished, accessible GUI that can drag files out to Explorer.

**Fact checks:**
- [Qt 6.12 for Windows](https://doc.qt.io/qt-6.12/windows.html): MSVC 2022 for x64; the last version to support Windows 10.
- [Qt 6.12 LTS released](https://qt.io/blog/qt-6.12-released).
- [CLion Docker toolchain](https://www.jetbrains.com/help/clion/clion-toolchains-in-docker.html).
- [linuxdeploy-plugin-qt](https://github.com/linuxdeploy/linuxdeploy-plugin-qt).


## Architecture

**Targets**
- **`sheepdog_core`**: a static C++20 library with **no Qt**.
  - `platform`: path codec, folder listing, file identity, volumes and `FsOps`, the only code allowed to change files. It is the only module with `#ifdef _WIN32`.
  - `catalog` (SQLite), `jobs`, `scan`.
  - `meta`: metadata parsers built on a bounds-checked `ByteReader`.
  - `tags`, `rules`, `query` (compiled to parameterised SQL), `template`.
  - `plan` (pure functions), `exec` (journal, executor, recovery).
  - A `Library` facade plus a read-only `CatalogReader` for paging, sorting and filtering.
- **`sheepdog-cli`**: links only the core. Commands: `root scan ls hash meta tag query rules plan apply undo recover missing relink manifest`. It is the test oracle, the scripting interface and what the Docker jobs run.
- **`sheepdog`**: the Qt Widgets GUI. It refers to files by catalog ID and never opens, moves or deletes them itself.
- **`tests`** and **`fuzz/*`**.

**Threads**
- The GUI thread.
- One `std::jthread` per job. Only one job that changes files runs at a time.
- A bounded worker pool. At most workers × 4 tasks are queued, with per-volume limits of 2 for HDDs and 8 for SSDs.
- One database-writer thread that commits about 1,000 rows per transaction (WAL mode).
- A query thread with its own read connection. `sqlite3_interrupt` cancels a query when a new one is typed.
- A low-priority thumbnail pool on the GUI side.

**Bloodhound patterns in C++**
|Bloodhound|Sheepdog|
|---|---|
|GUI-free core|Qt-free core + CLI|
|workers × 4 queue|`BoundedPool` (~150 lines)|
|cancel between 1 MiB chunks|`std::stop_token` checked per item and per chunk, plus SQLite's progress handler|
|immutable snapshot polled every 150 ms|`shared_ptr<const Snapshot>` swapped under a mutex, read by `QTimer(150)`. Same 5 s speed window, ETA, elapsed time and time left in the step|
|fake clock|injected `Clock`|
|`.partial` file + rename + verify + resume|`.sheepdog-partial` created with CREATE_NEW/O_EXCL, BLAKE3 re-read, resume from the journal|
|sorted scan, `name (2).ext`, case-insensitive comparison|same; comparison uses `name_key` = NFC + case folding (utf8proc)|
|IFileOpenDialog multi-select|same code, ported into the GUI|

**Conventions**
- Platform calls return `Result<T>`: a value, or a `std::error_code` plus the path. No exceptions cross threads.
- Numbers are parsed with `std::from_chars`, so the German locale's decimal comma can't break parsing.
- Neither `std::format` nor `<chrono>` time zones are used, because the AppImage builds with GCC 12.
- `std::filesystem::rename`, `remove` and `copy` are banned on user paths anywhere outside `FsOps`.


## GUI

**Qt 6 Widgets.** Releases are built with **Qt 6.12 LTS**; the code also compiles with Qt 6.8.
- **Style:** `windows11` on Windows 11, Fusion elsewhere. Light/dark follows the system.
- **Icons:** Lucide SVGs (ISC licence) through QtSvg.
- **Translation:** every string goes through `tr()` from day one; German arrives in v0.7.
- **Keyboard and accessibility:** every action has a shortcut and an accessible name.

```
┌ Query bar (chips, autocomplete, inline errors)   [Table|Grid] [zoom] [Organise…] ┐
│ Library (roots, offline)   │ Files: virtual table / thumbnail grid │ Inspector:    │
│ Tags (counts, auto = grey) │ multi-select, sort, drag onto tags,   │ metadata,     │
│ Smart folders · Layouts    │ drag out to Explorer (copy)           │ date + source,│
│ Missing · Duplicates       │                                       │ tag chips     │
├ Activity: phase · files/bytes · speed · ETA · elapsed · step left [Stop] │ History [Undo][Redo] ┤
└ Status: catalog · 182,340 files · 12 selected · v0.4.0                                       ┘
```

**Organise wizard**
1. **Scope:** the selection, a root or a query.
2. **Layout:** layout lines, destination and mode, with a live sample. The mode is copy; move arrives in v0.4 and is labelled beta until 1.0.
3. **Preview:** a before/after tree that can be filtered to conflicts, already-present files, no-ops or cross-drive moves. It also shows totals, bytes per drive and free space.
4. **Confirm,** then a progress view, then a report with Undo.

**Other pieces**
- **Tag chips:** manual tags are solid; automatic tags are outlined, with a tooltip naming the rule. × suppresses a tag; pin turns it into a manual tag.
- **Startup recovery dialog:** offers Resume / Roll back / Leave for batches that did not finish.
- **Models:** a `QAbstractTableModel` pages through SQLite, with pages fetched on the query thread. Every model is checked with `QAbstractItemModelTester`.
- **Folder picker:** Bloodhound's IFileOpenDialog multi-select (`FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT`) on Windows; QFileDialog on Linux.
- **Thumbnails**, tried in this order:
  1. the preview embedded in the EXIF data;
  2. Qt's image decoders, plus qtimageformats for WebP and TIFF;
  3. on Windows, `IShellItemImageFactory` (Explorer's thumbnails), which covers HEIC, RAW and video when the codecs are installed.

  On Linux those formats show a type icon. Thumbnails are cached in memory (LRU) and on disk, with a size limit.
- **Responsiveness:** the GUI thread never does file I/O. Long work runs as a core job whose progress is polled every 150 ms.


## Metadata and tags

**Extraction: one implementation per format family.** Every parser has size and nesting limits, its own fuzz target and generated test files.

| Formats | Fields | Implementation |
|---|---|---|
| Any file | type and kind | In-house: about 60 magic-byte signatures plus the extension |
| JPEG, TIFF, DNG, CR2, NEF, ARW, PNG, WebP | DateTimeOriginal + OffsetTimeOriginal, make, model, lens, orientation, size, GPS, embedded preview | In-house TIFF-IFD/EXIF parser. Exiv2 rejected: GPL-2.0+ |
| HEIC, AVIF, CR3, MP4, MOV | Exif item, `mvhd` creation time (1904 epoch, UTC; 0 means absent), duration, `©xyz` GPS | In-house ISOBMFF box reader |
| MP3, FLAC, Ogg, Opus, M4A, WAV, AIFF, WMA | audio tags | **TagLib 2**, linked statically under MPL-1.1 |
| XMP (in JPEG APP1, PNG iTXt, `.xmp` sidecars) | `dc:subject`, `lr:hierarchicalSubject`, rating, dates | pugixml |
| IPTC-IIM | keywords | In-house |
| DOCX, XLSX, PPTX, ODF, EPUB | title, author, dates (core.xml, meta.xml, content.opf) | miniz + pugixml |
| PDF | Info dictionary, XMP packet | In-house, best effort; FlateDecode via miniz; encrypted files skipped |
| File names | dates | Patterns: `IMG_20230514_…`, `PXL_`, `VID_`, `Screenshot_`, `-WA0001` |

After 1.0: MKV, WebM and AVI; place names from GPS; HEIC and RAW thumbnails on Linux.

**Capture date** sources, in order:
1. EXIF, with its time-zone offset;
2. XMP;
3. QuickTime time (UTC);
4. a date in the file name;
5. the modification time.

The date is stored with its source and a confidence level. Templates can send low-confidence files to `Unsorted/`.

**Tags**
- **Hierarchy.** Each tag stores its full path (`Places/Spain/Madrid`) plus a parent ID. `tag:Places/Spain` matches that tag and every tag whose path starts with `Places/Spain/` (SQL `LIKE`, with wildcards escaped).
- **Sources.** Each assignment records where it came from: `manual`, `rule:<id>` or `import:xmp`.
- **Suppress and pin.** A suppression always beats a rule. Pin copies an automatic tag to manual.
- **Aliases and implications.** Aliases are supported. An implication is just a rule, for example `when tag:People/Ana tag "Family"`.
- **Duplicates.** Tags belong to the individual file record. The Duplicates view offers "apply to all copies".

**Rules**
- A rule is `when <query>` plus a tag template, with a version number.
- Recomputing a rule deletes and re-inserts only that rule's rows, so it is idempotent. Rules also run incrementally after each scan.
- Before applying a change, Sheepdog shows the difference (+N/−M tags).
- **Default rules:** `Type/…`, `Year/{taken:%Y}`, `Camera/{make}/{model}`, `Music/Artist/{artist}`, `Rating/{n}`, Screenshot, Has GPS, Duplicate.

**Storage.** In 1.0, tags live only in the catalog.
- XMP and IPTC keywords are imported as `Imported/…` tags.
- Manual tags are exported to JSON.
- Opt-in XMP sidecars and `user.xdg.tags` export come after 1.0.


## Data model

**One SQLite catalog**, stored in `%LOCALAPPDATA%\Sheepdog\` or `~/.local/share/sheepdog/`. Sheepdog refuses to put it on a network drive.
- **Settings:** WAL mode, `foreign_keys=ON`, and `synchronous=FULL` for journal commits.
- **Checks and backups:**
  - `integrity_check` runs when the catalog opens;
  - `sqlite3_backup` runs before every migration and every organise batch, keeping 10 copies;
  - manual tags are exported to JSON on exit.
- **Migrations** use `PRAGMA user_version`. A sample catalog from each milestone is kept in `tests/data/schema/` for upgrade tests.

```sql
volume(id, uid TEXT UNIQUE,             -- NTFS serial or filesystem UUID
       label, fs_type, case_sensitive, rotational, online)
root(id, volume_id, path BLOB, display TEXT, added)
file(id, root_id, rel_path BLOB, display TEXT, name_key TEXT,   -- NFC + casefold
     size, mtime_ns, btime_ns, fs_id BLOB, quick_fp BLOB, blake3 BLOB,
     kind, ftype, taken TEXT, taken_off INTEGER, taken_src, taken_conf,
     status CHECK(status IN ('present','missing','ignored')),
     last_seen, meta_ver, UNIQUE(root_id, rel_path))
meta(file_id, key, txt, num, PRIMARY KEY(file_id, key))      -- 'exif.model'
tag(id, parent_id, name CHECK(instr(name,'/')=0), path TEXT UNIQUE, color, kind)
tag_alias(alias PRIMARY KEY, tag_id)
file_tag(file_id, tag_id, source, ts, PRIMARY KEY(file_id, tag_id, source))
tag_suppress(file_id, tag_id, PRIMARY KEY(file_id, tag_id))
rule(id, name, enabled, prio, version, when_expr, tag_template)
saved_query(id, name, expr)
layout(id, name, lines, dest_root, mode, on_conflict)
batch(id, kind, state, params_json, undoes_batch, created, finished)
op(id, batch_id, seq, kind, file_id, src BLOB, dst BLOB, size, mtime_ns,
   blake3, state, error) -- planned|started|copied|verified|done|skipped|failed|undone
tag_op(batch_id, file_id, tag_id, action)        -- undo for tag edits
file_fts USING fts5(display, meta_text)
```

**Notes**
- **Paths.** Each path is stored twice:
  - as native bytes in a `BLOB`: raw bytes on Linux, WTF-8 on Windows so unpaired surrogates survive;
  - as UTF-8 `display` text for the UI and search.
- **Dates.** `taken` is local ISO-8601 text. `taken_off` is the offset from UTC in minutes, or NULL for EXIF times without a zone.
- **Offline drives.** "Offline" is a property of the volume, not of each file. Files on an offline volume keep `present` and stay browsable.
- **Indexes:** `file(blake3)`, `file(size, quick_fp)`, `file(fs_id)`, `meta(key, num)`, `meta(key, txt)`, `file_tag(tag_id)`.


## Organising engine

**Templates.** Syntax is `{field[:fmt][|fallback]}`, with filters `|lower|upper|trim|max:N`.
- Fields: `name stem ext kind taken mtime camera.* audio.* doc.* rel_dir counter`, and `{tag:Trips/*}` for the first child tag.
- A layout is a list of lines `query -> template`. The first matching line wins, and `-> (keep)` leaves files where they are.

**Name cleaning** uses the same rules on both systems, so a library stays portable:
- replace `<>:"/\|?*` and control characters;
- add a suffix to reserved names (`CON`, `con.txt`, `COM1`);
- strip trailing dots and spaces;
- normalise to NFC;
- keep each path component within 255 bytes.

**Companion files** move as a group and get the same new stem: RAW+JPG+XMP+THM/AAE, Live Photo HEIC+MOV, video+SRT.

**Planner.** A pure function that gives the same result for the same input. `sheepdog-cli plan --json` is the dry run.
- Two files mapped to one target: the later one in sorted order becomes `name (2).ext`.
- Names that are equal after case folding always count as a clash.
- Target already exists: skip by default, or rename.
- Identical content already at the target: reported as *already present*, and the source stays where it is.
- Chains and swaps are ordered so no step lands on an occupied path, using temporary names where needed. Case-only renames always go through a temporary name.
- Checks free space per drive, path length, FAT32's 4 GiB limit and read-only targets.

**Executor.** The journal is written first: up to 100 operations are committed as `started` before the disk is touched. Before each operation, the source's size, modification time and file ID are checked again; if they changed, it is skipped.
- **Same drive:** a rename that refuses to overwrite.
  - Windows: `MoveFileExW` without `REPLACE_EXISTING`.
  - Linux: `renameat2(RENAME_NOREPLACE)`. If that returns EINVAL, fall back to link + unlink. On vfat/exFAT, which lack hard links, check the target is free, then rename.
- **Copy, or move to another drive:**
  1. Create `name.sheepdog-partial` with CREATE_NEW.
  2. Copy in 1 MiB chunks, hashing with BLAKE3 as it goes.
  3. Flush to disk.
  4. Restore timestamps and attributes.
  5. Rename into place without overwriting.
  6. Re-read the copy and compare hashes.
  7. Mark the operation `verified`.
  8. Move mode only: delete the source.
  9. Mark it `done`; the catalog update commits in the same transaction.

**Undo** replays the inverse operations in reverse order, but only where the files on disk still match the journal. It runs as a new batch, so redo comes for free. It removes empty folders the batch created.

**Recovery at startup**
- Only the source exists: redo or drop the operation.
- Only the destination exists: mark it done.
- Both exist: compare hashes.
- A partial file listed in the journal: delete it.

**Handled cases:**
- OneDrive cloud-only files are skipped.
- Locked files are retried 3 times with backoff.
- A clear message explains Controlled Folder Access blocks.
- Symlinks and junctions are never followed.
- Protected roots are refused.
- FAT's 2-second timestamps are tolerated.
- Decomposed (NFD) names are handled.
- The preview warns when extended attributes or alternate data streams will be lost.


## Dependencies

| Name | Purpose | Licence | How obtained | Needed from |
|---|---|---|---|---|
| Qt 6.12 LTS (Core, Gui, Widgets, Svg, qtimageformats plugins; Test and LinguistTools for development) | GUI, image decoding for thumbnails, translations, GUI test helpers | LGPL-3.0. Linked dynamically with unmodified DLLs, notices and a source link shipped; the app stays MIT | aqtinstall on Windows and in the AppImage image; Debian 13 qt6-base-dev 6.8.2 in the CI container, which also checks the minimum version | v0.1 |
| SQLite 3 with FTS5 | Catalog, journal and full-text name search | Public domain | vcpkg sqlite3[fts5] | v0.1 |
| BLAKE3 (C implementation) | Content hashes, quick fingerprints and copy verification | CC0-1.0, Apache-2.0, or Apache-2.0 WITH LLVM-exception | vcpkg blake3 | v0.1 |
| Catch2 v3 | The single test framework; not shipped | BSL-1.0 | vcpkg catch2 | v0.1 |
| Lucide icons | UI icons, stored as SVG in Qt resources | ISC | SVG files committed to the repo | v0.1 |
| utf8proc | NFC normalisation and case folding for name keys and name cleaning | MIT; its Unicode data is under the Unicode licence | vcpkg utf8proc | v0.2 |
| pugixml | XML in XMP and in OOXML/ODF/EPUB files | MIT | vcpkg pugixml | v0.5 |
| miniz | Reading ZIP containers (Office, EPUB) and inflating PDF streams | MIT | vcpkg miniz | v0.5 |
| TagLib 2 | Audio tags and durations | LGPL-2.1 or MPL-1.1. Used under MPL-1.1, linked statically, with a source link | vcpkg taglib | v0.5 |
| Clang/LLVM 19 (libFuzzer, ASan/UBSan/TSan, clang-tidy) | Fuzzing, sanitizers and static analysis (build only) | Apache-2.0 WITH LLVM-exception | Debian 13 packages in docker/ci.Dockerfile | v0.1 |
| linuxdeploy, linuxdeploy-plugin-qt, linuxdeploy-plugin-appimage | Building the AppImage (build only) | MIT (verify for each tool) | Pinned GitHub release downloads, unpacked inside docker/appimage.Dockerfile | v0.1 |
| aqtinstall and jurplel/install-qt-action | Installing Qt binaries locally, in the AppImage image and on the Windows CI runner (build only) | MIT | pip; GitHub Marketplace, pinned by commit SHA | v0.1 |
| Docker Engine on carbon | Linux containers for tests, sanitizers, fuzzing and the AppImage build (build only) | Apache-2.0 | Debian docker.io 26.1, already installed on carbon | v0.1 |
| Inno Setup 6 | Windows installer (build only) | Inno Setup License: free, including commercial use | winget locally; winget or Chocolatey on the CI runner | v0.9 |
| Python 3 standard library | Test-file generator and icon script (build only) | PSF-2.0 | Your existing Python install | v0.2 |

## Testing

**Framework**
- Catch2 v3 for every test.
- GUI tests also run under Catch2, using QtTest helpers (`QTest` events, `QSignalSpy`, `QAbstractItemModelTester` in Fatal mode) with `QT_QPA_PLATFORM=offscreen`.
- Labels `unit fs gui fault slow win linux` let `ctest -L` pick a group.

**Unit tests**
- path-codec round trips and the `ByteReader`;
- every parser;
- golden outputs for templates and name cleaning;
- planner snapshot tables;
- query-to-SQL tables, including injection strings;
- tag hierarchy and rule idempotence;
- progress maths with a fake clock;
- migrations from every saved schema.

**Real temp folder trees** (as in Bloodhound, built with a `TempTree` helper): scan → tag → plan → apply → verify → undo. A test passes when the hash manifest at the end is identical to the start, including modification times for moves.

**Fault injection**
- `FaultyFs` makes filesystem call *k* fail or "crash", for every *k* in a 30-file plan. Recovery then runs, and the tests check that no content was lost and that the batch completes.
- Kill tests start `sheepdog-cli apply` and kill it with `TerminateProcess`/`SIGKILL` at random points.

**Property tests** use random trees and templates from fixed seeds. They check that:
- targets stay unique after case folding;
- the same input always gives the same plan;
- companion files stay together;
- apply followed by undo restores the original.

**Parsers**
- Test files come from `tools/make_fixtures.py` (standard library only, so no licensing questions), plus a few CC0 sample files.
- Each parser has a libFuzzer + ASan/UBSan target. Every crash is saved as a regression test.

**Platform-specific tests**
- Windows CI: paths over 260 characters, reserved names, files held open by another handle, read-only files, cross-drive moves to a VHD, exFAT.
- Container:
  - file names that are not valid UTF-8;
  - `chmod 000` as a non-root user;
  - cross-device moves to a tmpfs;
  - vfat, exFAT and ntfs3 in `fstests`.

**Sandbox guard.** Test and debug builds refuse to change paths outside `SHEEPDOG_SANDBOX`. Claude sessions never run the executor anywhere else.

**Performance targets** (synthetic catalog of 1M files):
- queries under 300 ms;
- rescan of unchanged files under 60 s on an SSD;
- preview of 50k operations under 2 s;
- memory stays flat.

**Before release.** You run a trial on a *copy* of a real library. `sheepdog-cli manifest` output is compared before and after, and Bloodhound confirms that the set of unique contents didn't change.


## CI and packaging

**`ci.yml`** runs on every push and pull request. Permissions are `contents: read`, and third-party actions are pinned by commit SHA.
- **Windows job:**
  - runs on `windows-2025` with Qt's MinGW 13.1 toolchain (`tools_mingw1310`);
  - installs Qt 6.12 with `jurplel/install-qt-action` (aqt underneath, cached);
  - clones vcpkg at a pinned commit and keeps its binary cache with `actions/cache` (triplet `x64-mingw-static`);
  - builds with preset `win-ci` (`-Wall -Wextra -Wconversion -Werror`) and runs `ctest`, including the offscreen GUI tests;
  - from v0.4, creates a VHD with diskpart (NTFS and exFAT) to get a second drive.
- **Linux job:** builds `docker/ci.Dockerfile` with buildx and the GitHub Actions cache, then runs the compose `test` and `asan` services with `-Wall -Wextra -Wconversion -Werror`.

**`nightly.yml`** runs:
- each fuzzer for 5 minutes;
- ThreadSanitizer;
- the privileged `fstests`;
- the AppImage build, which catches GCC 12 or glibc slips;
- from v0.8, the 1M-file benchmarks.

**`release.yml`** runs on a tag `sheepdog-X.Y.Z`, with `contents: write`:
1. Fail unless the tag equals CMake's `project(VERSION)` and CHANGELOG has a section for it.
2. **Windows:**
   - Release build, then `windeployqt --release --no-opengl-sw --no-system-d3d-compiler`, which also copies the MinGW runtime DLLs (libgcc, libstdc++, winpthread).
   - Output: `Sheepdog-X.Y.Z-win64.zip`. An Inno Setup installer is added from v0.9.
3. **Linux:** the compose `appimage` service builds the AppImage and a CLI tarball (`-static-libstdc++`).
4. Write `SHA256SUMS`, then run `gh release create` on the runner (gh is preinstalled there) with the CHANGELOG section as release notes.

**`scripts/release.ps1 X.Y.Z`:**
- checks the working tree is clean;
- bumps the version;
- turns the CHANGELOG's *Unreleased* part into a version section;
- tags and pushes.

**Version.**
- CMake's `project(VERSION)` is the source of truth. Development builds append `-N-gSHA[-dirty]` from `git describe --match "sheepdog-*"`.
- The version appears in the title bar, the About dialog and `--version`.
- On Windows, a version resource and a manifest (long paths, UTF-8 code page, per-monitor DPI, icon) are embedded.

**Licences.**
- Qt DLLs ship unmodified and are linked dynamically.
- `THIRD_PARTY_LICENSES` holds the Qt licence texts with a source link and a TagLib source link, and the About dialog shows it.
- There is no single-file exe as with Bloodhound, because LGPL Qt has to stay replaceable; a portable zip takes its place.

**Signing.** Builds up to v0.9 are unsigned, so Windows SmartScreen shows a warning; this is documented. From 0.9-T2 the Windows files are signed through a free open-source signing programme (SignPath Foundation or similar), applied for once v0.1.0 is public. Linux ships an AppImage, a .deb for Debian 13 and a Flatpak bundle.


## Docker and the Linux host (carbon)

**Containers run on carbon**, not on Windows. carbon (Debian 13, Docker 26.1, 4 CPUs, 8 GB RAM) is ready: `carlos` is in the `docker` group and `hello-world` runs. Nothing Docker-related is installed on Windows.

**Images kept in `docker/`.** Both are developer/CI images, never a way to ship the app.
- **`ci.Dockerfile`**: base `debian:13` with GCC 14, Clang 19 (libFuzzer, sanitizer runtimes, clang-tidy), gdb, CMake and Ninja; Debian's Qt 6.8.2 development packages, the offscreen platform plugin and fonts; vcpkg in `/opt/vcpkg`; a non-root user `dev`; `QT_QPA_PLATFORM=offscreen`. GitHub Actions builds the same file, so carbon behaves like CI, and it proves the code still builds with Qt 6.8.
- **`appimage.Dockerfile`**: base `ubuntu:22.04` (glibc 2.35), GCC 12, Qt 6.12 installed with aqtinstall, and linuxdeploy with its Qt and AppImage plugins unpacked with `--appimage-extract` when the image is built. AppImages normally need FUSE, which containers don't have; unpacking avoids that, so no `--privileged` is needed.

**Running things with `compose.yaml`.** The source is mounted read-only at `/src`, build folders and the vcpkg cache live in named volumes, and a tmpfs at `/mnt/xdev` acts as a second device for real cross-device moves.
- `test`: Debug build plus every `ctest` suite, including the headless GUI tests (`ctest -L gui`).
- `asan`, `tsan`, `tidy`: sanitizer builds and clang-tidy.
- `fuzz` (`-e FUZZ_SECONDS=600`): every fuzz target.
- `fstests`: privileged; vfat, exFAT and ntfs3 disk images; run nightly or by hand.
- `appimage`: Release build with Qt 6.12 and `ctest`, producing `dist/Sheepdog-X.Y.Z-x86_64.AppImage` plus a CLI `.tar.gz`; `smoke-debian12` and `smoke-fedora` then run it with `--appimage-extract-and-run --selftest`.

**Getting the code to carbon.** `scripts/linux-test.ps1 <service>` packs the working tree with tar (no build folders), sends it with scp to `~/Workspace/sheepdog` on carbon, runs `docker compose run --rm <service>` there and prints only the summary. Don't pipe tar into ssh in Windows PowerShell 5.1: its pipeline corrupts binary data. Claude uses the same script, so uncommitted work can be tested on Linux without pushing. carbon has 4 cores and 8 GB RAM: build with `-j4` and run the fuzz, TSan and AppImage jobs one at a time.

**CLion.** Whole test suites go through the script. To step through a Linux-only failure, add a Remote Host toolchain (SSH to carbon). It builds on carbon itself, so it needs `sudo apt install cmake ninja-build gdb qt6-base-dev` there first. Set it up only when you need it.

**Seeing the Linux GUI.** carbon has no display. The offscreen GUI tests save PNG screenshots as test artifacts. For a live window use any Linux desktop, or install WSL later (`wsl --install`) and run the AppImage under WSLg.

**What Docker can't do here.**
- Build or test the Windows binary: use the local MinGW build and the windows-2025 GitHub runner. Windows containers need multi-GB Server Core images and have no GUI.
- Reproduce NTFS, OneDrive, Defender locks or the Recycle Bin: use Windows CI and Windows Sandbox.
- Mount disk images without `--privileged`, give realistic disk benchmarks, or build for macOS.

**About "building images".** The desktop app ships as a zip, an installer and an AppImage, not as a container image. The images above are build and test environments; push `sheepdog-ci` to ghcr.io only if building it in CI gets slow. A headless `sheepdog-cli` image for a NAS is an option after 1.0.


## Risks

- **Data loss while moving files, from an executor bug, a crash or a power cut**  
  Mitigation: Copy mode ships first (v0.2); moves arrive in v0.4 and stay beta until 1.0. The journal is written before the disk is touched, renames never overwrite, and a source is deleted only after its copy is verified. FaultyFs crash-at-every-call tests and kill tests cover the executor, a sandbox guard limits every Claude session, and 0.9-T3 runs a trial on a copy of real data with a Bloodhound cross-check.
- **Losing the catalog, and with it the manual tags**  
  Mitigation: WAL mode; sqlite3_backup before every migration and organise batch (10 copies kept); manual tags exported to JSON on exit; integrity_check on open; the catalog is refused on network drives.
- **Malformed files crash the in-house parsers**  
  Mitigation: A bounds-checked ByteReader with size and nesting limits, a fuzz target per parser run nightly, and ASan/UBSan. Move metadata extraction into a child process only if crashes keep appearing.
- **Wrong capture dates (EXIF times without a zone, QuickTime times that are zero or UTC, edited files)**  
  Mitigation: Store each date's source and confidence and show it in the Inspector, send low-confidence files to Unsorted/, and always preview before applying.
- **Qt churn: open-source patches for an LTS release stop after about a year, and aqtinstall may lag a change in Qt's download server**  
  Mitigation: The code targets the Qt 6.8 API, which the Debian container checks, so moving to 6.13+ or back to 6.11 is only a version bump. Pin the aqt version and cache Qt in CI.
- **Qt 6.12's Linux binaries may need a newer glibc than ubuntu:22.04 has, breaking the AppImage**  
  Mitigation: 0.1-T10 checks the GLIBC symbol versions in libQt6Core. If they are too new, the AppImage base moves to ubuntu:24.04 and the minimum distro is documented. Smoke tests run in Debian 12 and Fedora containers.
- **Running past the weekly Claude limit; GUI sessions use many tokens**  
  Mitigation: Task cards with at most 4 acceptance criteria, CLAUDE.md and ARCHITECTURE.md kept current, a fresh session per task, GUI and engine work never mixed, and any task that outgrows M gets split.
- **MinGW-specific problems (community vcpkg triplets, gaps in the Windows SDK headers, slower debugging)**  
  Mitigation: no compiler-specific extensions in the code; if MinGW blocks something, add the C++ workload to Build Tools 2022 and Qt's MSVC kit, then switch the Windows presets.
- **Flaky Windows tests from Defender or indexer file locks, or from changes to the GitHub runner image**  
  Mitigation: FsOps retries sharing violations with backoff, tests use unique temp folders, the build scripts pin Qt's MinGW toolchain explicitly, and the runner label is pinned to windows-2025.
- **Licence compliance slips (LGPL for Qt, MPL for TagLib)**  
  Mitigation: Qt is only ever linked dynamically. The release workflow checks THIRD_PARTY_LICENSES, the About dialog shows it, and every release carries source links for Qt and TagLib.
- **Unsigned binaries trigger SmartScreen and antivirus false positives**  
  Mitigation: Document the warning, publish SHA256SUMS, and sign through a free open-source signing programme from 0.9-T2; if it is not approved in time, 1.0 ships unsigned.

## Facts still to verify

- aqtinstall already lists Qt 6.12.x for windows/win64_mingw (CI) and linux/linux_gcc_64, and the module names are as assumed (qtimageformats; whether qtsvg is a separate module). Check with aqt list-qt.
- Qt 6.12.0 LTS is published under the LGPL in the open-source repository and gets open-source patch releases for about its first year; later LTS patches are commercial-only.
- Qt 6.12's official Linux x86_64 binaries run on ubuntu:22.04 (glibc 2.35). If not, the AppImage base moves to ubuntu:24.04.
- Debian 13 package names and versions: qt6-base-dev 6.8.2, the offscreen platform plugin in qt6-qpa-plugins (or libqt6gui6), Clang 19 with libFuzzer runtime packages, and libtag-dev 2.0.2.
- vcpkg port names, feature names and CMake targets: sqlite3[fts5], blake3, utf8proc, pugixml, miniz, taglib, catch2.
- The APPIMAGE_EXTRACT_AND_RUN=1 environment variable. The --appimage-extract-and-run flag is confirmed, and the plan unpacks the tools when the image is built, so neither is required.
- linuxdeploy and its Qt and AppImage plugins are MIT-licensed.
- The windows-2025 GitHub runner allows VHDs created with diskpart, and install-qt-action installs Qt 6.12's MinGW kit with the matching tools_mingw1310. The plan does not rely on a D: drive.
- Under MPL-1.1, linking TagLib statically into an MIT app only requires making TagLib's source available.
- renameat2(RENAME_NOREPLACE) returns EINVAL on vfat, exFAT and ntfs3; fstests will measure this. ext4 and tmpfs support it.
- Qt 6.12's windows11 style follows the Windows light/dark setting without extra code.
- Windows Sandbox can be enabled on this Windows 11 Pro machine (it needs virtualization enabled in the firmware).
- The performance targets (queries under 300 ms on 1M files, unchanged rescan under 60 s) are estimates until 0.8-T1 measures them.
- carbon (4 CPUs, 8 GB RAM) is fast enough for the fuzz, TSan and AppImage jobs; if not, those run only in GitHub Actions.
