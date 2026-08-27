# Contributor starter tasks

The three best first contributions for the current voxel milestone, each
filed as a `help wanted` issue with a narrow definition of done that can
be reviewed from screenshots, logs, or a small focused test. General
setup is in the [voxel contributor guide](VOXEL_CONTRIBUTING.md); the
golden rule everywhere: **no ROM or ROM-derived asset is ever
committed** — art and saves stay on the machine they came from.

## The headless play loop (used by all three)

`tools/vox_shot.c` boots a ROM without a window, drives it with an input
script, and dumps flat + voxel renders as PPMs. It can reach *real
gameplay*: park a test save beside the binary under the runner's name
and script the file select — the exact recipe, frame numbers included,
is in the header of `tools/vox_shot.c`. For menu-time navigation that
must not drift between runs, `tools/icon_rip.c` shows the state-driven
pattern (watch `wScrollMode` / `wTextIsActive` / `wOpenedMenuType` and
press what the state calls for, rather than trusting frame numbers).

## 1. Reconstruct the opening-scene chest as a compound voxel object

**Issue:** [#82](https://github.com/ZakyPew/epoch-equinox/issues/82)

The opening scene draws the chest as an OAM pair (`$60/$62`), while
normal map chests use BG tiles (`$F0/$F1`). Treating the pair as one
billboard is why the chest can look flat or let sprites sort incorrectly
around it.

Look first at `src/voxel/voxel_tiles.c` (the OAM scrape at the end of
`vox_scrape`), `src/voxel/voxel_render.c` (billboard draw + z ordering),
and `src/voxel/voxel_internal.h`. The implementation should preserve the
source pixels while giving the chest a body, lid, depth, and one shared
ground footpoint. `vox_decode_sprite_row` already decodes any OAM entry
with its true CGB palette and transparency — build on it rather than
re-deriving tile decoding.

**Done when:**

- The two OAM pieces are detected and rendered as one chest object.
- The body and lid have visible voxel depth without a magenta/opaque
  background.
- Link and nearby sprites sort against the chest's ground position, not
  its top edge.
- The opening scene and a normal BG chest both still render correctly.
- The PR includes flat/voxel screenshots and the exact state or route
  used.

## 2. Add a reproducible dungeon screenshot route

**Issue:** [#83](https://github.com/ZakyPew/epoch-equinox/issues/83)

Create a small route that loads a dungeon state, places the camera
behind Link, and captures the same view in flat and voxel modes.
Dungeon rooms use a different render job and a 15-column layout, so
this route should make camera, height, and billboard regressions
visible without manual searching.

Most of the pieces exist: `tests/saves/ages-veran-tower.sav` loads
inside a dungeon, the file-select script in `tools/vox_shot.c`'s header
reaches live play headlessly, and `VOX_DUMP_ROOM=1` prints the room the
renderer is actually working from. What's missing is one command (a
small script is fine) that runs the route and *names the room it
expects*. Beware combat rooms: knockback timing drifts between runs, so
calibrate where nothing is chasing you.

**Done when:**

- A new contributor can run the route from the documentation alone.
- It produces a stable flat reference and voxel capture.
- The capture shows the camera position, room edges, and at least one
  object that can expose depth ordering.
- Failure output points to the route or state that failed instead of
  silently producing an unrelated screenshot.

## 3. Sculpt one named overworld room-height override

**Issue:** [#84](https://github.com/ZakyPew/epoch-equinox/issues/84)

The in-game editor makes this a play session, not a programming task:
press **F4** in a voxel mode, paint the cells with `1-5`, `0` to hand a
cell back to collision, Backspace to undo. Every press writes the
room's override file (`voxel/overrides/<game>-<group>-<room>.txt`) —
plain text, reviewable, and exactly what you commit. Pick one area
where a cliff, ledge, or tree canopy reads as a floating billboard, and
prefer a small override over a renderer-wide rule.

**Done when:**

- The area name and room id are recorded (the sculpt HUD shows it).
- The override improves the selected room without flattening adjacent
  terrain, and the top surface is level with its cliff edge.
- Flat mode is unchanged (overrides only feed the voxel path).
- Voxel screenshots from at least two angles show the edge, ground
  level, trunks and nearby props — `tools/vox_shot.c` takes them.
- The change is data-driven or isolated enough to revert independently.

## How to propose a different task

Open an issue using the closest template. Include the area/state,
expected result, observed result, and the smallest screenshot or log
that proves it. For visual bugs, a single focused capture is more
useful than a full playthrough video.
