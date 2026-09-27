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
            data = root / "data" / "ages"
            for folder in (gfx, rooms / "small", layouts, data):
                folder.mkdir(parents=True)
            for filename, height in (
                ("gfx_tileset_overworld_standard.png", 48),
                ("gfx_tileset_overworld_present.png", 40),
                ("gfx_tileset_talus_peaks_1.png", 16),
                ("gfx_tileset_talus_peaks_2.png", 16),
                ("gfx_tileset_talus_peaks_3.png", 8),
                ("gfx_tileset_lynna_city_1.png", 16),
                ("gfx_tileset_lynna_city_2.png", 16),
                ("gfx_tileset_lynna_city_3.png", 8),
            ):
                (gfx / filename).write_bytes(png_header(128, height))
            (data / "tilesets.s").write_text(
                "; 0x08\n\t.db $0f, $01\n\t.db UNIQUE_GFXH_TALUS_PEAKS\n"
                "\t.db GFXH_TILESET_OVERWORLD_PRESENT\n\t.db PALH_TILESET_TALUS_PEAKS_PRESENT\n"
                "\t.db $06, $00, $01\n",
                encoding="utf-8",
            )
            (data / "gfxHeaders.s").write_text(
                "m_GfxHeaderStart $40, GFXH_TILESET_OVERWORLD_PRESENT\n"
                "\tm_GfxHeader gfx_tileset_overworld_standard, $8801\n"
                "\tm_GfxHeader gfx_tileset_overworld_present, $8e01\n"
                "\tm_GfxHeader gfx_tileset_lynna_city_1, $9301\n"
                "\tm_GfxHeader gfx_tileset_lynna_city_2, $9501\n"
                "\tm_GfxHeader gfx_tileset_lynna_city_3, $9701\n\tm_GfxHeaderEnd\n",
                encoding="utf-8",
            )
            (data / "uniqueGfxHeaders.s").write_text(
                "m_UniqueGfxHeaderStart $09, UNIQUE_GFXH_TALUS_PEAKS\n"
                "\tm_GfxHeader gfx_tileset_talus_peaks_1, $9301\n"
                "\tm_GfxHeader gfx_tileset_talus_peaks_2, $9501\n"
                "\tm_GfxHeader gfx_tileset_talus_peaks_3, $9701\n\tm_GfxHeaderEnd\n",
                encoding="utf-8",
            )
            (rooms / "small" / "room0000.bin").write_bytes(bytes(80))
            (rooms / "group0Tilesets.bin").write_bytes(bytes([8]) + bytes(255))
            (layouts / "tilesetMappings06.bin").write_bytes(bytes(2048))
            (layouts / "tilesetCollisions06.bin").write_bytes(bytes(256))
            output = root / "imported"

            manifest = import_tilesets(root, output)

            self.assertEqual(manifest["room"], {"group": 0, "id": 0, "tileset": 8, "layout": 6, "width": 10, "height": 8})
            self.assertEqual((output / "room0000.bin").stat().st_size, 80)
            self.assertEqual((output / "tilesetMappings06.bin").stat().st_size, 2048)
            self.assertEqual((output / "tilesetCollisions06.bin").stat().st_size, 256)
            self.assertEqual(len(manifest["images"]), 8)
            self.assertEqual([image["start_tile"] for image in manifest["images"]], [0, 96, 176, 208, 240, 176, 208, 240])
            self.assertEqual(json.loads((output / "manifest.json").read_text(encoding="utf-8"))["room"], manifest["room"])


if __name__ == "__main__":
    unittest.main()
