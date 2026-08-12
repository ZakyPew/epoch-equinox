# Epoch & Equinox v0.4.3

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. Windows users can run the launcher exe; on
Linux run `epoch`, or `launcher/epoch_launcher.py` for the UI (needs Python +
PySide6).

The stream overlays landed in v0.4.1 and were configured by editing files.
This release gives them a page in the launcher.

## A Stream page in the launcher

New menu item, next to Secrets and Updates. It sets up an overlay without a
text editor:

- **pick a layout** — all four, with the canvas size
- **place the openings** — x, y, width and height for the game box and the
  camera box, with a **Snap** button that rounds to the nearest whole
  multiple of the Game Boy's 160×144 that still fits, which is what keeps
  flat mode pixel-crisp
- **copy the numbers** for OBS's Transform box, or the overlay's path for the
  browser source
- **toggle** the camera opening and the alignment guide
- **type the "now building" line**
- **open the folder**

Nothing is written until you press Apply, and an opening that would hang off
the canvas is refused rather than saved — that is a black bar on stream, and
by the time it showed up the file would already be on disk.

Everything it writes is still a plain file you can edit by hand. The page is
a convenience, not a new source of truth.

## Two things that changed underneath

**The camera and guide switches were query parameters**, which are no use in
OBS's "Local file" mode — how most people add these. They live in
`stream/config.js` now, loaded the way `now.js` is and for the same reason:
browsers block `fetch()` over `file://`, but a script from the same folder
loads fine. `?cam` and `?guide` still override for a quick look, and so do
the **C** and **G** keys.

**The alignment guide's caption was a hardcoded string** in each layout, so
moving an opening left it lying about where the opening was. It is generated
from the same CSS custom properties the frame is built from now — which is
also what lets the launcher move a box by rewriting four numbers and have the
mat, the frame, the studs and the caption all follow.

## Verified

The read/write half has no Qt in it, so it runs headless in CI:
`tools/stream_config_test.py`, 30 checks over geometry round trips, a layout
with no openings being left alone, an off-canvas box, snapping that never
runs off the edge, a damaged config file reading as all-off rather than
throwing, and the escaping that stops a typed apostrophe ending a JavaScript
string.

The dialog itself was driven offscreen — it loads the shipped numbers, snaps,
applies, refuses the off-canvas box without touching the file — and
`config.js` was confirmed to drive both framed overlays in a real browser,
with the captions matching the launcher's numbers exactly.

## Upgrading

Nothing in the player changed since v0.4.1; this is the launcher and the
overlay files.
