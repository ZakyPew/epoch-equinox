# Improving the voxel mod

The voxel renderer is a live interpretation of what the cartridge is drawing.
It does not ship redrawn Oracle assets and it should not guess room geometry
from a stitched map image. The reliable inputs are the current BG tilemap,
tile patterns, palettes, OAM sprites, and the Oracles room/collision state in
WRAM. Preserve those source pixels; change where they live in 3D.

This guide is the shortest path from a fresh clone to a useful voxel PR.

## Build and run

No ROM is needed to compile. A ROM you own is needed to play or capture game
frames, and must never be committed.

```powershell
# Windows PowerShell, from the repository root
powershell -ExecutionPolicy Bypass -File setup.ps1 -NoRun
build\Release\epoch.exe --game tlozooa --voxel 4
```

```sh
# Linux or macOS
./setup.sh --no-run
./build/epoch --game tlozooa --voxel 4
```

Use the exact USA/Australia dumps named in `README.md` for Oracle-specific
work. Test Ages and Seasons when a change touches shared rendering or input.

## The pipeline

| File | Owns |
|---|---|
| `src/voxel/voxel_oracle.c` | Validated WRAM/HRAM reads: room, camera, collision masks, Link, scripts, menus and game-specific addresses |
| `src/voxel/voxel_tiles.c` | PPU/BG/OAM scraping, tile decoding, collision footprint, semantic object detection, sprite grouping and source-art masks |
| `src/voxel/voxel_render.c` | Diorama/chase projection, camera motion and collision, terrain faces, voxel objects, depth buffer, scene/UI compositing |
| `src/voxel/voxel_world.c` | Persistent room cache and neighbouring-room terrain/objects |
| `src/voxel/voxel_overrides.c` | Optional hand-authored room height grids |
| `src/voxel/voxel_menu.cpp` | Runtime controls and tuning UI |
| `src/voxel/voxel_internal.h` | Shared renderer data contracts and testable helpers |
| `patches/gbrt-hires-frame.patch` | Runtime frame hook, scaled frame path and chase-control bridge |

The data flow is:

1. Validate that the current game state describes a stable room.
2. Decode the live tilemap, palettes and OAM exactly as the PPU sees them.
3. Use collision bits for footprint and `wRoomLayout`/state for meaning.
4. Build terrain, fixed voxel objects and grouped sprite objects.
5. Project all of them into one depth buffer.
6. Composite the original status bar, menus and dialogue above the 3D world.

Do not put game-specific WRAM addresses directly into the renderer. Add them
to the appropriate profile in `voxel_oracle.c`, validate them, and expose a
small semantic value through `VoxOracleState` or a helper.

## Decide what an odd thing actually is

Before writing geometry, capture evidence and choose one source class:

- **Terrain:** part of the BG and described by the four collision bits. It
  belongs in the height/elevation grid.
- **BG object:** a stable 16x16 or compound drawing named by `wRoomLayout`.
  Trees, tufts and Impa's house use this path.
- **OAM object:** one or more moving 8x16 sprites. Link, NPCs, held items and
  the opening-scene chest use this path. Group all tiles belonging to one
  object before assigning depth.
- **UI:** window-layer/status/menu/dialog pixels. Keep these screen-space and
  composite them last; do not turn letters or hearts into world geometry.
- **Unstable scene:** room data is changing or a script owns the framing.
  Keep the world voxelled, but use the fixed scene camera until gameplay
  control returns.

Tile numbers are contextual. The same VRAM index can mean different art after
a tileset change, and OAM tile IDs do not mean the same thing as room-layout
IDs. Prefer named game state plus pixel/collision evidence over a global
`tile == number` rule.

## Fast visual loop

`tools/vox_shot.c` boots a ROM headlessly and writes paired flat/voxel PPMs:

```powershell
$env:VOX_SHOT_STATE = "build\Release\interesting.state1"
$env:VOX_DUMP_ROOM = "1"
$env:VOX_DUMP_LAYOUT = "1"
$env:VOX_DUMP_SPRITES = "1"
build\Release\vox_shot.exe roms\tlozooa.gbc 0 4 build\Release\probe
```

Useful switches:

