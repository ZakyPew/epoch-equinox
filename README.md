# Epoch & Equinox

*Oracle of Ages and Oracle of Seasons, recompiled from your own ROMs.*

**The Legend of Zelda: Oracle of Ages** and **Oracle of Seasons**, statically
recompiled to native C and wrapped in a modern launcher, with mod support and
an optional 3D diorama renderer.

Both games in one binary. Savestates, shaders, controller remapping, IPS/BPS
mod loading, and a voxel mode that rebuilds the overworld as a tilted diorama
from the PPU's own tilemap.

> **This project ships no game code.** The C that runs the games is generated
> on your machine, from your own ROM, at build time — the same model
> [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and
> [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) use.
> Nothing derived from the ROMs enters this repository or any release.

## Quick start

You need your own dumps of both games. Put them in `roms/`, named by game id:

```
roms/tlozooa.gbc     The Legend of Zelda: Oracle of Ages   (USA, Australia)
roms/tlozoos.gbc     The Legend of Zelda: Oracle of Seasons (USA, Australia)
```

Then:

```sh
git clone https://github.com/ZakyPew/epoch-equinox.git
cd epoch-equinox
./setup.sh
```

`setup.sh` checks your toolchain, builds the recompiler, turns your ROMs into
C, compiles both games, installs the launcher's Python dependencies and opens
it. Re-running is cheap — recompilation is cached against the ROM.

Either game alone works fine; you'll just get the one.

### What the first build does

1. fetches [gb-recompiled](https://github.com/GB-Recomp/gb-recompiled) (MIT)
   and builds `gbrecomp`, the recompiler — about 2 minutes
2. runs it over each ROM, emitting ~48 C files per game — about 75 seconds each
3. compiles all of it, which is the slow part

Budget 20–30 minutes cold on four cores. After that, builds are incremental.

### Doing it by hand

```sh
sudo apt-get install -y build-essential cmake ninja-build \
    libsdl2-dev libcurl4-openssl-dev python3-pip     # Debian/Ubuntu
# brew install cmake ninja sdl2 curl                 # macOS

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build -j$(nproc)

pip install -r launcher/requirements.txt
python3 launcher/epoch_launcher.py
```

If your Python is "externally managed" and pip refuses:

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install -r launcher/requirements.txt
python launcher/epoch_launcher.py
```

### CMake options

| option | default | effect |
|---|---|---|
| `EPOCH_ROM_DIR` | `./roms` | where your ROMs live |
| `EPOCH_WITH_VOXEL` | `ON` | build the voxel diorama renderer |
| `EPOCH_JOBS` | auto | parallelism for recompilation |
| `GBRECOMP_SHRINK` | `ON` | dead-strip + symbol-strip the binary |
| `GBRT_REF` | `main` | pin a git ref for the fetched runtime |

### Better symbol names

Optional: drop the `.sym` files from
[Stewmath/oracles-disasm](https://github.com/Stewmath/oracles-disasm) next to
your ROMs as `roms/tlozooa.sym` / `roms/tlozoos.sym`. Generated functions then
carry real names like `gfxRegisterStates` instead of addresses. Cosmetic, but
it makes the output far easier to read.

## Any GBC ROM works

Nothing about the cart list is hardcoded. Every ROM in `roms/` gets recompiled
and added to the launcher — the two Oracles ids just get proper titles and
theming. Drop another Game Boy Color game in and it builds too, though the
voxel classifier is tuned for top-down overworlds.

## Run

`./build/epoch --game tlozooa` runs a cart directly, `--list-games` prints
the ids, `--no-mods` boots stock. Esc opens the runtime menu in game
(savestates, palette, shaders, borders, input remapping, audio).

### Controllers

In game, the runtime binds SDL_GameController directly — remapping lives in
the Esc menu, with per-brand button labels for Xbox, PlayStation, Switch Pro
and Joy-Con.

The launcher takes a pad too if `pygame` is installed: d-pad or left stick to
move, A/Start to confirm, B to quit. Without pygame it falls back to keyboard
and mouse.

### Cover art

Each panel draws a procedural motif by default. For your own art, drop PNGs
next to the game binary:

```
build/covers/tlozooa.png
build/covers/tlozoos.png
```

**Recommended: 1600 × 1600 (square). Minimum 1000 × 1000.** Each game gets a
diagonal slice of the window at roughly 1.1:1, so square art crops least; keep
the subject inside the middle 70%, since the seam cuts the inner edge and the
title sits over the outer one. Full spec in
[`examples/covers/README.md`](examples/covers/README.md).

`covers/` is gitignored — scans and key art aren't ours to redistribute.

## Mods

Mods are applied to `assets/<id>/rom.bin` before the cart boots, which is why
the toggles live in the launcher. A pristine `rom.bin.orig` snapshot is kept
and the live ROM rebuilt from it every launch, so turning a mod off genuinely
undoes it and two runs with the same mod set are byte-identical.

One directory per mod under `mods/`:

```
mods/my-randomizer/
  manifest.json      required
  seed.bps           patch named by the manifest
  overlay/           optional raw byte splices
```

```json
{
  "id":       "my-randomizer",
  "name":     "Seasons Randomizer",
  "version":  "1.0.0",
  "games":    ["tlozoos"],
  "patch":    "seed.bps",
  "priority": 100,
  "enabled":  true
}
```

IPS and BPS both work — that's what Oracles randomizers and romhacks already
ship as. Unknown manifest keys are ignored, so a manifest for a newer loader
still loads. Full spec and a worked example in
[`examples/mods/`](examples/mods).

**What won't work:** a patch that changes the ROM *size*. The generated C is
bound to the original bank layout, so the loader reports it and skips the mod.
BPS patches also carry a source checksum; one built for a different revision
is rejected rather than silently producing garbage.

## Voxel mode

**Off by default.** The game plays exactly as it always did unless you turn it
on. Press **F3** to cycle `OFF → 15 → 30 → 45` (camera pitch), or start on a
rung with `--voxel 2`.

The overworld is re-rendered as a tilted 3D diorama built from PPU state every
frame. A static recompilation has no scene graph, entity list or collision
data, so this reads the only honest source there is — the BG tilemap, CGB
palettes and OAM in emulated VRAM:

- tiles are classified per 8×8 into terrain heights (water sinks, paths lie
  flat, bushes and rocks rise, trees and walls rise highest)
- each screen column is marched far-to-near, projecting cell tops and filling
  the exposed front wall where height steps down — painter's order per column,
  no depth buffer
- terrain is textured with the game's own composed frame, so palettes, season
  tints and tile animation carry through untouched
- sprites are re-decoded from VRAM and stood upright as billboards with a
  contact shadow
- the status bar stays flat and is composited back on top

Output is a normal 160×144 frame handed back through the runtime's present
path, so shaders, scaling and screenshots all still apply.

### How it hooks in

Generated cart code calls `gb_platform_render_frame()` directly and can't be
edited, so [`patches/gbrt-frame-hook.patch`](patches) adds a ~27-line hook to
the fetched runtime at configure time, letting a host substitute its own
rendering. Small and upstreamable.

### Honest limits

- Terrain height comes from what tiles *look like*, not from real collision
  data, so it's a plausible relief rather than a correct one. Thresholds are
  tunable at the top of [`voxel_tiles.c`](src/voxel/voxel_tiles.c).
- Fixed pitch ladder. No free-roam or first-person camera — moving the player
  off the grid needs the engine's own collision, which a recompilation doesn't
  expose.
- Native 160×144, so it's chunky by construction. Deliberate.
- Menus, cinematics and interiors get extruded too, since the classifier only
  sees tiles. F3 back to OFF for those.

### No widescreen

Not possible here, and there's a measurement behind that rather than a guess.
`VOXEL_DUMP_MAP=<frame>` writes the whole 32×32 BG map; dumped mid-gameplay it
holds about one screen of real tiles and flat filler everywhere else. Oracles
only maintains the columns it's about to scroll into, so there is no
off-screen world to reveal. Widening the view would show filler. Real
widescreen would mean rewriting the cart's own map-drawing routines.

### 60 FPS

Already there. The runtime paces at the Game Boy's native 59.7 FPS (70224
cycles per frame) and Oracles runs its logic every frame — there's no 30→60
unlock to do like there is on N64 recomps, where games shipped at 20 or 30.

## Layout

```
cmake/GenerateCarts.cmake       build-time recompilation from your own ROM
runner_main.cpp                 game runner: --game / --games-json / --voxel
src/mod_loader.{h,c}            manifest parsing, IPS/BPS, overlays
src/voxel/                      voxel diorama renderer
launcher/epoch_launcher.py    the launcher app
launcher/gamepad.py             optional pad navigation
patches/                        frame-hook patch for the fetched runtime
tools/make_test_patches.py      generates IPS/BPS patches to test the loader
examples/                       mod manifest + cover art specs
setup.sh                        one-command build and launch
```

## Legal

No ROM data is included, distributed, or produced by CI. The games are
© Nintendo / Capcom; supply your own dumps of games you own. Hashes are
verified before anything is recompiled.

See [CREDITS.md](CREDITS.md) for full attribution and [LICENSE](LICENSE) for
terms. This project's own code is MIT.
