# Epoch & Equinox v0.6.2

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it.

One theme this time: **the diorama look**, chasing the best voxel demakes.

## Pixel cubes

Every source pixel now reads as a tiny block — its top-left edge lit, its
bottom-right shaded — on diorama ground, chase ground, cliff and wall
faces alike. Trees and tufts, which were already per-pixel geometry,
now carry orientation light (canopy tops brightest, north faces in
shade) plus a faint checker so adjacent blocks catch the light
differently. The cart's own colours still do all the drawing; the edges
only modulate them.

## Tilt-shift focus

A diorama-photo depth of field: sharp in the band where Link stands,
melting toward the top and bottom of the frame. The HUD and dialog
composite after it and stay crisp.

## A horizon worth looking at

Outdoors in chase view, where the ground runs out, a two-rank forest
silhouette now undulates under the sky in the fog's own palette — and
unexplored ground sinks into haze with distance instead of smearing its
border pixels to the horizon as streaks.

## Yours to tune

All of it lives in the Esc menu's new **Diorama finish** section:
*Pixel cubes* and *Tilt-shift blur* sliders, saved to
`voxel/tuning.ini` with everything else. Zero either slider and that
pass is gone entirely.

## Verified

All render/logic suites pass; the reproducible dungeon route
(`tools/dungeon_shot.py`, new since v0.6.1) re-verified its room with
the new look, and `docs/dungeon-chase.png` shows it — the tower floor
reads as laid stone blocks now.
