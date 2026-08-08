# Oracles

**The Legend of Zelda: Oracle of Ages** and **Oracle of Seasons**, statically
recompiled into portable C and built into a single binary on the
[GB-Recomp/gb-recompiled](https://github.com/GB-Recomp/gb-recompiled) runtime.

Ages' generated sources live in this repo; Seasons is pulled in from
[GB-Recomp/tlozoos](https://github.com/GB-Recomp/tlozoos) at configure time.
Symbol names follow the [Stewmath/oracles-disasm](https://github.com/Stewmath/oracles-disasm)
WLA-DX disassembly, so labels like `tlozooa__sym_gfxRegisterStates` map back to
the same names you'd see in the decomp.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build -j$(nproc)
```

Needs CMake 3.16+, a C/C++ compiler, SDL2, libcurl and OpenGL ES 2. CMake
fetches the runtime and the Seasons cart itself — no submodules. On a
Debian/Ubuntu box:

```sh
sudo apt-get install -y build-essential cmake ninja-build libsdl2-dev libcurl4-openssl-dev
```

The first configure clones ~200 MB of generated Seasons sources, and a cold
build compiles ~100 large translation units. Produces `build/oracles`.

### CMake options

| option | default | effect |
|---|---|---|
| `ORACLES_WITH_SEASONS` | `ON` | fetch and link Oracle of Seasons |
| `ORACLES_WITH_VOXEL` | `ON` | build the voxel diorama renderer |
| `GBRECOMP_SHRINK` | `ON` | dead-strip + symbol-strip the binary |
| `TLOZOOS_REF` / `GBRT_REF` | `main` | pin a git ref for the fetched repos |

## Required ROMs

Both carts are locked to one revision each. Drop your own dumps next to the
executable:

```sh
mkdir -p build/roms
cp 'Legend of Zelda, The - Oracle of Ages (USA, Australia).gbc'    build/roms/tlozooa.gbc
cp 'Legend of Zelda, The - Oracle of Seasons (USA, Australia).gbc' build/roms/tlozoos.gbc
./build/oracles
```

| game | id | SHA-1 |
|---|---|---|
| Oracle of Ages (USA/Australia) | `tlozooa` | `880374fb978b18af4aa529e2e32f7ffb4d7dd2f4` |
| Oracle of Seasons (USA/Australia) | `tlozoos` | `ba1268290fb2b1b70505d2d7b5825fc8a4816a4b` |

The loader verifies the hash on first launch and refuses to extract any other
dump. After that first boot the ROM is decompressed into `assets/<id>/` and the
source `.gbc` is no longer needed.

No ROM data is committed to this repo, and none ever should be.

## Run

```sh
pip install PySide6-Essentials pygame     # pygame is optional, for gamepads
python3 launcher/oracles_launcher.py
```

The launcher is a **separate app**, the way Zelda64Recomp and Ship of Harkinian
split theirs: the C binary only runs carts, and everything else — game
selection, mods, ROM install — lives in Python. The runner links ~170 MB of
generated C and takes minutes to rebuild; the launcher is the part that
actually changes, so it lives where a restart costs nothing. A crash in the
cart also can't take the launcher down with it.

There is no duplicated game table: the launcher asks the runner for one with
`oracles --games-json`.

You can skip the launcher entirely — `./build/oracles --game tlozooa` runs a
cart directly, `--list-games` prints the ids, `--no-mods` boots stock.

Esc opens the runtime menu in game (savestates, palette, shaders, borders,
input remapping, audio).

### Controllers

In game, the runtime binds SDL_GameController directly — remapping lives in the
Esc menu, with per-brand button labels for Xbox, PlayStation, Switch Pro and
Joy-Con.

The launcher menus take a pad too, if `pygame` is installed: d-pad or left
stick to move, A/Start to confirm, B to quit. Without pygame it silently falls
back to keyboard and mouse — nothing about it is required.

### Cover art

Each panel draws a procedural motif themed per cart. To use real art instead,
drop `covers/tlozooa.png` and/or `covers/tlozoos.png` next to the binary.
Keep those local; they don't belong in the repo.

## Mods

Mods are applied to `assets/<id>/rom.bin` *before* the cart boots, which is why
the toggles live in the launcher rather than the in-game Esc menu — the ROM has
to be final by the time the cart reads it back.

The launcher keeps a pristine `rom.bin.orig` snapshot and rebuilds the live ROM
from it on every launch, so turning a mod off genuinely undoes it and two runs
with the same mod set are byte-identical.

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
  "overlay":  "overlay",
  "priority": 100,
  "enabled":  true
}
```

- **`games`** — which carts it applies to. Omit the key to apply to both.
- **`patch`** — an `.ips` or `.bps` file. This is the format Oracles
  randomizers and romhacks already ship in, so most existing patches drop
  straight in.
- **`overlay`** — a directory of raw splices named for their hex ROM offset
  (`overlay/03f200.bin` writes those bytes at `0x3F200`). Handy for a
  hand-edited tileset without generating a whole patch.
- **`priority`** — lower numbers apply first. Unknown keys are ignored, so a
  manifest aimed at a newer loader still loads.

A working example lives in [`examples/mods/`](examples/mods).

### What won't work

A patch that **changes the ROM size** can't run here. The generated C is bound
to the original bank layout, so a resized image has nowhere to go — the loader
reports it and skips that mod rather than booting something broken. BPS patches
also carry a source checksum; one built against a different revision is
rejected with a message saying so, instead of silently producing garbage.

## Voxel mode

See [`src/voxel/`](src/voxel). The overworld is re-rendered as a 3D diorama
built from the PPU's own tilemap each frame. This is original code — it is not
a port of any existing mod, and it works off emulated VRAM rather than an
engine-level scene graph, because a static recompilation doesn't have one.

## Layout

```
tlozooa_*.c                     generated Oracle of Ages sources (~170 MB)
runner_main.cpp                 game runner: --game / --games-json / --no-mods
src/mod_loader.{h,c}            manifest parsing, IPS/BPS, overlays
src/voxel/                      voxel diorama renderer
launcher/oracles_launcher.py    the launcher app
launcher/gamepad.py             optional pad navigation for the launcher
tools/make_test_patches.py      generates IPS/BPS patches to test the loader
examples/mods/                  sample mod manifest
```

## Credits

- [GB-Recomp/gb-recompiled](https://github.com/GB-Recomp/gb-recompiled) — the
  static recompiler and runtime everything here is built on.
- [Stewmath/oracles-disasm](https://github.com/Stewmath/oracles-disasm) — the
  disassembly the symbol names come from.
