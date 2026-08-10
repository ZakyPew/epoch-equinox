# Epoch & Equinox v0.3.0

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. The optional launcher UI is
`launcher/epoch_launcher.py` (needs Python + PySide6), and the Windows build
ships it as an executable.

Everything below landed since v0.1.0.

## The 3D mode grew a third dimension worth having

- **Third-person chase camera** (`F3` past the diorama tilts): the same
  heightfield raycast in true perspective, with distance fog, depth-scaled
  sprite billboards, z-buffered occlusion so a cliff can hide you, and held
  items floating over Link's head.
- **The camera follows him.** It swings around behind Link while he walks,
  eased over about a second, and ignores standing turns so tapping a
  direction to face a sign doesn't whip the world around. The right stick
  (or `Q`/`E`) takes over and holds until he walks again; `chase_follow=0`
  in `voxel/tuning.ini` pins it to a fixed heading.
- **The world persists.** Every room you visit is remembered and redrawn
  past the border of the one you're standing in — terrain, cliff faces and
  tree masses — so the world runs to the horizon and fills in as you
  explore instead of ending in fog at the screen edge. About 150 KB per
  visited room, allocated lazily. Seasons rooms refuse to reappear under a
  different season.
- **Objects know what they are**: `wRoomLayout` names the object in each
  cell, so trees and cuttable tufts are told apart from terrain instead of
  guessed at from colour.
- **Cliff faces wear the cart's own tile art**, tiled down the face, so a
  wall reads as rock rather than a smear of the ground colour.
- **3× internal resolution** via a scaled frame hook, with the status bar
  composited back on top flat.
- **Per-room height overrides**, hand-sculptable as plain text: run with
  `VOXEL_EDIT=1` and every room you enter writes a template you can edit,
  reloaded live on save.
- **Live tuning sliders** in the Esc menu for shape, camera and fog, saved
  to `voxel/tuning.ini`.

## Mods

- Mods can dress the chase camera's billboards: `voxel/tree.ppm` and
  `voxel/tuft.ppm` (16×16 P6) in a mod directory turn tree and tuft cells
  into standing billboards wearing that art, with trunk and canopy shading
  derived from it. Without mod art those cells stay extruded terrain — the
  cart's top-down tree tiles read as cardboard cutouts when stood upright.
- Highest-priority mod wins when several supply the same file, the same
  order ROM patches apply in.

## Launcher and shell

- Ships the project's cover art, hides itself while the game runs, and has
  a real mods dialog.
- Captures the runner's output and surfaces early exits instead of failing
  silently.
- Esc menu leads with a Display section and a live scale readout.
- Rewind with room checkpoints, and a HUD that says what happened.
- Licensing splashes flash past instead of playing out at 60 Hz.
- Transparent OBS stream overlays, horizontal and 1080×1920 vertical, with
  real cover art in the emblem.

## Fixes worth calling out

- **Odd-row overworld rooms rendered as bare ground.** `wScreenOffsetY`
  reports a whole room height when a room's tilemap is paged into the lower
  half of the BG map, and feeding that to the screen→room converters pushed
  every collision and layout lookup a full room off the table. Ages pages
  every odd row that way, so half the overworld lost its trees and its
  heightfield — and the world cache, correctly refusing to remember a frame
  it couldn't trust, never stored those rooms.
- Terrain gate relaxed so flat rooms stop being refused, and refusals now
  name their reason.
- Camera unmirrored: walking east moves you screen-east.
- Dialog floats over a frozen diorama instead of flattening the world, and
  frozen frames keep their tree list.
- Launcher-spawned games failing to open a window; carts identified by
  header.

## Building

Nothing here contains game code. `cmake -S . -B build && cmake --build build`
takes about a minute; see the README for platform dependencies.
