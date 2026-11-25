<#
.SYNOPSIS
  Build CADX on Windows: engine, tests, UI, and the NSIS installer.

.DESCRIPTION
  Needs Visual Studio 2022+ (or Build Tools) with the C++ workload, Python 3,
  Node 20+ with pnpm, Rust stable and the Tauri CLI (cargo install tauri-cli).
  Dependencies:
    -OcctDir   an OpenCASCADE 7.6+ install with cmake/OpenCASCADEConfig.cmake
               (the official "opencascade-7.9.3-vc14-64-combined.zip" works)
    -VcpkgRoot a vcpkg checkout with, for x64-windows:
               nlohmann-json eigen3 boost-graph boost-math gtest

.EXAMPLE
  .\scripts\build-windows.ps1 -OcctDir C:\deps\occt\opencascade-7.9.3-vc14-64 -VcpkgRoot C:\deps\vcpkg
#>
param(
  [Parameter(Mandatory = $true)] [string] $OcctDir,
  [Parameter(Mandatory = $true)] [string] $VcpkgRoot,
  [string] $BuildDir = "build-win",
  [switch] $SkipTests,
  [switch] $SkipInstaller
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" } }

# CMake: use the one on PATH, else the one that ships with Visual Studio.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if (-not (Test-Path $cmake)) { throw "CMake not found" }

$occtBin = Join-Path $OcctDir "win64\vc14\bin"
$thirdParty = Join-Path (Split-Path -Parent $OcctDir) "3rdparty-vc14-64"
$vcpkgBin = Join-Path $VcpkgRoot "installed\x64-windows\bin"

Step "Configure engine"
& $cmake -S engine -B $BuildDir -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
  "-DVCPKG_MANIFEST_MODE=OFF" "-DVCPKG_TARGET_TRIPLET=x64-windows" `
  "-DOpenCASCADE_DIR=$OcctDir\cmake"
Check "cmake configure"

Step "Build engine"
& $cmake --build $BuildDir --config Release --parallel
Check "engine build"

# DLLs the engine and tests load at run time.
$dllDirs = @($occtBin, $vcpkgBin) + (Get-ChildItem $thirdParty -Recurse -Filter *.dll -ErrorAction SilentlyContinue |
  ForEach-Object { $_.DirectoryName } | Sort-Object -Unique)
$env:PATH = ($dllDirs -join ";") + ";" + $env:PATH

if (-not $SkipTests) {
  Step "Engine tests"
  & "$BuildDir\Release\cadx-tests.exe" --gtest_brief=1
  Check "engine tests"
}

Step "Stage engine and DLLs for the bundle"
# MSVC runtime, so the installer works without the VC++ redistributable.
$crt = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory -ErrorAction SilentlyContinue |
  Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) {
  $crt = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
}
$stageDirs = @("$BuildDir\Release") + $dllDirs
if ($crt) { $stageDirs += $crt.FullName }
python scripts\stage_engine.py "$BuildDir\Release\cadx-engine.exe" x86_64-pc-windows-msvc @stageDirs
Check "staging"

Step "Build UI"
pnpm install --dir ui --frozen-lockfile
Check "pnpm install"

if (-not $SkipInstaller) {
  Step "Build installer"
  Push-Location app
  cargo tauri build --config src-tauri/tauri.bundle.conf.json
  $code = $LASTEXITCODE
  Pop-Location
  if ($code -ne 0) { throw "tauri build failed" }
  $setup = Get-ChildItem app\src-tauri\target\release\bundle\nsis\*.exe | Select-Object -First 1
  Write-Host "`nInstaller: $($setup.FullName)" -ForegroundColor Green
}
