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
