# Third-party licences

Sheepdog itself is MIT-licensed (see [LICENSE](LICENSE)). It uses the components below.
The full licence texts are added to this file, and shown in the About dialog, before the
first binary release (v0.1.0).

| Component | Used for | Licence | Shipped in binaries | Source |
|---|---|---|---|---|
| Qt 6 (Core, Gui, Widgets) | GUI | LGPL-3.0, linked dynamically with unmodified libraries | yes | https://download.qt.io/official_releases/qt/ |
| SQLite 3 | Catalog | Public domain | yes (static) | https://sqlite.org |
| BLAKE3 (C implementation) | Content hashes | CC0-1.0, Apache-2.0, or Apache-2.0 WITH LLVM-exception | yes (static) | https://github.com/BLAKE3-team/BLAKE3 |
| Catch2 v3 | Tests only | BSL-1.0 | no | https://github.com/catchorg/Catch2 |
| MinGW-w64 runtime (libgcc, libstdc++, winpthread) | Windows runtime libraries | GPL-3.0 with the GCC Runtime Library Exception; winpthread under MIT-style terms | yes (Windows) | https://www.mingw-w64.org |
