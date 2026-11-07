# Development setup

Sheepdog is developed on Windows with the MinGW-w64 toolchain that ships with Qt, and tested
on Linux inside Docker containers on a separate Linux host called **carbon**. Nothing
Docker- or WSL-related is installed on Windows. There is no MSVC build at the moment; the code
avoids compiler-specific extensions so one can be added later.

## Windows machine

| Tool | Location | Notes |
|---|---|---|
| Qt 6.12.0 MinGW kit | `C:\Qt\6.12.0\mingw_64` | Qt Online Installer. Base package includes Qt SVG, Linguist and windeployqt. Add Qt Image Formats before v0.7. |
| MinGW-w64 GCC 13.1 | `C:\Qt\Tools\mingw1310_64\bin` | Installed with Qt (component "MinGW 13.1.0 64-bit"). |
| CMake 3.30 | `C:\Qt\Tools\CMake_64\bin` | Installed with Qt. An older CMake elsewhere on PATH is ignored by the scripts. |
| Ninja 1.12 | `C:\Qt\Tools\Ninja` | Installed with Qt. |
| vcpkg | `F:\Workspace\vcpkg`, environment variable `VCPKG_ROOT` | A normal git clone, bootstrapped with `bootstrap-vcpkg.bat`. |
| Git | on PATH | `git config --global core.longpaths true`. |
| OpenSSH client | Windows built-in | For `ssh carbon`. |

Also enable Win32 long paths once (admin): registry
`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`, `LongPathsEnabled = 1`.

Check everything with:

```powershell
scripts\check-env.ps1
```

It checks the Qt kit version, GCC 13.1, CMake, Ninja, `VCPKG_ROOT` and `ssh carbon docker info`,
and exits with 1 if anything is missing. Other locations can be given through the environment
variables `SHEEPDOG_QT_DIR` (the kit, default `C:/Qt/6.12.0/mingw_64`) and `SHEEPDOG_QT_TOOLS`
(default `C:\Qt\Tools`).

### Building and testing from the terminal

The scripts run in Windows PowerShell 5.1 and put MinGW, CMake, Ninja and Qt's `bin` folder at
the front of PATH for their own process (`scripts\env.ps1`).

```powershell
scripts\build.ps1                       # configure + build the win-debug preset
scripts\build.ps1 -Preset win-release
scripts\test.ps1                        # build win-debug and run every test
scripts\test.ps1 -Filter version        # ctest -R version
scripts\test.ps1 -Label gui             # ctest -L gui
scripts\test.ps1 -Preset win-ci         # warnings as errors, as in CI
```

Build folders are `build\<preset>`. vcpkg installs the dependencies into `build\vcpkg_installed`
on the first configure (a few minutes) and keeps built packages in its binary cache
(`%LOCALAPPDATA%\vcpkg\archives`), so later configures are fast.

To run the GUI from a terminal, put the toolchain on PATH first:
`. scripts\env.ps1; build\win-debug\src\gui\sheepdog.exe`.

### vcpkg and MinGW

The dependencies (`sqlite3[fts5]`, `blake3`, `catch2`) come from `vcpkg.json` in manifest mode.
Its `builtin-baseline` is the vcpkg commit the clone was at when the manifest was written, so
every machine resolves the same versions; update it with `vcpkg x-update-baseline` in the repo.

Windows uses vcpkg's community triplet **`x64-mingw-static`** for both the target and the host
(`VCPKG_TARGET_TRIPLET` and `VCPKG_HOST_TRIPLET` in `CMakePresets.json`), so vcpkg builds every
port with the MinGW GCC on PATH and never looks for MSVC. The triplet passes PATH through to the
port builds, which is why the scripts put MinGW first on PATH. Linux uses `x64-linux`.

### CMake presets

`CMakePresets.json` (committed) defines `win-debug`, `win-release`, `win-ci`, `linux-debug`,
`linux-release`, `linux-asan`, `linux-tsan` and `linux-fuzz`. The Windows presets find Qt through
`$env{SHEEPDOG_QT_DIR}`, which the scripts set.

`CMakeUserPresets.json` is git-ignored and holds the local Qt path. `scripts\env.ps1` creates it
from `cmake\CMakeUserPresets.template.json` the first time a script runs. It adds `dev-debug` and
`dev-release`, which inherit `win-debug` and `win-release` and set `SHEEPDOG_QT_DIR`. Use these
two in CLion.

### CLion

1. Settings, Build, Execution, Deployment, Toolchains: add **MinGW** with the folder
   `C:\Qt\Tools\mingw1310_64` and CMake `C:\Qt\Tools\CMake_64\bin\cmake.exe`; move it to the top.
2. Run `scripts\build.ps1` once so `CMakeUserPresets.json` exists.
3. Settings, CMake: enable the presets **dev-debug** and **dev-release** and delete the default
   profile.
4. To run `sheepdog` or the GUI tests from CLion, add `C:\Qt\6.12.0\mingw_64\bin` to PATH in the
   run configuration's environment (or in the toolchain's environment file).

CLion's `.idea\` and `cmake-build-*\` folders are git-ignored.

## carbon (Linux host)

carbon runs Debian 13 with Docker 26.1 (4 CPUs, 8 GB RAM). It has no display and no CMake or Qt
on the host: everything Linux-related builds and runs inside containers. The user `carlos` is in
the `docker` group, so no sudo is needed.

`%USERPROFILE%\.ssh\config` on the Windows machine:

```
Host carbon
    HostName carbon
    User carlos
    IdentityFile ~/.ssh/cicero_carbon
    IdentitiesOnly yes
```

### Getting the code there

Uncommitted work is tested without pushing: the working tree is packed with `tar` into a temporary
file (excluding `.git`, `build*` and `cmake-build-*`), copied with `scp` to carbon and unpacked into
`~/Workspace/sheepdog-ci`, which each run replaces. `~/Workspace/sheepdog` is a plain git clone that
the scripts leave alone. Never pipe tar through ssh in Windows PowerShell 5.1: its pipeline corrupts binary
data. From task 0.1-T3, `scripts\linux-test.ps1 <service>` does this and then runs
`docker compose -p sheepdog run --rm <service>` from `compose.yaml` with the image `docker/ci.Dockerfile`
(services `test`, `asan`, `tsan`, `tidy`, `shell`). Build with `-j4` and run the heavy jobs (TSan,
fuzzing, AppImage) one at a time.

To repeat tests without a rebuild, for example to catch a rare race, run them in the `shell`
service on carbon. It sees the same build volume as the other services:

```sh
cd ~/Workspace/sheepdog-ci
docker compose -p sheepdog run --rm -T shell ctest --test-dir /build/linux-tsan -R "queue cap" --repeat until-fail:20
```

Pinning a run to one CPU (`taskset -c 0`) forces other thread orders and finds timing bugs far
sooner: a test that hung in 3 of 115 free runs hung in every pinned one. Give each repeated run a
`timeout`, since `ctest --repeat` and a bare test binary wait for ever.

Until 0.1-T3 lands there is no committed Linux image; the `linux-*` presets need GCC or Clang,
CMake 3.25+, Ninja, Qt 6.8+ development packages and vcpkg with `VCPKG_ROOT` set.

### Seeing the Linux GUI

carbon is headless. GUI tests run with `QT_QPA_PLATFORM=offscreen`; for a live window use a Linux
desktop or WSLg later.
