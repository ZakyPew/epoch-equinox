# One-command setup for Epoch & Equinox on Windows.
#
#   powershell -ExecutionPolicy Bypass -File setup.ps1
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -NoRun
#
# Safe to re-run: the build is incremental and pip is a no-op once satisfied.
#
# Needs Visual Studio 2019+ with the "Desktop development with C++" workload
# (the free Community edition is fine), CMake, Python 3, and vcpkg for SDL2
# and libcurl. It will tell you which of those are missing rather than
# failing halfway through a build.

[CmdletBinding()]
param([switch]$NoRun)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Say  { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }
function Fail { param($m) Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }

# --------------------------------------------------------------------------
# 1. tools
# --------------------------------------------------------------------------
Say 'Checking build tools'

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail @'
CMake not found.

    winget install Kitware.CMake

Then open a new terminal so PATH is picked up.
'@
}

$python = (Get-Command python -ErrorAction SilentlyContinue) ??
          (Get-Command python3 -ErrorAction SilentlyContinue)
if (-not $python) {
    Fail @'
Python 3 not found.

    winget install Python.Python.3.12
'@
}

# MSVC is found via CMake's generator, but check early so the failure is
# legible rather than a wall of CMake output.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if (-not $vs) {
        Fail @'
Visual Studio is installed but without the C++ toolchain.

Open the Visual Studio Installer, Modify your installation, and tick
"Desktop development with C++".
'@
    }
    Write-Host "    MSVC: $vs"
} else {
    Fail @'
Visual Studio not found.

    winget install Microsoft.VisualStudio.2022.Community

During install, tick "Desktop development with C++".
'@
}

# --------------------------------------------------------------------------
# 2. SDL2 + libcurl via vcpkg
# --------------------------------------------------------------------------
Say 'Checking SDL2 and libcurl'

$toolchain = $null
if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
    $toolchain = "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
    Write-Host "    vcpkg: $env:VCPKG_ROOT"
    & "$env:VCPKG_ROOT\vcpkg.exe" install sdl2:x64-windows curl:x64-windows
} else {
    Fail @'
vcpkg not found (VCPKG_ROOT is unset or wrong).

    git clone https://github.com/microsoft/vcpkg C:\vcpkg
    C:\vcpkg\bootstrap-vcpkg.bat
    setx VCPKG_ROOT C:\vcpkg

Then open a new terminal and re-run this script. It will install SDL2 and
libcurl for you.
'@
}

# --------------------------------------------------------------------------
# 3. ROMs -- required before anything can be built
# --------------------------------------------------------------------------
Say 'Checking for ROMs'
New-Item -ItemType Directory -Force -Path roms | Out-Null
$roms = Get-ChildItem roms -Include *.gb, *.gbc, *.sgb -File -ErrorAction SilentlyContinue

if (-not $roms) {
    Write-Host @'

  No ROMs in roms\.

  This project ships no game code -- it recompiles the games from your own
  dumps, so a ROM has to be there before anything can be built:

      roms\tlozooa.gbc     The Legend of Zelda: Oracle of Ages   (USA, Australia)
      roms\tlozoos.gbc     The Legend of Zelda: Oracle of Seasons (USA, Australia)

  Either one alone is fine -- you will just get that game.

'@ -ForegroundColor Yellow
    exit 1
}
$roms | ForEach-Object { Write-Host "    found: $($_.Name)" }

# --------------------------------------------------------------------------
# 4. build
# --------------------------------------------------------------------------
Say 'Configuring -- builds the recompiler, then turns your ROMs into C'
Write-Host '    (first run: a couple of minutes for the recompiler, ~75s per game)'

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed' }

Say 'Compiling -- this is the slow part. Go make tea.'
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { Fail 'Build failed' }

$exe = Get-ChildItem build -Recurse -Filter epoch.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1
if (-not $exe) { Fail 'Build finished but epoch.exe is missing' }
Write-Host "    built: $($exe.FullName)"

# --------------------------------------------------------------------------
# 5. launcher deps
# --------------------------------------------------------------------------
Say 'Installing launcher dependencies'
& $python.Source -m pip install --quiet -r launcher/requirements.txt
if ($LASTEXITCODE -ne 0) {
    Write-Host 'pip failed; try:  python -m venv .venv; .venv\Scripts\activate' -ForegroundColor Yellow
}

Say "Done. Launcher:  python launcher\epoch_launcher.py"
if (-not $NoRun) {
    & $python.Source launcher/epoch_launcher.py --runner $exe.FullName
}
