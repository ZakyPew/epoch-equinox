<h1 align="center">Epoch &amp; Equinox</h1>

<p align="center">
  <b>The Legend of Zelda: Oracle of Ages</b> and <b>Oracle of Seasons</b>,
  statically recompiled to native C — with a modern launcher, mod support,
  and an optional 3D diorama mode.
</p>

<p align="center">
  <img src="docs/launcher.png" alt="The Epoch &amp; Equinox launcher" width="820">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/license-MIT-informational?style=for-the-badge" alt="MIT">
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey?style=for-the-badge" alt="Platforms">
  <img src="https://img.shields.io/badge/ROM-not%20included-critical?style=for-the-badge" alt="No ROM included">
</p>

---

> [!IMPORTANT]
> **This project contains no game code.** The C that runs the games is
> generated on your machine, from a ROM you already own, when you build it.
> Nothing derived from the games enters this repository, and there are no
> binary downloads — the same approach
> [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and
> [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) take.

Both games run from one binary at full native speed, with savestates, shader
presets, controller remapping, SGB borders, IPS/BPS mod loading — and a voxel
mode that rebuilds the overworld as a tilted diorama out of the Game Boy's own
tilemap.

## Quick start

You need your own dumps. Put them in `roms/`, named by game id:

| game | file | SHA-1 |
|---|---|---|
| Oracle of Ages (USA/Australia) | `roms/tlozooa.gbc` | `880374fb978b18af4aa529e2e32f7ffb4d7dd2f4` |
| Oracle of Seasons (USA/Australia) | `roms/tlozoos.gbc` | `ba1268290fb2b1b70505d2d7b5825fc8a4816a4b` |

Then:

```sh
git clone https://github.com/ZakyPew/epoch-equinox.git
cd epoch-equinox
./setup.sh
```

`setup.sh` checks your toolchain, builds the recompiler, turns your ROMs into
C, compiles both games, installs the launcher's Python dependencies and opens
it. Re-running is cheap — recompilation is cached against the ROM's hash.

Either game on its own is fine; you'll just get that one. Any other Game Boy
Color ROM in `roms/` builds too.

Hashes are verified before anything is recompiled. A different revision is
refused rather than quietly producing a broken build.

<details>
<summary><b>What the first build actually does</b> (and why it takes a while)</summary>

1. fetches [gb-recompiled](https://github.com/GB-Recomp/gb-recompiled) (MIT)
   and builds `gbrecomp`, the recompiler — about 2 minutes
2. runs it over each ROM, emitting ~48 C files per game — about 75 seconds each
3. compiles all of it — the slow part

Budget 20–30 minutes cold on four cores. After that, builds are incremental.
Step 2 is why there are no prebuilt downloads: the output is translated from
your ROM, so it can only legitimately be made by you, on your machine.

</details>

## Controls

Defaults. All of it is rebindable in the in-game menu, which also has per-brand
button labels for Xbox, PlayStation, Switch Pro and Joy-Con.

| Action | Keyboard | Controller |
|---|---|---|
| D-pad | Arrow keys / `WASD` | D-pad or left stick |
| A | `Z` / `J` | B *(right face button)* |
| B | `X` / `K` | A *(bottom face button)* |
| Select | `Backspace` / `Right Shift` | Back |
| Start | `Enter` | Start |
| Fast forward *(hold)* | `Tab` | Right trigger |
| Max speed *(toggle)* | `` ` `` | Left trigger |

> Face buttons follow the Nintendo layout, so on an Xbox pad the Game Boy's A
> is the physically right button and B is the bottom one — the same positions
> as the original hardware.

## Hotkeys

| Key | Action | Controller |
|---|---|---|
| `Esc` / `F10` | Open the runtime menu | L3 / R3 |
| `F5` | Save state | X |
| `F8` | Load state | Y |
| `F6` / `F7` | Previous / next savestate slot | — |
| `F1` | Toggle FPS overlay | — |
| `M` | Mute | — |
| **`F3`** | **Cycle voxel mode** (off → 15° → 30° → 45°) | — |

The runtime menu covers savestates, palettes, shader presets, SGB borders,
hardware mode, audio and input remapping.

## Launch options

The launcher handles this for you, but the binary stands alone:

| option | effect |
|---|---|
| `--game <id>` | run a cart directly (`tlozooa`, `tlozoos`) |
| `--list-games` | print the ids in this build |
| `--no-mods` | boot the stock ROM, ignoring `mods/` |
| `--voxel <n>` | start with the diorama on (`0`–`3`) |
| `--games-json` | machine-readable game table (what the launcher reads) |

```sh
./build/epoch --game tlozooa --voxel 2
```

## Voxel mode

**Off by default — press `F3`.** The game plays exactly as it always did until
you turn it on.

The overworld is re-rendered as a tilted 3D diorama, rebuilt every frame from
PPU state. A static recompilation has no scene graph, entity list or collision
data, so this reads the only honest source there is: the BG tilemap, CGB
palettes and OAM sitting in emulated VRAM.

- tiles are classified per 8×8 into terrain heights — water sinks, paths lie
  flat, bushes and rocks rise, trees and walls rise highest
- each screen column is marched far-to-near, projecting cell tops and filling
  the exposed front wall where height steps down
- terrain is textured with the game's *own* composed frame, so palettes,
  season tints and tile animation carry through untouched
- sprites are re-decoded from VRAM and stood upright as billboards
- the status bar stays flat and composites back on top

Output is a normal 160×144 frame handed back through the runtime's present
path, so shaders, scaling and screenshots all still apply.

<details>
<summary><b>Honest limits</b></summary>

- Heights come from what tiles *look like*, not real collision data, so it's a
  plausible relief rather than a correct one. Thresholds are tunable at the top
  of [`voxel_tiles.c`](src/voxel/voxel_tiles.c).
- Fixed pitch ladder. No free-roam or first-person camera — moving the player
  off the grid needs the engine's own collision, which a recompilation doesn't
  expose.
- Native 160×144, so it's chunky by construction. Deliberate.
- Menus, cinematics and interiors get extruded too, since the classifier only
  sees tiles. `F3` back to off for those.
- **No widescreen, and that's measured not guessed.** `VOXEL_DUMP_MAP=<frame>`
  writes the whole 32×32 BG map; mid-gameplay it holds about one screen of real
  tiles and flat filler everywhere else. Oracles only maintains the columns
  it's about to scroll into, so there's no off-screen world to reveal.
- **60 FPS is already the case.** The runtime paces at the Game Boy's native
  59.7 FPS and Oracles runs its logic every frame — there's no 20/30→60 unlock
  to do like on N64 recomps.

`VOXEL_DEBUG=1` prints per-frame classification stats.

</details>

## Mods

Mods are applied to the extracted ROM *before* the cart boots, which is why the
toggles live in the launcher. A pristine snapshot is kept and the live ROM
rebuilt from it every launch, so turning a mod off genuinely undoes it and two
runs with the same mod set are byte-identical.

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

**IPS and BPS both work** — that's what Oracles randomizers and romhacks
already ship as, so most existing patches drop straight in. Unknown manifest
keys are ignored, so a manifest written for a newer loader still loads.

Worked example and the full field reference: [`examples/mods/`](examples/mods).

> [!NOTE]
> A patch that changes the ROM's **size** can't work here — the generated C is
> bound to the original bank layout, so the loader reports it and skips the mod
> rather than booting something broken. BPS patches also carry a source
> checksum; one built for a different revision is rejected with a message
> instead of silently producing garbage.

## Cover art

Each launcher panel draws a procedural motif unless you supply art.

| where | for | committed? |
|---|---|---|
| `build/covers/<id>.png` | your own art, this machine only | no |
| `art/covers/<id>.png` | art shipped with the project | yes |

```sh
python3 tools/install_cover_art.py --ages ages.png --seasons seasons.png
```

That names, centre-crops and resizes both correctly. **1600 × 1600 square** is
ideal; keep the subject in the middle 70%, since the diagonal seam cuts the
inner edge and the title sits over the outer one. Add `--local` for art that
shouldn't be redistributed. Details: [`art/covers/README.md`](art/covers/README.md).

## Building by hand

```sh
sudo apt-get install -y build-essential cmake ninja-build \
    libsdl2-dev libcurl4-openssl-dev python3-pip     # Debian/Ubuntu
# brew install cmake ninja sdl2 curl                 # macOS

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build -j$(nproc)

pip install -r launcher/requirements.txt
python3 launcher/epoch_launcher.py
```

| CMake option | default | effect |
|---|---|---|
| `EPOCH_ROM_DIR` | `./roms` | where your ROMs live |
| `EPOCH_WITH_VOXEL` | `ON` | build the voxel renderer |
| `EPOCH_JOBS` | auto | parallelism for recompilation |
| `GBRECOMP_SHRINK` | `ON` | dead-strip + symbol-strip |
| `GBRT_REF` | `main` | pin a git ref for the fetched runtime |

**Nicer symbol names (optional):** drop the `.sym` files from
[Stewmath/oracles-disasm](https://github.com/Stewmath/oracles-disasm) beside
your ROMs as `roms/tlozooa.sym` / `roms/tlozoos.sym` and generated functions
carry real names like `gfxRegisterStates` instead of addresses.

<details>
<summary><b>Troubleshooting</b></summary>

**"The games haven't been built yet"** — you ran the launcher before building.
`./setup.sh`, or the two `cmake` commands above.

**pip refuses: "externally managed environment"** — use a virtualenv:

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install -r launcher/requirements.txt
python launcher/epoch_launcher.py
```

**"No ROMs found in .../roms"** — expected. Nothing can be built without one;
see [Quick start](#quick-start).

**SHA-1 mismatch** — your dump isn't the USA/Australia revision the hashes
above describe. Other revisions aren't supported yet.

**No controller in the launcher** — `pip install pygame`. Optional; the game
itself handles pads either way.

</details>

## Contributing

Issues and pull requests welcome. Useful things to know:

- `src/` and `launcher/` are the project's own code — that's where changes go
- generated cart C lives in `build/generated/` and is never committed
- CI builds our C against a synthetic cart, imports the launcher headlessly,
  and checks the runtime patch still applies upstream

Good first areas: tuning the voxel height classifier against
`oracles-disasm`'s real collision tables, per-section asset splitting for
finer-grained mods, and Windows build support.

**Never attach a ROM, savefile, or ROM-derived asset to an issue or PR.**

## Credits

Built on [GB-Recomp/gb-recompiled](https://github.com/GB-Recomp/gb-recompiled)
(MIT) — the static recompiler and runtime that make all of this possible, and
itself a fork of [arcanite24/gb-recompiled](https://github.com/arcanite24/gb-recompiled).

Symbol names come from [Stewmath/oracles-disasm](https://github.com/Stewmath/oracles-disasm),
the community disassembly.

Full attribution, including what this project started from and what is original
to it, is in [CREDITS.md](CREDITS.md).

## Legal

No ROM data is included, distributed, or produced by CI. The Legend of Zelda:
Oracle of Ages and Oracle of Seasons are © Nintendo / Capcom. Supply your own
dumps of games you own.

This project's own code is [MIT](LICENSE). Game Boy and Game Boy Color are
trademarks of Nintendo. This project is not affiliated with, endorsed by, or
associated with Nintendo or Capcom.
