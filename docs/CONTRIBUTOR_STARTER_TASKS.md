# Contributor starter tasks

Companion notes for the `help wanted` issues. Each section names the
issue it backs, the files involved, and the fast loop for checking your
work without a display. General setup is in the
[voxel contributor guide](VOXEL_CONTRIBUTING.md); the golden rule
everywhere: **no ROM or ROM-derived asset is ever committed** — art and
saves stay on the machine they came from.

## The headless play loop (used by all three)

`tools/vox_shot.c` boots a ROM without a window, drives it with an input
script, and dumps flat + voxel renders as PPMs. Since the dungeon work
it can reach *real gameplay*: park a test save beside the binary under
the runner's name and script the file select. The exact recipe — frame
numbers included — is in the header of `tools/vox_shot.c`. For menu-time
navigation that must not drift between runs, `tools/icon_rip.c` shows
the state-driven pattern (watch `wScrollMode` / `wTextIsActive` /
`wOpenedMenuType` and press what the state calls for, rather than
trusting frame numbers).

## Issue #82 — the opening-scene chest as a compound OAM object

Normal chests are BG layout objects (`$F0`/`$F1`) and extrude fine; the
scripted opening scene draws its chest as an OAM sprite pair, which
today billboards flat and can sort wrongly against nearby sprites.

- Where: `src/voxel/voxel_tiles.c` (the OAM scrape at the end of
  `vox_scrape`), `src/voxel/voxel_render.c` (billboard draw + z
  ordering), `src/voxel/voxel_internal.h`.
- The shape of the fix: group the pair into one object with body, lid,
  real depth, and a single shared world-space footpoint to sort by.
  `vox_decode_sprite_row` already decodes any OAM entry with its true
  CGB palette and transparency — build on it, don't re-derive.
- Prove it with two captures: the opening scene and a normal BG chest,
  flat vs voxel, plus the script or state that reaches them.

## Issue #83 — a reproducible dungeon screenshot route

Most of this exists now; the task is to turn it into a documented,
stable route rather than a recipe in a header comment.

- The pieces: `tests/saves/ages-veran-tower.sav` loads inside a dungeon,
  the file-select script in `tools/vox_shot.c`'s header reaches live
  play headlessly, and `VOX_DUMP_ROOM=1` prints the room the renderer
  is actually working from.
- What's missing: one command (a small script is fine) that runs the
  route, captures the same flat + chase view every time, and *names the
  room it expects* — so a failure says "wrong room / wrong state"
  instead of silently shooting an unrelated scene. Beware combat rooms:
  knockback timing drifts between runs, so calibrate where nothing is
  chasing you.

## Issue #84 — sculpt one named overworld area

The in-game editor makes this a play session, not a programming task:
press **F4** in a voxel mode, paint the cells with `1-5`, `0` to hand a
cell back to collision, Backspace to undo. Every press writes the
room's override file (`voxel/overrides/<game>-<group>-<room>.txt`) —
plain text, reviewable, and exactly what you commit.

- Pick one area where a cliff, ledge or canopy reads wrong, note the
  room id (the sculpt HUD shows it), and keep the change small.
- Done looks like: the high ground actually elevated, the top surface
  level with its cliff edge, neighbours untouched, and two camera
  angles showing edge, ground, trunks and props. `tools/vox_shot.c`
  takes the screenshots; flat mode must be unaffected (overrides only
  feed the voxel path).
