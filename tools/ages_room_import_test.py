from __future__ import annotations

import json
import struct
import tempfile
import unittest
from pathlib import Path

from import_ages_gfx import import_tilesets


def png_header(width: int, height: int) -> bytes:
    return b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">II", width, height)


class AgesRoomImportTests(unittest.TestCase):
    def test_imports_room_graphics_mappings_and_collision_tables(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            gfx = root / "gfx_compressible" / "ages"
            rooms = root / "rooms" / "ages"
            layouts = root / "tileset_layouts" / "ages"
            for folder in (gfx, rooms / "small", layouts):
                folder.mkdir(parents=True)
            for filename, height in (
                ("gfx_tileset_overworld_standard.png", 48),
                ("gfx_tileset_overworld_present.png", 40),
                ("gfx_tileset_overworld_past.png", 40),
            ):
                (gfx / filename).write_bytes(png_header(128, height))
            (rooms / "small" / "room0000.bin").write_bytes(bytes(80))
            (rooms / "group0Tilesets.bin").write_bytes(bytes([8]) + bytes(255))
            (layouts / "tilesetMappings08.bin").write_bytes(bytes(2048))
            (layouts / "tilesetCollisions08.bin").write_bytes(bytes(256))
            output = root / "imported"

            manifest = import_tilesets(root, output)

            self.assertEqual(manifest["room"], {"group": 0, "id": 0, "tileset": 8, "width": 10, "height": 8})
            self.assertEqual((output / "room0000.bin").stat().st_size, 80)
            self.assertEqual((output / "tilesetMappings08.bin").stat().st_size, 2048)
            self.assertEqual((output / "tilesetCollisions08.bin").stat().st_size, 256)
            self.assertEqual(json.loads((output / "manifest.json").read_text(encoding="utf-8"))["room"], manifest["room"])


if __name__ == "__main__":
    unittest.main()
