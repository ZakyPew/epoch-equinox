# Epoch & Equinox v0.4.1

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. Windows users can run the launcher exe; on
Linux run `epoch`, or `launcher/epoch_launcher.py` for the UI (needs Python +
PySide6).

v0.4.0 was about everything around the game. This one is about how it looks:
on your screen, and on someone else's.

## The camera answers to you

The chase camera used to swing itself and fight you for the stick. Now the
right stick owns it: the camera stays on Link and orbits where you point it,
and it does not quietly drift back on its own. Recentring is something you
**ask** for — click the stick, or turn on auto-recentre in the display menu,
which is **off by default**.

## Cliffs stop notching

Each tile used to vote on its own height, so one cliff face could come out as
a row of steps with a notch every few tiles. Solid masses are now flood-filled
and settled together: one cliff, one height. Ground that sits directly below a
tall wall is lifted to a mid step, so the drop reads as a bevel rather than a
sheer cut.

## Stream overlays

Four transparent OBS overlays now, landscape and vertical, in two styles.
`overlay.html` and `overlay-vertical.html` float a few panels over your
capture, as before. The new **`overlay-framed.html`** and
**`overlay-framed-vertical.html`** dress the whole canvas instead: a navy mat,
a gilded frame around the play area, and a rail of live stats beside it.

The opening in the frame is a real hole — the mat is four panels built around
it, so your game capture shows through and not one pixel is covered. Load it
with `?guide` and it prints the exact rectangle to type into OBS's Transform
box; `?cam` opens a second 16:9 hole for a camera. Both boxes are whole
multiples of 160×144, so flat mode stays pixel-crisp.

**They are live.** While the player runs it writes `stream/live.js` once a
second, and the overlays show the game, the room, a linked-game badge,
essences, hearts, rings, rupees, deaths, play time and achievements earned —
with an unlock card that lands the instant you earn one, at full canvas size
rather than as a small toast inside a scaled game window. Nothing appears until
you are actually in a room with a file loaded, and the panel hides itself
rather than leave stale numbers up if the player exits.

Setup is in `stream/README.md`, and `stream/` ships beside the binary.

## Achievement icons

The 47 achievement icons are now real artwork with true alpha, in both the
in-game card and the launcher's browser — the old chroma-key edge is gone.

## Fixes

- **The Windows build was broken.** MSVC has no `M_PI`, and nothing was
  compiling the player on merges to main, so it went unnoticed for a release.
  Both are fixed: the constant is local now, and every merge builds the player.
- Pixel art is marked binary, so a Windows checkout cannot mangle it.

## Under the hood

- `VOX_DUMP_ROOM=1` prints a room's collision grid, which is how the dungeon
  question got settled: the data indoors is correct, and the remaining oddity
  is the screen-to-room mapping for large scrolling rooms, not the tileset.
- Real battery saves live in `tests/saves/` now. They immediately caught a
  linked-game check watching a runtime copy of the flag instead of the one in
  the save block.

## Verified

CI builds the player on Linux and Windows on every merge and runs the whole
test suite: achievements, both secret encoders, the typist, the chase camera,
the cliff unifier, the updater, and now the stream feed — 34 checks over the
gate that keeps the file-select screen from driving the overlay, both carts'
memory layouts, and the escaping that stops a pack title's quote ending a
JavaScript string early.

All four overlays were rendered and checked in a real browser over `file://`,
the way OBS loads them, including the transparency of both cut-out holes.