| Variable | Purpose |
|---|---|
| `VOX_SHOT_STATE` | Start from a reproducible savestate; frame numbers begin there |
| `VOX_DUMP_ROOM` | Print game/group/room and the validated collision grid |
| `VOX_DUMP_LAYOUT` | Print the 16x12 room-layout object IDs |
| `VOX_DUMP_SPRITES` | Print decoded/grouped OAM entries |
| `VOX_DUMP_TREES` | Write the recovered 16x16 vegetation art and mask sheet |
| `VOX_DUMP_HGT` | Print the final height/elevation classification |
| `VOX_SHOT_NOCACHE` | Render without remembered neighbouring rooms |
| `VOX_DUMP_WORLD` | Log persistent-world cache hits and misses at room borders |
| `VOX_SHOT_TURN` | Apply one chase-camera turn impulse for an alternate angle |

Keep a flat screenshot beside every voxel screenshot. Test the object from at
least front, side and rear chase angles, then cross a room boundary and return.
A fix that only works from the capture angle is not done.

For a useful issue or handoff, include:

- game, group-room readout and approximate frame;
- save or savestate when legally shareable;
- flat and voxel screenshots at the same frame;
- voxel mode, camera yaw and any tuning overrides;
- relevant `VOX_DUMP_*` output; and
- expected shape versus the observed failure.

Never attach a ROM, extracted tilesheet, or other ROM-derived asset.

## Building a new 3D object

1. Prove whether it comes from BG layout or OAM.
2. Group the complete authored drawing. Do not depth-sort its 8x16 pieces
   independently.
3. Choose a single world-space footpoint and collision/elevation base.
4. Recover transparency from the surrounding pixels without deleting enclosed
   dark outlines.
5. Put the original pixels on the important face or top. Exposed side faces
   may fold/repeat edge pixels and apply modest directional shading, but should
   not introduce a replacement texture or palette.
6. Remove or replace the object's old overhead copy so it does not remain
   painted onto the ground.
7. Draw through the shared depth buffer so Link naturally passes in front of
   and behind the whole object.
8. Add a focused helper/test, then capture multiple camera angles in both a
   cold room and a remembered room.

For the chest task specifically, do not key only on layout `$F0`/`$F1`. The
opening cutscene uses an OAM pair (`$60` and `$62` in the current Ages capture),
so the correct implementation needs a semantic OAM group with one footpoint,
a shallow body, and a separate lid. Confirm those IDs against additional
open/closed chests before treating them as universal.

## Cliff and elevation rules

- Oracle collision `$01`-`$0F` is a four-bit quadrant occupancy mask, not a
  height value or a count of visible rock lines.
- The collision mask gives the X/Y footprint. Tile art proposes a low/mid/high
  visual class; a connected architectural mass votes once.
- Enclosed walkable ground inherits the class of its architectural cliff lip,
  so the shelf top and edge meet exactly.
- Vegetation can block movement but must not create a raised plateau.
- A cliff face repeats complete native 8px source bands down a planar edge.
  Do not stretch one scanline or paint generic rock.

Use `VOXEL_EDIT=1` only for genuinely exceptional rooms. A broad semantic fix
belongs in classification; an override should not hide a systemic bug.

## Required checks

Before opening a PR, run the same small checks CI uses:

```sh
cmake --build build --config Release --parallel
python tools/updater_test.py
python tools/secrets_test.py
```

Build and run the C checks from `tools/` against `epoch_support` and `gbrt`:

- `chasecam_test` for chase heading, controls, scripted-camera selection and
  camera collision;
- `cliff_test` for collision quadrants, connected heights and plateaus;
- `vox_world_test` for persistent terrain and voxel objects;
- `achievements_test`, `secrets_c_test`, `typist_test`, and `stream_test` for
  shared-player regressions.

The exact Linux and macOS compile loops live in `.github/workflows/release.yml`; Windows
contributors can use the generated Visual Studio/vcpkg include and library
paths or let the PR run the portable matrix. Always launch the built player
and confirm `--list-games` before calling the build healthy.

## Collaboration rules

- Start from current `main` and use a short feature branch.
- Keep one visual problem per PR when possible. Include before/after captures
  and the room identifier in the PR body.
- Preserve changes already in the branch; do not rewrite or reset another
  contributor's work.
- Add comments for evidence and invariants, not a diary of experiments.
- Never commit `roms/`, saves containing private play data, build output, or
  ROM-derived images.
- If classification is uncertain, submit the capture/dump first. A small
  evidence PR is more useful than a large table of guessed tile IDs.

Good first tasks are the OAM-aware chest reconstruction, large dungeon room
coordinates, more compound structures, and scripted screenshot routes for
Seasons. Each exercises a different part of the pipeline without requiring a
new renderer.
