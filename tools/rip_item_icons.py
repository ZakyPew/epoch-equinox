#!/usr/bin/env python3
"""Rip the item-tracker icons from YOUR ROMs into the stream overlays.

Runs tools/icon_rip (the headless ripper: boots each installed cart
over your save, walks the menus by game state, and reads every
equippable item's icon out of the HUD's own sprites), then converts
the PAMs it writes into the PNGs the overlay tracker loads:

    stream/icons/items/<cart>/<id>.png

Everything stays on your machine. The folder is gitignored on purpose:
the icons are the cartridge's art, ripped from your ROM for your
stream, and must never be committed or redistributed.

Usage, from the repo root (or pass paths):

    python3 tools/rip_item_icons.py [--tool build/icon_rip]
                                    [--roms build/roms] [--out stream]

The tool loads the battery save that sits next to the binary -- rip
after you have played, so the bag is as full as your file is. Items
the file does not hold cannot be equipped and keep their text label
on the overlay, which is the fallback anyway.
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from pam_to_png import read_pam, write_png  # noqa: E402

CARTS = {"tlozooa.gbc": "tlozooa", "tlozoos.gbc": "tlozoos"}


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", default=os.path.join(ROOT, "build", "icon_rip"))
    ap.add_argument("--roms", default=os.path.join(ROOT, "build", "roms"))
    ap.add_argument("--out", default=os.path.join(ROOT, "stream"))
    args = ap.parse_args(argv)

    if not os.path.exists(args.tool):
        print(f"icon_rip not found at {args.tool} -- build it first "
              "(see the header of tools/icon_rip.c)", file=sys.stderr)
        return 1

    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")

    total = 0
    for rom_name, cart in CARTS.items():
        rom = os.path.join(args.roms, rom_name)
        if not os.path.exists(rom):
            print(f"[{cart}] no ROM at {rom}, skipping")
            continue
        with tempfile.TemporaryDirectory(prefix="iconrip-") as tmp:
            print(f"[{cart}] ripping (a few minutes of headless play)...")
            r = subprocess.run(
                [args.tool, rom, tmp],
                cwd=os.path.dirname(os.path.abspath(args.tool)),
                env=env, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"[{cart}] ripper failed:\n{r.stderr[-800:]}",
                      file=sys.stderr)
                continue
            dst = os.path.join(args.out, "icons", "items", cart)
            os.makedirs(dst, exist_ok=True)
            n = 0
            for name in sorted(os.listdir(tmp)):
                if not name.endswith(".pam"):
                    continue
                w, h, rgba = read_pam(os.path.join(tmp, name))
                ident = name.rsplit("-", 1)[1][:-4]
                out = os.path.join(dst, f"{ident}.png")
                with open(out, "wb") as f:
                    f.write(write_png(w, h, rgba))
                n += 1
            print(f"[{cart}] {n} icon(s) -> {dst}")
            total += n

    if total:
        print(f"\n{total} icons ripped. They are yours alone: the folder "
              "is gitignored, keep it out of any repo or upload.")
    return 0 if total else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
