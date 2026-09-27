# Epoch Ages Lab

This is an Epoch-owned Godot prototype, implemented independently of the
separate `ooa-godot` reconstruction. It currently demonstrates a fixed
160x144 room grid, deterministic tile-step movement, wall/water collision, one
interactive chest, and a small local save. The scene art is temporary
programmer art; this is not yet imported Oracle of Ages content.

The prototype is deliberately hidden from the normal launcher menu. To show the
developer-only “Start native Ages” action, run Epoch's launcher with
`--dev-native-backend`. Godot 4 must be available as `GODOT4`, `GODOT`, or on
`PATH`. The launcher discovers this project at `backends/ages-godot` and passes
the selected mods directory through `--mods-dir`; the prototype does not load
mods yet.

Run directly from this directory:

```powershell
& $env:GODOT4 --path .
```

Run its headless regression:

```powershell
& $env:GODOT4 --headless --path . -- --smoke-test
```

## Local graphics import

With a local `oracles-disasm` checkout (built from a clean supported ROM),
import the first decoded overworld graphics sheets into the Git-ignored
`imported/` directory:

```powershell
python ..\..\tools\import_ages_gfx.py --disasm-root C:\path\to\oracles-disasm
```

Press F1 in the prototype to inspect the locally imported overworld tileset
sheet; Esc closes the atlas. These decoded assets are not committed or
distributed. This is an initial asset-pipeline milestone, not yet a room
renderer: translating room dictionaries/layouts, palettes, tile properties,
and object placements is the next reconstruction step.

No upstream reconstruction source, ROM, or ROM-derived assets are included.
