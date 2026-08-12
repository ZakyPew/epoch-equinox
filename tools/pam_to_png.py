#!/usr/bin/env python3
"""Mirror the achievement icons into PNGs the stream overlays can load.

The icons ship as PAM (P7) because that is what the player reads: a
header and raw RGBA, no decoder to carry. No browser reads PAM, though,
and the overlays are browser pages -- so the same pictures are mirrored
into stream/icons/<cart>/<id>.png, which every browser does read.

Stdlib only, and deterministic: the same PAM in gives byte-identical
PNG out, so CI can regenerate and diff to catch a mirror that drifted
from its source.

  python3 tools/pam_to_png.py            # write stream/icons/
  python3 tools/pam_to_png.py --check    # fail if anything is stale
"""

import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "achievements", "icons")
DST = os.path.join(ROOT, "stream", "icons")


def read_pam(path):
    """Return (width, height, rgba_bytes) for a P7 RGB_ALPHA file."""
    with open(path, "rb") as f:
        blob = f.read()
    if not blob.startswith(b"P7"):
        raise ValueError("%s: not a PAM" % path)
    end = blob.find(b"ENDHDR\n")
    if end < 0:
        raise ValueError("%s: no ENDHDR" % path)
    header = {}
    for line in blob[:end].split(b"\n")[1:]:
        parts = line.split()
        if len(parts) == 2:
            header[parts[0].decode()] = parts[1].decode()
    if header.get("DEPTH") != "4" or header.get("MAXVAL") != "255":
        raise ValueError("%s: expected 8-bit RGBA, got %r" % (path, header))
    w, h = int(header["WIDTH"]), int(header["HEIGHT"])
    pixels = blob[end + len(b"ENDHDR\n"):]
    if len(pixels) != w * h * 4:
        raise ValueError("%s: %d bytes of pixels, expected %d"
                         % (path, len(pixels), w * h * 4))
    return w, h, pixels


def write_png(w, h, rgba):
    """Smallest honest PNG: 8-bit RGBA, filter 0 on every row."""
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def main(argv):
    check = "--check" in argv
    stale, written = [], 0
    for cart in sorted(os.listdir(SRC)):
        cart_dir = os.path.join(SRC, cart)
        if not os.path.isdir(cart_dir):
            continue
        out_dir = os.path.join(DST, cart)
        if not check:
            os.makedirs(out_dir, exist_ok=True)
        for name in sorted(os.listdir(cart_dir)):
            if not name.endswith(".pam"):
                continue
            png = write_png(*read_pam(os.path.join(cart_dir, name)))
            out = os.path.join(out_dir, name[:-4] + ".png")
            if check:
                have = open(out, "rb").read() if os.path.exists(out) else None
                if have != png:
                    stale.append(os.path.relpath(out, ROOT))
                continue
            with open(out, "wb") as f:
                f.write(png)
            written += 1

    if check:
        if stale:
            print("stale or missing PNG mirrors:", file=sys.stderr)
            for s in stale:
                print("  " + s, file=sys.stderr)
            print("run: python3 tools/pam_to_png.py", file=sys.stderr)
            return 1
        print("[pam_to_png] every icon mirror is current")
        return 0
    print("[pam_to_png] wrote %d PNGs into stream/icons/" % written)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
