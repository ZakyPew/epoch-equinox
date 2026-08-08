#!/usr/bin/env python3
"""Generate IPS and BPS patches for exercising the mod loader.

Not a general-purpose patcher — it emits the simplest valid encoding of
"take this base ROM and change these byte ranges", which is enough to drive
every code path the loader has: IPS records (both literal and RLE), and BPS
SourceRead / TargetRead actions with the varint and CRC32 framing.

    python3 tools/make_test_patches.py base.gbc out_dir
"""
from __future__ import annotations

import binascii
import struct
import sys
from pathlib import Path

# (offset, replacement bytes) — chosen to sit in ROM data well away from the
# header so a patched image still boots.
EDITS = [
    (0x00040000, bytes([0xA5] * 16)),
    (0x00081234, bytes([0x5A] * 8)),
]


def make_ips(edits) -> bytes:
    out = bytearray(b"PATCH")
    for off, data in edits:
        if off >= 0xFFFFFF:
            raise ValueError("IPS cannot address past 16 MiB")
        out += struct.pack(">I", off)[1:]        # 3-byte offset
        out += struct.pack(">H", len(data))      # 2-byte length
        out += data
    # An RLE record too, so that branch of the loader is covered.
    rle_off = 0x000C0000
    out += struct.pack(">I", rle_off)[1:]
    out += struct.pack(">H", 0)                  # size 0 marks RLE
    out += struct.pack(">H", 12)                 # run length
    out += bytes([0x3C])                         # fill value
    out += b"EOF"
    return bytes(out)


def bps_varint(value: int) -> bytes:
    """BPS's self-terminating base-128: high bit set marks the last byte,
    and each continuation subtracts one so encodings stay unique."""
    out = bytearray()
    while True:
        x = value & 0x7F
        value >>= 7
        if value == 0:
            out.append(0x80 | x)
            break
        out.append(x)
        value -= 1
    return bytes(out)


def make_bps(source: bytes, target: bytes) -> bytes:
    body = bytearray(b"BPS1")
    body += bps_varint(len(source))
    body += bps_varint(len(target))
    body += bps_varint(0)  # no metadata

    # Walk the target, emitting SourceRead for stretches identical to the
    # source and TargetRead (literal) for stretches that differ.
    i = 0
    n = len(target)
    while i < n:
        same = i < len(source) and target[i] == source[i]
        j = i
        while j < n and ((j < len(source) and target[j] == source[j]) == same):
            j += 1
        length = j - i
        if same:
            body += bps_varint(((length - 1) << 2) | 0)   # SourceRead
        else:
            body += bps_varint(((length - 1) << 2) | 1)   # TargetRead
            body += target[i:j]
        i = j

    out = bytearray(body)
    out += struct.pack("<I", binascii.crc32(source) & 0xFFFFFFFF)
    out += struct.pack("<I", binascii.crc32(target) & 0xFFFFFFFF)
    out += struct.pack("<I", binascii.crc32(bytes(out)) & 0xFFFFFFFF)
    return bytes(out)


def apply_edits(base: bytes, edits) -> bytes:
    buf = bytearray(base)
    for off, data in edits:
        buf[off : off + len(data)] = data
    return bytes(buf)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    base_path, out_dir = Path(argv[1]), Path(argv[2])
    base = base_path.read_bytes()
    out_dir.mkdir(parents=True, exist_ok=True)

    ips = make_ips(EDITS)
    (out_dir / "test.ips").write_bytes(ips)

    target = apply_edits(base, EDITS)
    bps = make_bps(base, target)
    (out_dir / "test.bps").write_bytes(bps)

    # The expected result of the IPS, including its RLE record.
    ips_target = apply_edits(base, EDITS + [(0x000C0000, bytes([0x3C] * 12))])
    (out_dir / "expected_ips.bin").write_bytes(ips_target)
    (out_dir / "expected_bps.bin").write_bytes(target)

    print(f"wrote test.ips ({len(ips)} bytes) and test.bps ({len(bps)} bytes) to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
