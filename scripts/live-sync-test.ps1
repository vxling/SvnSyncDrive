# Live sync test for SvnSyncDrive: creates a throwaway local SVN repository
# with two working copies, then runs the automated two-way live sync test:
#   - upward:  a file added in wc-app is auto-committed by the engine
#   - downward: a file committed by "another user" in wc-other is pulled in
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File live-sync-test.ps1
#   powershell -ExecutionPolicy Bypass -File live-sync-test.ps1 -Keep
#        -Keep keeps the repository + working copies (paths are printed)
#        for manual inspection / GUI testing afterwards.
#
# Exit code is the exit code of synccoretest.

[CmdletBinding()]
param(
    [string]$SvnBin = "C:\Users\xuser\Documents\LibSVNPlus\build\_stage\bin",
    [string]$TestExe = "C:\Users\xuser\Documents\SvnSyncDrive\build\synccoretest.exe",
    [switch]$Keep
)

$ErrorActionPreference = "Stop"

$svnadmin = Join-Path $SvnBin "svnadmin.exe"
$svn = Join-Path $SvnBin "svn.exe"
if (-not (Test-Path -LiteralPath $svnadmin)) { throw "svnadmin not found: $svnadmin" }
if (-not (Test-Path -LiteralPath $svn)) { throw "svn not found: $svn" }
if (-not (Test-Path -LiteralPath $TestExe)) { throw "synccoretest not found: $TestExe" }

$root = Join-Path $env:TEMP ("svnsync-live-" + [guid]::NewGuid().ToString("N"))
$repo = Join-Path $root "repo"
$wcApp = Join-Path $root "wc-app"
$wcOther = Join-Path $root "wc-other"
New-Item -ItemType Directory -Force -Path $repo | Out-Null

try {
    & $svnadmin create $repo
    if ($LASTEXITCODE -ne 0) { throw "svnadmin create failed" }

    $url = "file:///" + ($repo -replace "\\", "/")
    & $svn mkdir -m "init trunk" "$url/trunk"
    if ($LASTEXITCODE -ne 0) { throw "svn mkdir trunk failed" }

    & $svn checkout "$url/trunk" $wcApp
    if ($LASTEXITCODE -ne 0) { throw "checkout wc-app failed" }

    & $svn checkout "$url/trunk" $wcOther
    if ($LASTEXITCODE -ne 0) { throw "checkout wc-other failed" }

    Write-Host "repo   : $url/trunk"
    Write-Host "wc-app : $wcApp"
    Write-Host "wc-other: $wcOther"
    Write-Host ""

    # Live BreakLock / GetLastChangedTime integration (real worker + libsvnplus):
    # lock a committed file, then let the test break that lock with svn unlock --force.
    $lockFile = "lockme-" + [guid]::NewGuid().ToString("N").Substring(0, 8) + ".txt"
    $lockPath = Join-Path $wcApp $lockFile
    Set-Content -LiteralPath $lockPath -Value "lock test" -Encoding UTF8
    & $svn add $lockPath
    if ($LASTEXITCODE -ne 0) { throw "svn add lock file failed" }
    & $svn commit -m "locktest: add lock file" $lockPath
    if ($LASTEXITCODE -ne 0) { throw "svn commit lock file failed" }
    & $svn lock $lockPath
    if ($LASTEXITCODE -ne 0) { throw "svn lock failed" }

    & $TestExe --locktest $wcApp $lockFile
    if ($LASTEXITCODE -ne 0) {
        Write-Host "locktest failed"
        exit $LASTEXITCODE
    }

    # No lock should remain on the file after --locktest broke it.
    $locksInfo = (& $svn info --show-item locks $lockPath) -join ""
    if (-not [string]::IsNullOrWhiteSpace($locksInfo)) {
        throw "break-lock did not clear the lock"
    }

    & $TestExe --livesync "$url/trunk" $wcApp $wcOther
    $exit = $LASTEXITCODE
    exit $exit
}
finally {
    if ($Keep) {
        Write-Host ""
        Write-Host "Kept fixture (use --repo to run the GUI against wc-app):"
        Write-Host "  repo   : $url/trunk"
        Write-Host "  wc-app : $wcApp"
        Write-Host "  wc-other: $wcOther"
    } else {
        Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
    }
}
