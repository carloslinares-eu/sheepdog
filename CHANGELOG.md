# Changelog

All notable changes to Sheepdog are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Release tags are named `sheepdog-X.Y.Z`.

## [Unreleased]

### Added
- Development setup for Windows (MinGW toolchain) and carbon: `docs/DEVELOPMENT.md` and `scripts/check-env.ps1` (0.1-T1).
- Repo bootstrap: CMake targets (`sheepdog_core`, `sheepdog-cli`, a `sheepdog` GUI stub, Catch2 tests), CMake presets, vcpkg manifest, git-describe versioning, Windows manifest and version resource, build and test scripts, and the docs skeleton (0.1-T2).
- Linux test container (`docker/ci.Dockerfile`, `compose.yaml` with test, asan, tsan, tidy and shell services), `scripts/linux-test.ps1` to run them on carbon, and a GitHub Actions CI skeleton with a Windows MinGW job and a Linux container job (0.1-T3). The Windows CI job uses Qt 6.10.3 because aqtinstall cannot install Qt 6.11+ yet.
- Platform layer, read side (`core/platform/`): `Result<T>` errors, the `NativePath` codec (WTF-8 on Windows, raw bytes on Linux, plus a UTF-8 display string), sorted folder listings that never follow symlinks or junctions, file details with a cloud-placeholder flag, file identity, volume info (uid, label, filesystem, case-sensitivity probe, HDD/SSD, remote) and a `FileReader`; Windows paths get the `\\?\` prefix so paths over 260 characters work. Tests build real temp trees through `tests/support/temp_tree.hpp` (0.1-T4).
- Catalog database (`core/catalog/`): an SQLite RAII wrapper, schema v1 (volume, root, file, meta), `user_version` migrations with an `sqlite3_backup` first (10 backups kept), WAL and pragmas, `integrity_check` on open with a clear refusal that names the newest backup, refusal of catalogs on UNC, mapped network drives and NFS/SMB mounts, a DB-writer thread committing about 1,000 rows per transaction (100k rows in about 1 s), and read-only connections. Links now report size 0 on Linux too, matching Windows (0.1-T5).
