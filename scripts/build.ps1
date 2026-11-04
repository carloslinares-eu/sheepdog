<#
.SYNOPSIS
  Configures and builds Sheepdog with a CMake preset (default win-debug).
.EXAMPLE
  scripts\build.ps1
  scripts\build.ps1 -Preset win-release
  scripts\build.ps1 -Preset win-ci -Target sheepdog-cli
#>
param(
    [string]$Preset = 'win-debug',
    [string]$Target = '',
    [switch]$Fresh
)
# Native tools report failure through $LASTEXITCODE; 'Stop' would turn their
# stderr output (warnings, progress) into terminating errors in PowerShell 5.1.
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'env.ps1')

Push-Location $SheepdogRoot
try {
    $configureArgs = @('--preset', $Preset)
    if ($Fresh) { $configureArgs += '--fresh' }
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for preset $Preset" }

    $buildArgs = @('--build', '--preset', $Preset)
    if ($Target) { $buildArgs += @('--target', $Target) }
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed for preset $Preset" }
}
finally {
    Pop-Location
}
