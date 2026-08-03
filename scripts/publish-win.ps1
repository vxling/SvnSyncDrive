# Publishes the SvnSyncDrive Windows release: rebuilds the app, stages a clean
# portable layout (exe + Qt/SVN/OpenSSL DLLs + plugins), then produces
#   build\publish\SvnSyncDrive-<version>-win64.zip
#   build\publish\SvnSyncDrive-<version>-win64.msi   (WiX per-user MSI)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\publish-win.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\publish-win.ps1 -Version 0.2.0
#
# Requires: Visual Studio (any recent), CMake + Ninja, Qt 6 (see -QtRoot), and
# WiX 3.14 (candle.exe/light.exe/heat.exe, see -WixRoot). The staged libsvnplus
# install is taken from LIBSVNPLUS_ROOT (default: ..\LibSVNPlus\build\_stage).

[CmdletBinding()]
param(
    [string]$Version = "0.2.0",
    [string]$QtRoot = "C:\Users\xuser\Qt\6.11.1\msvc2022_64",
    [string]$LibSvnPlusRoot = "C:\Users\xuser\Documents\LibSVNPlus\build\_stage",
    [string]$WixRoot = "C:\Users\xuser\Tools\WiX314\tools",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Invoke-Shell {
    param([Parameter(Mandatory)][string]$Cmd, [string[]]$ArgsList = @())
    & $Cmd @ArgsList
    if ($LASTEXITCODE -ne 0) { throw "$Cmd failed with exit code $LASTEXITCODE" }
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scriptDir
if (-not (Test-Path -LiteralPath (Join-Path $root "CMakeLists.txt"))) {
    throw "Could not locate SvnSyncDrive root from $scriptDir"
}

if (-not (Test-Path -LiteralPath $QtRoot)) { throw "Qt root not found: $QtRoot" }
if (-not (Test-Path -LiteralPath $LibSvnPlusRoot)) {
    throw "libsvnplus stage not found: $LibSvnPlusRoot (build LibSVNPlus first)"
}
$candle = Join-Path $WixRoot "candle.exe"
$light = Join-Path $WixRoot "light.exe"
$heat = Join-Path $WixRoot "heat.exe"
foreach ($t in @($candle, $light, $heat)) {
    if (-not (Test-Path -LiteralPath $t)) { throw "WiX tool not found: $t (install WiX 3.14 and pass -WixRoot)" }
}

# ── Visual Studio environment for cmake/ninja ───────────────────────────────
$vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vsWhere)) { throw "vswhere not found" }
$vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation found" }
$vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
cmd /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process") }
}

$buildDir = Join-Path $root "build"

# ── Build (unless skipped) ───────────────────────────────────────────────────
if (-not $SkipBuild) {
    Invoke-Shell "cmake" @("-B", $buildDir, "-S", $root, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_PREFIX_PATH=$QtRoot", "-DLIBSVNPLUS_ROOT=$LibSvnPlusRoot")
    Invoke-Shell "cmake" @("--build", $buildDir)
}

$exe = Join-Path $buildDir "svnsyncdrive.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "svnsyncdrive.exe not found after build" }

# ── Stage a clean portable layout ───────────────────────────────────────────
$stageParent = Join-Path $buildDir "publish"
$stage = Join-Path $stageParent "SvnSyncDrive-$Version-win64"
if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item -LiteralPath $exe -Destination $stage
Get-ChildItem -LiteralPath $buildDir -Filter *.dll -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $stage
}
# windeployqt drops Qt plugin directories (platforms, imageformats, tls, ...)
# next to the executable at the top level of the build dir.
foreach ($d in @("generic", "iconengines", "imageformats", "networkinformation",
                 "platforms", "sqldrivers", "styles", "tls", "vc_redist.x64.exe")) {
    $src = Join-Path $buildDir $d
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $stage -Recurse }
}

# ── Green zip ───────────────────────────────────────────────────────────────
$zip = Join-Path $stageParent "SvnSyncDrive-$Version-win64.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

# ── MSI installer (WiX, per-user) ─────────────────────────────────────────────
$wixWork = Join-Path $buildDir "wix"
if (Test-Path -LiteralPath $wixWork) { Remove-Item -Recurse -Force -LiteralPath $wixWork }
New-Item -ItemType Directory -Force -Path $wixWork | Out-Null
$componentsWxs = Join-Path $wixWork "components.wxs"
Invoke-Shell $heat @("dir", $stage, "-out", $componentsWxs, "-cg", "MainComponentGroup",
                     "-gg", "-scom", "-sreg", "-srd", "-sfrag", "-su", "-dr", "INSTALLDIR",
                     "-var", "var.StageDir", "-sw5150")
$mainWxs = Join-Path $scriptDir "svn_sync_drive.wxs"
Invoke-Shell $candle @("-dStageDir=$stage", "-dVersion=$Version", "-ext", "WixUIExtension",
                       $mainWxs, $componentsWxs, "-out", "$wixWork\")
$msi = Join-Path $stageParent "SvnSyncDrive-$Version-win64.msi"
if (Test-Path -LiteralPath $msi) { Remove-Item -LiteralPath $msi }
Invoke-Shell $light @("-ext", "WixUIExtension", "-cultures:en-us",
                      "-sice:ICE38", "-sice:ICE64",
                      (Join-Path $wixWork "svn_sync_drive.wixobj"),
                      (Join-Path $wixWork "components.wixobj"),
                      "-o", $msi)

Get-ChildItem -LiteralPath $stageParent | Select-Object Name, Length
