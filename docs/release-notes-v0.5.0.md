# Epoch & Equinox v0.5.0

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. Windows users can run the launcher exe; on
Linux run `epoch`, or `launcher/epoch_launcher.py` for the UI (needs Python +
PySide6).

This release is the largest voxel-mode fidelity pass so far. The chase camera
now treats the cartridge's tiles as pieces of a 3D world instead of replacing
recognizable objects with generic shapes.

## Tile-authored trees and structures

- Full trees are fixed world geometry: a lower trunk and an upper hard canopy
  built from the live 16x16 source cell. They are no longer camera-facing
  billboards or rounded generated foliage.
- Connected canopy variants join without internal side walls, while exposed
  sides fold the matching edge of the original tile art.
- Bushes and grass tufts become shallow masked reliefs, leaving real ground
  beneath them.
- Impa's tree house is recognized as one compound facade and canopy instead of
  six unrelated raised cells.
- Terrain, structures, vegetation, Link and NPCs share depth, fixing sprites
  that appeared on top of Link merely because their OAM pieces were sorted
  separately.

## Correct cliffs and plateaus

- Oracle collision values `$01`-`$0F` are decoded as four quadrant bits, so
  half walls and corners keep their real footprint.
- Connected cliff masses choose one consistent low/mid/high class.
- Enclosed walkable ground rises to the exact level of its cliff lip. Objects
  on a shelf inherit that base elevation instead of floating above flat land.
- Chase view projects straight world-space cliff faces and textures them with
  complete native 8px bands from the raised tile, avoiding stretched lines
  and bent per-column edges.

## Chase camera, controls and scenes

- A fresh chase camera starts directly behind Link, trails his movement after
  manual orbit input settles, and keeps the right-stick camera direction
  unchanged.
- Keyboard and controller movement are remapped to the visible camera basis,
  fixing reversed W/S and A/D controls at rotated angles.
- The camera checks walls and cliff geometry before choosing its distance, so
  it stays on Link's side of nearby architecture.
- Scripted scenes remain voxelled but use a stable fixed voxel camera when the
  game temporarily freezes or replaces Link's gameplay object.
- Dialogue and status UI are composited in screen space above that voxel
  scene instead of exposing a second flat game frame.

## Developer handoff

[`docs/VOXEL_CONTRIBUTING.md`](VOXEL_CONTRIBUTING.md) documents the renderer
pipeline, the savestate/screenshot workflow, source-pixel rules, test matrix,
and a first-task queue for additional contributors.

One known prop still needs that next pass: the chest in the Ages opening scene
is drawn as a two-tile OAM object rather than the normal BG chest metatile. It
keeps the correct live art in this release but does not yet have a dedicated
body-and-lid volume. The guide records the evidence and the safe implementation
path instead of shipping a room-specific guess.

## Verified

The release is covered by focused tests for collision quadrants, connected
cliff height, plateau elevation, screen-relative chase controls, initial
camera heading, camera collision, scripted-scene camera selection, persistent
world objects, and cross-platform updater archives. The player and launcher
are also built and smoke-tested on Linux and Windows by the release workflow.

## Upgrading

Replace the previous release folder or use **Updates** in the launcher. ROMs,
mods, covers, saves, states and voxel tuning files are preserved.
