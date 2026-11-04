# Dot-source this file (. scripts\env.ps1) to put the Windows toolchain on PATH:
# MinGW-w64 GCC 13.1, CMake 3.30 and Ninja from C:\Qt\Tools, plus Qt's bin folder.
# Override the locations with SHEEPDOG_QT_DIR and SHEEPDOG_QT_TOOLS.

$QtTools = if ($env:SHEEPDOG_QT_TOOLS) { $env:SHEEPDOG_QT_TOOLS } else { 'C:\Qt\Tools' }
if (-not $env:SHEEPDOG_QT_DIR) { $env:SHEEPDOG_QT_DIR = 'C:/Qt/6.12.0/mingw_64' }

$SheepdogToolDirs = @(
    (Join-Path $QtTools 'mingw1310_64\bin'),
    (Join-Path $QtTools 'CMake_64\bin'),
    (Join-Path $QtTools 'Ninja'),
    (Join-Path $env:SHEEPDOG_QT_DIR 'bin')
)
foreach ($dir in $SheepdogToolDirs) {
    if (-not (Test-Path $dir)) { throw "Toolchain folder not found: $dir (see docs/DEVELOPMENT.md)" }
}
$env:PATH = (($SheepdogToolDirs | ForEach-Object { [IO.Path]::GetFullPath($_) }) -join ';') + ';' + $env:PATH

# A shell started before VCPKG_ROOT was set does not see it yet: read the stored value.
if (-not $env:VCPKG_ROOT) {
    $env:VCPKG_ROOT = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
    if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'Machine') }
}
if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set (see docs/DEVELOPMENT.md)' }

$SheepdogRoot = Split-Path -Parent $PSScriptRoot

# CLion and the dev-* presets read the Qt path from the git-ignored CMakeUserPresets.json.
$userPresets = Join-Path $SheepdogRoot 'CMakeUserPresets.json'
if (-not (Test-Path $userPresets)) {
    $template = Get-Content -Raw (Join-Path $SheepdogRoot 'cmake\CMakeUserPresets.template.json')
    $qtDir = $env:SHEEPDOG_QT_DIR -replace '\\', '/'
    [IO.File]::WriteAllText($userPresets, $template.Replace('@QT_DIR@', $qtDir))
    Write-Host "Created CMakeUserPresets.json for Qt in $qtDir"
}
