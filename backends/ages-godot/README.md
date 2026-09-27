# Epoch Ages Lab

This is an Epoch-owned Godot prototype, implemented independently of the
separate `ooa-godot` reconstruction. Without local imported assets, it falls
back to a fixed 160x144 programmer-art room with deterministic movement,
wall/water collision, and a small saveable chest interaction.

When local imported assets are present, the lab instead loads room `0000`
(10x8 metatiles), applies its tileset-08 tile mappings and collision table, and
draws the room from the decoded overworld graphics. The player remains a simple
debug marker; the initial render uses the source sheets' grayscale palette.

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
import the decoded overworld graphics, room 0000 layout, and its tileset-08
mapping/collision tables into the Git-ignored `imported/` directory:

```powershell
python ..\..\tools\import_ages_gfx.py --disasm-root C:\path\to\oracles-disasm
```

The prototype automatically selects room 0000 after import. Walk with arrows
or WASD; grid cells whose imported collision type is non-zero currently block
movement. Press F1 to inspect the combined 256-tile atlas; Esc closes it. These
decoded assets are not committed or distributed. Palette-header interpretation,
exact collision semantics, object placements, and the correct Link sprite are
still follow-up work.

No upstream reconstruction source, ROM, or ROM-derived assets are included.
