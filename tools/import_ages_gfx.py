#!/usr/bin/env python3
"""Import decoded Oracle of Ages tileset sheets from a local disassembly tree.

The source ROM and decoded game art remain outside the repository. The importer
copies a small, explicit allowlist into Godot's ignored local asset directory.
It does not execute or import code from any external reconstruction project.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
from pathlib import Path


TILESETS = (
    "gfx_tileset_overworld_standard.png",
    "gfx_tileset_overworld_present.png",
    "gfx_tileset_overworld_past.png",
)


def png_dimensions(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError(f"Not a valid PNG image: {path}")
    return struct.unpack(">II", header[16:24])


def import_tilesets(disasm_root: Path, output_dir: Path) -> dict[str, object]:
    source_dir = disasm_root / "gfx_compressible" / "ages"
    images: list[dict[str, object]] = []
    output_dir.mkdir(parents=True, exist_ok=True)
    for filename in TILESETS:
        source = source_dir / filename
        if not source.is_file():
            raise FileNotFoundError(f"Required decoded Ages graphics are missing: {source}")
        width, height = png_dimensions(source)
        destination = output_dir / filename
        shutil.copyfile(source, destination)
        images.append({"file": filename, "width": width, "height": height})
    room = disasm_root / "rooms" / "ages" / "small" / "room0000.bin"
    assignments = disasm_root / "rooms" / "ages" / "group0Tilesets.bin"
    if not room.is_file() or not assignments.is_file():
        raise FileNotFoundError(f"Room 0000 or its tileset assignment is missing from {disasm_root}")
    room_data = room.read_bytes()
    assignment_data = assignments.read_bytes()
    if not assignment_data:
        raise ValueError(f"Room group 0 tileset assignment table is empty: {assignments}")
    tileset_id = assignment_data[0]
    if len(room_data) != 80:
        raise ValueError(f"Expected an 80-byte 10x8 room layout: {room}")
    mappings = disasm_root / "tileset_layouts" / "ages" / f"tilesetMappings{tileset_id:02x}.bin"
    collisions = disasm_root / "tileset_layouts" / "ages" / f"tilesetCollisions{tileset_id:02x}.bin"
    for source, expected_size in ((mappings, 2048), (collisions, 256)):
        if not source.is_file() or source.stat().st_size != expected_size:
            raise ValueError(f"Missing or malformed tileset {tileset_id:02x} table: {source}")
        shutil.copyfile(source, output_dir / source.name)
    shutil.copyfile(room, output_dir / "room0000.bin")
    manifest: dict[str, object] = {
        "format": 1,
        "source": "local oracles-disasm gfx_compressible/ages",
        "images": images,
        "room": {"group": 0, "id": 0, "tileset": tileset_id, "width": 10, "height": 8},
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disasm-root", type=Path, required=True, help="Local oracles-disasm checkout")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "backends" / "ages-godot" / "imported",
        help="Local Godot asset output (ignored by Git)",
    )
    args = parser.parse_args()
    manifest = import_tilesets(args.disasm_root.resolve(), args.output_dir.resolve())
    print(f"Imported {len(manifest['images'])} decoded Ages tilesets to {args.output_dir.resolve()}")
    for image in manifest["images"]:
        print(f"  {image['file']}: {image['width']}x{image['height']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
