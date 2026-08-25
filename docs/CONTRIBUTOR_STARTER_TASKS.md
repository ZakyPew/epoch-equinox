# Contributor starter tasks

These are the three best first contributions for the current voxel milestone. Each one has a narrow definition of done and can be reviewed from screenshots, logs, or a small focused test.

## 1. Reconstruct the opening-scene chest as a compound voxel object

**Suggested issue title:** `[Voxel] Reconstruct the opening-scene chest as a compound OAM object`

The opening scene draws the chest as an OAM pair (`$60/$62`), while normal map chests use BG tiles (`$F0/$F1`). Treating the pair as one billboard is why the chest can look flat or allow sprites to sort incorrectly around it.

Look first at `src/voxel/voxel_tiles.c`, `src/voxel/voxel_render.c`, and `src/voxel/voxel_internal.h`. The implementation should preserve the source pixels while giving the chest a body, lid, depth, and one shared ground footpoint.

**Done when:**

- The two OAM pieces are detected and rendered as one chest object.
- The body and lid have visible voxel depth without a magenta/opaque background.
- Link and nearby sprites sort against the chest's ground position, not its top edge.
- The opening scene and a normal BG chest both still render correctly.
- The PR includes flat/voxel screenshots and the exact state or route used.

## 2. Add a reproducible dungeon screenshot route

**Suggested issue title:** `[Testing] Add a reproducible dungeon screenshot route`

Create a small route that loads a dungeon state, places the camera behind Link, and captures the same view in flat and voxel modes. Dungeon rooms use a different render job and a 15-column layout, so this route should make camera, height, and billboard regressions visible without manual searching.

Keep the route deterministic. It may use a legal save already in the project workflow, but do not add a ROM or ROM-derived asset to the repository. Document the room/state and the command or flags needed to capture it.

**Done when:**

- A new contributor can run the route from the documentation alone.
- It produces a stable flat reference and voxel capture.
- The capture shows the camera position, room edges, and at least one object that can expose depth ordering.
- Failure output points to the route or state that failed instead of silently producing an unrelated screenshot.

## 3. Sculpt one named overworld room-height override

**Suggested issue title:** `[Voxel] Author a room-height override for one named overworld area`

Use the voxel editor workflow to fix one area where a cliff, ledge, or tree canopy currently reads as a floating billboard. Capture the room coordinates and the tile edge that establishes the intended ground level before changing the height map.

Prefer a small override over a renderer-wide rule. The top surface should be level with the cliff edge, higher ground should actually be elevated, and trunks/canopies should preserve the original tile relationship from more than one camera angle.

**Done when:**

- The area name and coordinates are recorded.
- The override improves the selected room without flattening adjacent terrain.
- Flat mode is unchanged.
- Voxel screenshots from at least two angles show the edge, ground level, and nearby props.
- The change is data-driven or isolated enough to revert independently.

## How to propose a different task

Open an issue using the closest template. Include the area/state, expected result, observed result, and the smallest screenshot or log that proves it. For visual bugs, a single focused capture is more useful than a full playthrough video.
