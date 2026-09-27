# Epoch Ages Lab

Epoch includes the `ooa-godot` repository as a pinned Git submodule at
`backends/ooa-godot`, and launches that project's normal title/file-select
flow. Clone Epoch with `--recurse-submodules` (or run `git submodule update
--init --recursive`) to fetch its game source. This folder remains a small
Epoch-owned development fallback, not a second game implementation to grow in
parallel.

To use another checkout, set `OOA_GODOT_PATH` to its project directory or pass
`--godot-backend` to the launcher. The developer-only “Start native Ages” menu
item remains hidden from the normal launcher menu. The backend's own `mods/`
directory is passed through to its native mod system; use
`--godot-mods-dir` to override it for a local test.

When local imported assets are present, the lab instead loads room `0000`
(10x8 metatiles), reads tileset 08's layout index from the disassembly,
applies that tile mapping/collision table, and draws the room from the exact
present-era VRAM graphics sequence (common overworld sheets plus Talus Peaks
unique sheets). Room tiles use the Talus Peaks background palettes decoded
from the local disassembly data; the player remains a simple debug marker.

To show the developer-only native-backend actions, run Epoch's launcher with
`--dev-native-backend`. Godot 4.7.1 with .NET support must be available as
`GODOT4`, `GODOT`, or on `PATH` when launching the full `ooa-godot` project.
The small fallback prototype does not load mods.

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
import the graphics and tables required by room 0000 into the Git-ignored
`imported/` directory. Graphics and layout IDs are resolved from the
disassembly's `tilesets.s` and graphics-header tables:

```powershell
python ..\..\tools\import_ages_gfx.py --disasm-root C:\path\to\oracles-disasm
```

To inspect a particular overworld room (for example, match the reference
window's room `0-8A`), re-import it with `--room-group 0 --room-id 0x8a`.
The Godot lab reads the selected room and its assigned tileset from the manifest.

The prototype automatically loads the room selected during import. Walk with
arrows or WASD; grid cells whose imported collision type is non-zero currently block
movement. Press F1 to inspect the combined 256-tile atlas; Esc closes it. These
decoded assets are not committed or distributed. Exact collision semantics,
object placements, and the correct Link sprite are still follow-up work.

No upstream reconstruction source, ROM, or ROM-derived assets are included.
