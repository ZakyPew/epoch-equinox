# One-command setup for Epoch & Equinox on Windows.
#
#   powershell -ExecutionPolicy Bypass -File setup.ps1
#   powershell -ExecutionPolicy Bypass -File setup.ps1 -NoRun
#
# Written for Windows PowerShell 5.1 -- the one every Windows ships with --
# so no PowerShell 7 syntax anywhere in this file. It previously used the
# `??` operator, which 5.1 cannot even parse: the script died before its
# first line, with no message. Parse errors are the one failure this file
# cannot catch for you, which is why staying 5.1-clean matters.
#
# Safe to re-run: the build is incremental and pip is a no-op once satisfied.
#
# Needs Visual Studio 2019+ with the "Desktop development with C++" workload
# (the free Community edition is fine), CMake, Python 3, and vcpkg for SDL2
# and libcurl. It tells you which of those are missing rather than failing
# halfway through a build.

[CmdletBinding()]
param([switch]$NoRun)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Say  { param($m) Write-Host "`n==> $m" -ForegroundColor Cyan }

# Pause before dying when the window would vanish (double-click), so the
# error is actually readable.
function Fail {
    param($m)
    Write-Host "`nERROR: $m" -ForegroundColor Red
    if ($Host.Name -eq 'ConsoleHost' -and -not $env:CI) {
        Read-Host 'Press Enter to close'
    }
    exit 1
}

try {

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

# Find a real Python. Two traps here: 5.1 has no ?? operator, and a bare
# Windows has a fake python.exe (the Microsoft Store alias under
# WindowsApps) that Get-Command happily returns but that only opens the
# Store when run.
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if ($python -and $python.Source -like '*WindowsApps*') {
    $python = $null
}
if (-not $python) {
    Fail @'
Python 3 not found (or only the Microsoft Store stub is on PATH).

    winget install Python.Python.3.12

Then open a new terminal. If it still fails, disable the Store alias:
Settings > Apps > Advanced app settings > App execution aliases >
turn off "python.exe".
'@
}
Write-Host "    Python: $($python.Source)"

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
    if ($LASTEXITCODE -ne 0) { Fail 'vcpkg could not install SDL2/libcurl' }
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
# 3. build -- fast, and needs no ROM at all
# --------------------------------------------------------------------------
Say 'Configuring'
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed (scroll up for the first error)' }

Say 'Compiling (about a minute)'
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { Fail 'Build failed (scroll up for the first error)' }

$exe = Get-ChildItem -Path build -Recurse -Filter epoch.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1
if (-not $exe) { Fail 'Build finished but epoch.exe is missing' }
Write-Host "    built: $($exe.FullName)"

# --------------------------------------------------------------------------
# 4. ROMs -- needed to PLAY, not to build
# --------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path roms | Out-Null
$romExts = @('.gb', '.gbc', '.sgb')
$roms = @(Get-ChildItem -Path roms -File -ErrorAction SilentlyContinue |
          Where-Object { $romExts -contains $_.Extension.ToLower() })
if ($roms.Count -eq 0) {
    Write-Host @'

  Built. To play, drop your ROMs into roms\:

      roms\tlozooa.gbc     Oracle of Ages   (USA, Australia)
      roms\tlozoos.gbc     Oracle of Seasons (USA, Australia)

  Any other .gb/.gbc works too. Then re-run this script or start the
  launcher directly.

'@ -ForegroundColor Yellow
} else {
    $roms | ForEach-Object { Write-Host "    ROMs: $($_.Name)" }
}

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

} catch {
    # Anything unexpected: show it and hold the window open instead of
    # flashing and closing.
    Write-Host "`nUNEXPECTED ERROR:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host $_.ScriptStackTrace
    if ($Host.Name -eq 'ConsoleHost' -and -not $env:CI) {
        Read-Host 'Press Enter to close'
    }
    exit 1
}
