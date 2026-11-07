<#
.SYNOPSIS
  Copy the working tree to carbon and run a compose service there (Linux build + tests).
.EXAMPLE
  scripts\linux-test.ps1            # service "test": linux-debug build + all ctest suites
  scripts\linux-test.ps1 asan       # also: tsan, tidy
.NOTES
  Uncommitted changes are included. ~/Workspace/sheepdog on carbon is a git clone, which this script
  leaves alone: the copy goes to ~/Workspace/sheepdog-ci, replaced on every run. The tree is packed
  with tar into a temp file and sent
  with scp: piping binary data through Windows PowerShell 5.1 would corrupt it.
  The full log stays on carbon in ~/Workspace/sheepdog-<service>.log; only the tail is printed.
#>
param(
    [string]$Service = 'test',
    [string]$HostName = 'carbon',
    [int]$TailLines = 40
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$archive = Join-Path $env:TEMP 'sheepdog-src.tar.gz'
$remoteDir = '~/Workspace/sheepdog-ci'

Write-Host "Packing $repo ..."
& "$env:WINDIR\System32\tar.exe" -czf $archive --exclude=./build --exclude=./.git --exclude=./.idea `
    --exclude=./.claude --exclude=./dist --exclude='./cmake-build-*' --exclude=./CMakeUserPresets.json -C $repo .
if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }

Write-Host "Copying to ${HostName}:$remoteDir ..."
& scp -q $archive "${HostName}:/tmp/sheepdog-src.tar.gz"
if ($LASTEXITCODE -ne 0) { throw "scp failed ($LASTEXITCODE)" }

$log = "~/Workspace/sheepdog-$Service.log"
$remote = "set -e; rm -rf $remoteDir; mkdir -p $remoteDir; tar -xzf /tmp/sheepdog-src.tar.gz -C $remoteDir; " +
          "cd $remoteDir; docker compose -p sheepdog build -q > $log 2>&1 || { tail -n $TailLines $log; exit 1; }; " +
          "set +e; docker compose -p sheepdog run --rm -T $Service >> $log 2>&1; rc=`$?; tail -n $TailLines $log; exit `$rc"
Write-Host "Running '$Service' on $HostName (full log: $log) ..."
& ssh $HostName $remote
$rc = $LASTEXITCODE
if ($rc -eq 0) { Write-Host "OK: $Service passed on $HostName" -ForegroundColor Green }
else { Write-Host "FAILED: $Service on $HostName (exit $rc)" -ForegroundColor Red }
exit $rc
