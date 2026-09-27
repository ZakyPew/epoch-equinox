#!/usr/bin/env python3
"""Import decoded Oracle of Ages tileset sheets from a local disassembly tree.

The source ROM and decoded game art remain outside the repository. The importer
copies a small, explicit allowlist into Godot's ignored local asset directory.
It does not execute or import code from any external reconstruction project.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
from pathlib import Path


ROOM_GROUP = 0
ROOM_ID = 0
ROOM_WIDTH = 10
ROOM_HEIGHT = 8


def png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError(f"Not a valid PNG image: {path}")
    return struct.unpack(">II", header[16:24])


def read_tileset_record(tilesets_source: Path, tileset_id: int) -> list[str]:
    lines = tilesets_source.read_text(encoding="utf-8").splitlines()
    marker = f"; 0x{tileset_id:02x}"
    start = next((i for i, line in enumerate(lines) if line.strip().lower() == marker), None)
    if start is None:
        raise ValueError(f"Could not find tileset record {tileset_id:02x} in {tilesets_source}")
    rows: list[str] = []
    for line in lines[start + 1 :]:
        if re.match(r"\s*;\s*0x[0-9a-f]{2}\s*$", line, re.IGNORECASE):
            break
        match = re.match(r"\s*\.db\s+(.+?)\s*$", line, re.IGNORECASE)
        if match:
            rows.append(match.group(1))
            if len(rows) == 5:
                break
    if len(rows) != 5:
        raise ValueError(f"Tileset record {tileset_id:02x} is incomplete in {tilesets_source}")
    return rows


def resolve_graphics_header(header_source: Path, symbol: str, unique: bool) -> list[dict[str, object]]:
    macro = "m_UniqueGfxHeaderStart" if unique else "m_GfxHeaderStart"
    lines = header_source.read_text(encoding="utf-8").splitlines()
    start = next((i for i, line in enumerate(lines) if macro in line and symbol in line), None)
    if start is None:
        raise ValueError(f"Could not find graphics header {symbol} in {header_source}")
    graphics: list[dict[str, object]] = []
    for line in lines[start + 1 :]:
        if "m_GfxHeaderEnd" in line:
            break
        match = re.search(r"\bm_GfxHeader\s+([A-Za-z0-9_]+)\s*,\s*\$([0-9a-f]{4})\b", line, re.IGNORECASE)
        if match:
            address = int(match.group(2), 16) & 0xFFFE
            if 0x8800 <= address < 0x9800 and (address - 0x8800) % 16 == 0:
                graphics.append({"file": match.group(1) + ".png", "start_tile": (address - 0x8800) // 16})
    if not graphics:
        raise ValueError(f"Graphics header {symbol} has no PNG assets in {header_source}")
    return graphics


def resolve_background_palettes(header_source: Path, data_source: Path, symbol: str) -> dict[str, object]:
    header_lines = header_source.read_text(encoding="utf-8").splitlines()
    start = next(
        (i for i, line in enumerate(header_lines) if re.search(rf"m_PaletteHeaderStart\s+\$[0-9a-f]+,\s*{re.escape(symbol)}\b", line, re.IGNORECASE)),
        None,
    )
    if start is None:
        raise ValueError(f"Could not find palette header {symbol} in {header_source}")
    entry: tuple[int, int, str] | None = None
    for line in header_lines[start + 1 :]:
        if "m_PaletteHeaderEnd" in line:
            break
        match = re.search(r"\bm_PaletteHeaderBg\s+(\d+)\s*,\s*(\d+)\s*,\s*([A-Za-z0-9_]+)", line)
        if match:
            if entry is not None:
                raise ValueError(f"Palette header {symbol} has multiple background ranges")
            entry = (int(match.group(1)), int(match.group(2)), match.group(3))
    if entry is None:
        raise ValueError(f"Palette header {symbol} has no background palette range")
    first_palette, palette_count, data_label = entry

    data_lines = data_source.read_text(encoding="utf-8").splitlines()
    data_start = next((i for i, line in enumerate(data_lines) if line.strip() == f"{data_label}:"), None)
    if data_start is None:
        raise ValueError(f"Could not find palette data {data_label} in {data_source}")
    colors: list[list[int]] = []
    for line in data_lines[data_start + 1 :]:
        match = re.search(r"\bm_RGB16\s+\$([0-9a-f]{1,2})\s+\$([0-9a-f]{1,2})\s+\$([0-9a-f]{1,2})", line, re.IGNORECASE)
        if match:
            colors.append([int(component, 16) for component in match.groups()])
            if len(colors) == palette_count * 4:
                break
        elif colors and line.strip().endswith(":"):
            break
    if len(colors) != palette_count * 4:
        raise ValueError(f"Palette data {data_label} has {len(colors)} colors; expected {palette_count * 4}")
    palettes = [colors[index : index + 4] for index in range(0, len(colors), 4)]
    return {"start_index": first_palette, "palettes": palettes}


def import_tilesets(disasm_root: Path, output_dir: Path, room_group: int = ROOM_GROUP, room_id: int = ROOM_ID) -> dict[str, object]:
    source_dir = disasm_root / "gfx_compressible" / "ages"
    sprite_source = disasm_root / "gfx" / "common" / "spr_link.png"
    data_dir = disasm_root / "data" / "ages"
    images: list[dict[str, object]] = []
    output_dir.mkdir(parents=True, exist_ok=True)
    if not 0 <= room_group <= 7 or not 0 <= room_id <= 255:
        raise ValueError("Room group must be 0..7 and room ID must be 0..255")
    room_file = f"room{room_id:04x}.bin"
    room = disasm_root / "rooms" / "ages" / "small" / room_file
    assignments = disasm_root / "rooms" / "ages" / f"group{room_group}Tilesets.bin"
    if not room.is_file() or not assignments.is_file():
        raise FileNotFoundError(f"Room {room_group}-{room_id:02x} or its tileset assignment is missing from {disasm_root}")
    room_data = room.read_bytes()
    assignment_data = assignments.read_bytes()
    if len(assignment_data) <= room_id:
        raise ValueError(f"Room group {room_group} tileset assignment table has no entry for room {room_id:02x}: {assignments}")
    if len(room_data) != ROOM_WIDTH * ROOM_HEIGHT:
        raise ValueError(f"Expected an 80-byte 10x8 room layout: {room}")
    tileset_assignment = assignment_data[room_id]
    tileset_id = tileset_assignment & 0x7F
    record = read_tileset_record(data_dir / "tilesets.s", tileset_id)
    layout_numbers = re.findall(r"\$([0-9a-f]{2})", record[4], re.IGNORECASE)
    if len(layout_numbers) != 3:
        raise ValueError(f"Could not decode tileset {tileset_id:02x} mapping/layout indices")
    layout_id = int(layout_numbers[0], 16)
    graphics = resolve_graphics_header(data_dir / "gfxHeaders.s", record[2], unique=False)
    graphics += resolve_graphics_header(data_dir / "uniqueGfxHeaders.s", record[1], unique=True)
    palettes = resolve_background_palettes(
        data_dir / "paletteHeaders.s", data_dir / "paletteData.s", record[3]
    )
    covered_tiles: set[int] = set()
    for entry in graphics:
        filename = str(entry["file"])
        source = source_dir / filename
        if not source.is_file():
            raise FileNotFoundError(f"Required decoded Ages graphics are missing: {source}")
        width, height = png_dimensions(source)
        if width != 128 or height % 8 != 0:
            raise ValueError(f"Expected an 8x8-aligned 128px-wide VRAM sheet: {source} ({width}x{height})")
        start_tile = int(entry["start_tile"])
        tile_count = (width // 8) * (height // 8)
        if start_tile % 16 != 0 or start_tile + tile_count > 256:
            raise ValueError(f"Graphics header places {filename} outside the BG tile index atlas")
        destination = output_dir / filename
        shutil.copyfile(source, destination)
        images.append({"file": filename, "width": width, "height": height, "start_tile": start_tile})
        covered_tiles.update(range(start_tile, start_tile + tile_count))
    if len(covered_tiles) != 256:
        raise ValueError(f"Graphics for tileset {tileset_id:02x} fill only {len(covered_tiles)} of 256 BG tile slots")
    mappings = disasm_root / "tileset_layouts" / "ages" / f"tilesetMappings{layout_id:02x}.bin"
    collisions = disasm_root / "tileset_layouts" / "ages" / f"tilesetCollisions{layout_id:02x}.bin"
    for source, expected_size in ((mappings, 2048), (collisions, 256)):
        if not source.is_file() or source.stat().st_size != expected_size:
            raise ValueError(f"Missing or malformed tileset {tileset_id:02x} table: {source}")
        shutil.copyfile(source, output_dir / source.name)
    shutil.copyfile(room, output_dir / room_file)
    sprite_name = "spr_link.png"
    if not sprite_source.is_file():
        raise FileNotFoundError(f"Required decoded Link sprite sheet is missing: {sprite_source}")
    sprite_width, sprite_height = png_dimensions(sprite_source)
    if sprite_width != 128 or sprite_height < 16 or sprite_height % 8 != 0:
        raise ValueError(f"Expected an 8x8-aligned 128px-wide Link sprite sheet: {sprite_source} ({sprite_width}x{sprite_height})")
    shutil.copyfile(sprite_source, output_dir / sprite_name)
    manifest: dict[str, object] = {
        "format": 1,
        "source": "local oracles-disasm decoded Ages graphics and layout tables",
        "images": images,
        "palettes": palettes,
        "room": {"group": room_group, "id": room_id, "tileset": tileset_id, "assignment": tileset_assignment, "layout": layout_id, "width": ROOM_WIDTH, "height": ROOM_HEIGHT, "file": room_file},
        # Link's special-object animation table points idle at 0x2140 and its
        # two-step walk at 0x2080/0x20c0 within spr_link's decoded tile stream.
        "player_sprite": {"file": sprite_name, "format": "gameboy_oam_8x16_pair", "tile_ids": [0, 2], "idle_offset": 0x2140, "walking_offsets": [0x2080, 0x20C0]},
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disasm-root", type=Path, required=True, help="Local oracles-disasm checkout")
    parser.add_argument("--room-group", type=lambda value: int(value, 0), default=ROOM_GROUP, help="Ages room group (default: 0)")
    parser.add_argument("--room-id", type=lambda value: int(value, 0), default=ROOM_ID, help="Ages room ID (decimal or 0x-prefixed; default: 0)")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "backends" / "ages-godot" / "imported",
        help="Local Godot asset output (ignored by Git)",
    )
    args = parser.parse_args()
    manifest = import_tilesets(args.disasm_root.resolve(), args.output_dir.resolve(), args.room_group, args.room_id)
    room = manifest["room"]
    print(f"Imported Ages room {room['group']:02x}{room['id']:02x} (tileset {room['tileset']:02x}, layout {room['layout']:02x}) to {args.output_dir.resolve()}")
    for image in manifest["images"]:
        print(f"  {image['file']}: {image['width']}x{image['height']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
