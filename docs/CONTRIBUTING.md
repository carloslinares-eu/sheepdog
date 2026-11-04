# Contributing to Sheepdog

## Workflow

- Work is planned as task cards in [RELEASE_PLAN.md](RELEASE_PLAN.md). One task is one commit,
  with the message `<task id>: <short title>`, e.g. `0.2-T4: planner`.
- A task is done when its "Done when" criteria pass through `scripts/test.ps1` (Windows) and
  the Linux container on carbon, CHANGELOG.md has a line under *Unreleased*, and the task is ticked.
- Interface changes update [ARCHITECTURE.md](ARCHITECTURE.md) in the same commit.

## Code conventions

- C++20, formatted with `.clang-format`, checked with `.clang-tidy`.
- `sheepdog_core` never includes Qt. Only `platform/` may use `#ifdef _WIN32`.
- Platform calls return `Result<T>`; no exceptions cross threads.
- Parse numbers with `std::from_chars`. Do not use `std::format` or `<chrono>` time zones
  (the AppImage builds with GCC 12).
- Files stay under about 500 lines.
- Only `FsOps` may change files on disk. Tests work in temporary folders, and test and debug
  builds refuse to change files outside `SHEEPDOG_SANDBOX`.
- Warnings are errors in CI (`WARNINGS_AS_ERRORS=ON`, preset `win-ci`).

## Tests

Catch2 v3. Tag every test case with one of the labels `unit fs gui fault slow win linux`;
the tags become ctest labels, so `ctest -L unit` runs one group. GUI tests run with
`QT_QPA_PLATFORM=offscreen`.

## Setting up

See [DEVELOPMENT.md](DEVELOPMENT.md).
