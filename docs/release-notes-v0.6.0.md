# Epoch & Equinox v0.6.0

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. Windows users can run the launcher exe; on
Linux run `epoch`, or `launcher/epoch_launcher.py` for the UI (needs Python +
PySide6).

Two headliners: sculpt the voxel world from inside the game, and a full
speedrunning kit — auto-splits that drive LiveSplit, plus a run timer, split
list, item tracker and input display on the stream overlays.

## Sculpt rooms from inside the game

The voxel terrain has always been overridable per room with a text file, and
the file has always reloaded live. What was missing was everything in front
of that: you had to alt-tab to a text editor and type letters into a grid.

Now press **F4** in any voxel mode (or toggle *Sculpt room heights* in the
Esc menu). The cell in front of Link glows gold — that's the brush — and the
number keys paint it:

`1` flat · `2` water · `3` low · `4` mid · `5` high · `0` back to collision

Every press rewrites the room's override file for you, atomically, and the
terrain reshapes immediately. The files are the same documented format they
always were: hand edits and in-game edits are interchangeable, and cells you
authored by hand survive in-game painting. `VOXEL_EDIT=1` still writes
templates, and now also starts the player with sculpt mode armed.

The gold highlight is painted into the terrain texture itself, so it lands on
the real extruded geometry in every voxel mode rather than floating in screen
space.

## Auto-splits, driving LiveSplit

The player watches the game's own memory and splits when you earn the thing —
nobody hits a button mid-run. A split file is an achievement pack
(`splits/<cart>.txt`, same six condition kinds, same addresses), one entry
per segment, in route order. Both games ship an essence route; copy the file
and cut what you skip.

It deliberately does not keep the time: **LiveSplit** does, with your PB and
history already in it. Start LiveSplit's TCP server, tick *Send splits to
LiveSplit* on the launcher's Stream page, and the player pushes
`startorsplit` the frame a condition fires. Loopback only; never blocks the
emulator; LiveSplit not running is silent and retried, so starting it
mid-session just works.

Two run-ruiners are designed out and tested: the file-select screen (a save
block sits in WRAM just to draw the preview — browsing files must not fire a
route) and loading a save mid-route (what the file already has is marked as
behind you without firing; the next real split still fires). The shipped
routes are verified end-to-end against real finished saves in CI.

## The overlays learned to speedrun

Four new blocks on the stream overlays, each off unless switched on from the
Stream page:

- **Run timer** — the file's own clock, to hundredths. It pauses when the
  game does and survives an overlay reload mid-run.
- **Split list** — the route with your current segment marked, times from
  the game's frame counter.
- **Item tracker** — the run's items lit as you collect them, from the same
  treasure bits the splits watch. Upgradeables name their tier: the cell
  says **Noble**, not "sword 2".
- **Input display** — the buttons being held, in the Game Boy's layout.

Turning any of them on puts the overlay in run mode: the "now building"
ticker and the achievement plaque step aside for the split list.

## Also

- The overlays now have a rendering test in CI: every layout is loaded in a
  real Chromium over `file://` — the way OBS loads them — and 50+ checks
  cover panels filling in and hiding, hole transparency, icon fallbacks, and
  that no block ever overlaps another or hangs off the safe area. It caught
  two real layout bugs before they shipped.
- `vox_shot` prints why the terrain gate declined a frame, and documents two
  probe findings: the attract demo never populates `wRoomCollisions`, and a
  rewind-state resume restores VRAM but not banked WRAM (rooms look right,
  the game inside is not running).

## Verified

`tools/voxedit_test.c` closes the sculpting loop headlessly: paint a cell,
then ask the override loader — the code the renderer actually calls — what
the room looks like now. 29 checks, including that two paints inside the
same second both land (mtime granularity would otherwise eat the second
one), that hand-authored cells survive in-game painting, and that garbage
codes are refused. `tools/splits_test.c` (34 checks) and the overlay render
test run in CI beside the rest of the suite.

The sculpt HUD and the gold tint follow the same drawing paths as the
shipped HUDs and terrain textures, but have not yet been eyeballed in a live
session — the two headless probe routes both turned out to run without the
game's collision state, which is the finding documented above. First F4
press on a real session will show it; the file-writing core underneath is
fully tested either way.
