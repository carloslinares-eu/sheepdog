<#
.SYNOPSIS
  Checks the Windows development environment for Sheepdog (see docs/DEVELOPMENT.md).
.DESCRIPTION
  Checks the Qt 6.12 MinGW kit, MinGW-w64 GCC 13.1, CMake, Ninja, VCPKG_ROOT and that
  Docker answers on carbon over SSH. Prints one line per check and exits with 1 if any fails.
  Override the default locations with SHEEPDOG_QT_DIR and SHEEPDOG_QT_TOOLS.
#>
param(
    [string]$LinuxHost = 'carbon'
)
$ErrorActionPreference = 'Continue'

$QtTools = if ($env:SHEEPDOG_QT_TOOLS) { $env:SHEEPDOG_QT_TOOLS } else { 'C:\Qt\Tools' }
$QtDir = if ($env:SHEEPDOG_QT_DIR) { $env:SHEEPDOG_QT_DIR } else { 'C:\Qt\6.12.0\mingw_64' }
$script:Failures = 0

function Report([string]$Name, [bool]$Ok, [string]$Detail) {
    if ($Ok) {
        Write-Host ('[ OK ] {0,-22} {1}' -f $Name, $Detail) -ForegroundColor Green
    } else {
        Write-Host ('[FAIL] {0,-22} {1}' -f $Name, $Detail) -ForegroundColor Red
        $script:Failures++
    }
}

# Runs a native tool and returns its first stdout line, or $null when it cannot run.
function First-Line([string]$Exe, [string[]]$Arguments) {
    if (-not (Test-Path $Exe)) { return $null }
    try {
        $out = & $Exe @Arguments
        if ($LASTEXITCODE -ne 0) { return $null }
        return ($out | Select-Object -First 1)
    } catch {
        return $null
    }
}

# Qt 6.12 MinGW kit
$qtVersionFile = Join-Path $QtDir 'lib\cmake\Qt6\Qt6ConfigVersionImpl.cmake'
$qtVersion = $null
if (Test-Path $qtVersionFile) {
    $m = Select-String -Path $qtVersionFile -Pattern 'set\(PACKAGE_VERSION "([0-9.]+)"\)' | Select-Object -First 1
    if ($m) { $qtVersion = $m.Matches[0].Groups[1].Value }
}
$qtOk = ($qtVersion -like '6.12.*') -and (Test-Path (Join-Path $QtDir 'bin\Qt6Widgets.dll'))
Report 'Qt 6.12 MinGW kit' $qtOk $(if ($qtVersion) { "$qtVersion in $QtDir" } else { "not found in $QtDir" })

# MinGW-w64 GCC 13.1
$gcc = Join-Path $QtTools 'mingw1310_64\bin\g++.exe'
$gccLine = First-Line $gcc @('--version')
Report 'MinGW-w64 GCC 13.1' ($gccLine -match ' 13\.1\.') $(if ($gccLine) { $gccLine } else { "not found: $gcc" })

# CMake (3.25 or newer; the Qt-bundled 3.30 is the one the scripts use)
$cmake = Join-Path $QtTools 'CMake_64\bin\cmake.exe'
$cmakeLine = First-Line $cmake @('--version')
$cmakeOk = $false
if ($cmakeLine -match 'version (\d+)\.(\d+)') {
    $cmakeOk = ([int]$Matches[1] -gt 3) -or (([int]$Matches[1] -eq 3) -and ([int]$Matches[2] -ge 25))
}
Report 'CMake' $cmakeOk $(if ($cmakeLine) { "$cmakeLine ($cmake)" } else { "not found: $cmake" })

# Ninja
$ninja = Join-Path $QtTools 'Ninja\ninja.exe'
$ninjaLine = First-Line $ninja @('--version')
Report 'Ninja' ($null -ne $ninjaLine) $(if ($ninjaLine) { "$ninjaLine ($ninja)" } else { "not found: $ninja" })

# VCPKG_ROOT (a shell started before it was set only sees the stored value)
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) { $vcpkgRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User') }
if (-not $vcpkgRoot) { $vcpkgRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'Machine') }
$vcpkgOk = $vcpkgRoot -and (Test-Path (Join-Path $vcpkgRoot 'vcpkg.exe')) -and
           (Test-Path (Join-Path $vcpkgRoot 'triplets\community\x64-mingw-static.cmake'))
Report 'VCPKG_ROOT' ([bool]$vcpkgOk) $(if ($vcpkgRoot) { $vcpkgRoot } else { 'not set' })

# Docker on the Linux host over SSH
$sshExe = (Get-Command ssh -ErrorAction SilentlyContinue).Source
$dockerVersion = $null
if ($sshExe) {
    $dockerVersion = First-Line $sshExe @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
        $LinuxHost, 'docker', 'info', '--format', '{{.ServerVersion}}')
}
Report "ssh $LinuxHost docker info" ([bool]$dockerVersion) $(if ($dockerVersion) { "Docker $dockerVersion" } else { 'no answer' })

if ($script:Failures -gt 0) {
    Write-Host "$($script:Failures) check(s) failed. See docs/DEVELOPMENT.md." -ForegroundColor Red
    exit 1
}
Write-Host 'All checks passed.' -ForegroundColor Green
