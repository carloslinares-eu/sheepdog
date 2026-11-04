# Sheepdog

Sheepdog (working name) catalogs your files, lets you tag and query them, and organises them
into folders by copying or moving them. It shows a preview of every change and keeps an undo history.
It runs on Windows and Linux, and never changes a file without showing you the plan first.

**Status:** early development, nothing to download yet. See [docs/RELEASE_PLAN.md](docs/RELEASE_PLAN.md)
for the road to 1.0 and [docs/DESIGN.md](docs/DESIGN.md) for the design.

## Building

Windows (MinGW-w64 GCC 13.1, Qt 6.12, vcpkg):

```powershell
scripts\check-env.ps1     # checks the toolchain
scripts\test.ps1          # configures the win-debug preset, builds and runs the tests
```

Linux builds run in containers on a Linux host. The full setup is in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md); how to contribute is in [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md).

## Licence

Sheepdog is released under the [MIT licence](LICENSE). It uses Qt under the LGPL-3.0 and other
third-party components listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
