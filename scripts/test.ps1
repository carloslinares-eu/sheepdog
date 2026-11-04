<#
.SYNOPSIS
  Builds Sheepdog with a CMake preset (default win-debug) and runs ctest.
.EXAMPLE
  scripts\test.ps1
  scripts\test.ps1 -Filter version        # ctest -R version
  scripts\test.ps1 -Label gui             # ctest -L gui
#>
param(
    [string]$Preset = 'win-debug',
    [string]$Filter = '',
    [string]$Label = '',
    [switch]$Fresh
)
# Native tools report failure through $LASTEXITCODE; 'Stop' would turn their
# stderr output (warnings, progress) into terminating errors in PowerShell 5.1.
$ErrorActionPreference = 'Continue'
& (Join-Path $PSScriptRoot 'build.ps1') -Preset $Preset -Fresh:$Fresh
. (Join-Path $PSScriptRoot 'env.ps1')

Push-Location $SheepdogRoot
try {
    $testArgs = @('--preset', $Preset)
    if ($Filter) { $testArgs += @('-R', $Filter) }
    if ($Label) { $testArgs += @('-L', $Label) }
    & ctest @testArgs
    if ($LASTEXITCODE -ne 0) { throw "Tests failed for preset $Preset" }
}
finally {
    Pop-Location
}
