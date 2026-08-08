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
# 2. ROMs -- required before anything can be built
# --------------------------------------------------------------------------
say "Checking for ROMs"
mkdir -p roms
shopt -s nullglob
found=(roms/*.gb roms/*.gbc roms/*.sgb)
shopt -u nullglob

if ((${#found[@]} == 0)); then
    cat >&2 <<'EOF'

  No ROMs in roms/.

  This project ships no game code -- it recompiles the games from your own
  dumps, so a ROM has to be there before anything can be built. Copy yours
  in, named by game id:

      roms/tlozooa.gbc     The Legend of Zelda: Oracle of Ages   (USA, Australia)
      roms/tlozoos.gbc     The Legend of Zelda: Oracle of Seasons (USA, Australia)

  Either one alone is fine -- you'll just get that game.

  Optional: drop the matching .sym files from Stewmath/oracles-disasm beside
  them (roms/tlozooa.sym) and generated functions get real names.

EOF
    exit 1
fi
printf '    found: %s\n' "${found[@]}"

# --------------------------------------------------------------------------
# 3. build
# --------------------------------------------------------------------------
say "Configuring -- builds the recompiler, then turns your ROMs into C"
echo "    (first run: ~2 min for the recompiler, ~75s per game)"
cmake -S . -B build "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=MinSizeRel

say "Compiling -- this is the slow part, 20-30 min cold. Go make tea."
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

[[ -x build/epoch ]] || fail "build finished but build/epoch is missing"

# --------------------------------------------------------------------------
# 4. launcher deps
# --------------------------------------------------------------------------
say "Installing launcher dependencies"
PY=$(command -v python3 || command -v python) || fail "python3 not found"
"$PY" -m pip install --quiet -r launcher/requirements.txt || cat >&2 <<'EOF'

pip failed. If this is a system-managed Python (the "externally-managed
environment" error), use a virtualenv:

    python3 -m venv .venv
    . .venv/bin/activate
    pip install -r launcher/requirements.txt
    python launcher/epoch_launcher.py
EOF

say "Done. Launcher:  $PY launcher/epoch_launcher.py"
((RUN_LAUNCHER)) && exec "$PY" launcher/epoch_launcher.py
