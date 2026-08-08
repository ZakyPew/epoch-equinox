#!/usr/bin/env bash
# One-command setup for the Oracles compilation.
#
#   ./setup.sh            configure, build, install launcher deps, then run
#   ./setup.sh --no-run   do everything except launch
#
# Safe to re-run: the build is incremental and pip is a no-op once satisfied.
set -euo pipefail

cd "$(dirname "$0")"
RUN_LAUNCHER=1
[[ "${1:-}" == "--no-run" ]] && RUN_LAUNCHER=0

say()  { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }
fail() { printf '\n\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# --------------------------------------------------------------------------
# 1. build tools
# --------------------------------------------------------------------------
say "Checking build tools"
missing=()
for tool in cmake git; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || \
    command -v clang >/dev/null 2>&1 || missing+=("a C compiler")

if ((${#missing[@]})); then
    cat >&2 <<EOF

Missing: ${missing[*]}

  Debian/Ubuntu:  sudo apt-get install -y build-essential cmake ninja-build \\
                      libsdl2-dev libcurl4-openssl-dev python3-pip
  Fedora:         sudo dnf install -y gcc-c++ cmake ninja-build SDL2-devel \\
                      libcurl-devel python3-pip
  macOS:          brew install cmake ninja sdl2 curl
EOF
    exit 1
fi

# SDL2 and libcurl are what the runtime actually links; catch them here rather
# than 40 seconds into a CMake configure.
if command -v pkg-config >/dev/null 2>&1; then
    for lib in sdl2 libcurl; do
        pkg-config --exists "$lib" || fail "$lib development headers not found.
  Debian/Ubuntu:  sudo apt-get install -y libsdl2-dev libcurl4-openssl-dev
  Fedora:         sudo dnf install -y SDL2-devel libcurl-devel
  macOS:          brew install sdl2 curl"
    done
fi

GENERATOR=()
command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)

# --------------------------------------------------------------------------
# 2. build
# --------------------------------------------------------------------------
say "Configuring (this fetches the runtime and Oracle of Seasons -- ~200 MB)"
cmake -S . -B build "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=MinSizeRel

say "Building both carts -- first run compiles ~100 large files, go make tea"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

[[ -x build/oracles ]] || fail "build finished but build/oracles is missing"

# --------------------------------------------------------------------------
# 3. launcher deps
# --------------------------------------------------------------------------
say "Installing launcher dependencies"
PY=$(command -v python3 || command -v python) || fail "python3 not found"
"$PY" -m pip install --quiet -r launcher/requirements.txt || cat >&2 <<'EOF'

pip failed. If this is a system-managed Python (the "externally-managed
environment" error), use a virtualenv:

    python3 -m venv .venv
    . .venv/bin/activate
    pip install -r launcher/requirements.txt
    python launcher/oracles_launcher.py
EOF

# --------------------------------------------------------------------------
# 4. ROMs
# --------------------------------------------------------------------------
mkdir -p build/roms
if [[ ! -f build/roms/tlozooa.gbc && ! -f build/roms/tlozoos.gbc ]]; then
    cat <<'EOF'

  No ROMs found yet. Drop your own dumps in:

      build/roms/tlozooa.gbc     Oracle of Ages   (USA, Australia)
      build/roms/tlozoos.gbc     Oracle of Seasons (USA, Australia)

  Or use "Install ROM" in the launcher. Hashes are verified on first boot.
EOF
fi

say "Done. Launcher:  $PY launcher/oracles_launcher.py"
((RUN_LAUNCHER)) && exec "$PY" launcher/oracles_launcher.py
