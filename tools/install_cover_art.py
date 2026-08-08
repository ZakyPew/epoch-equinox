#!/usr/bin/env python3
"""Install panel art into the launcher, correctly named and sized.

    python3 tools/install_cover_art.py --ages hourglass.png --seasons wheel.png

By default it writes to art/covers/ (committed, shipped with the project).
Pass --local to write to build/covers/ instead, which is gitignored -- use
that for anything derived from the games' own art.

Handles the fiddly parts: the launcher keys off exact filenames (tlozooa.png,
tlozoos.png), wants square art, and crops to the middle ~70% of a panel. Art
that isn't square is centre-cropped here rather than silently mangled at draw
time, and you get told what was lost.

Needs Pillow:  pip install pillow
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("This needs Pillow:  pip install pillow")

TARGET = 1600
MIN_USABLE = 1000

# The launcher looks these up by exact name; see launcher/epoch_launcher.py.
GAME_IDS = {"ages": "tlozooa", "seasons": "tlozoos"}


def install(source: Path, game_id: str, dest_dir: Path) -> None:
    im = Image.open(source)
    im = im.convert("RGBA") if im.mode in ("RGBA", "LA", "P") else im.convert("RGB")
    w, h = im.size
    print(f"  {source.name}: {w}x{h} {im.mode}")

    if min(w, h) < MIN_USABLE:
        print(
            f"    warning: shorter side is {min(w, h)}px, under the {MIN_USABLE}px "
            "minimum -- this will soften once the window is maximised",
            file=sys.stderr,
        )

    # Centre-crop to square. The panel is roughly 1.1:1, so a square source
    # loses almost nothing; anything wider or taller loses the difference.
    if w != h:
        side = min(w, h)
        left, top = (w - side) // 2, (h - side) // 2
        lost = abs(w - h)
        axis = "sides" if w > h else "top and bottom"
        print(f"    not square: centre-cropping to {side}x{side} ({lost}px off the {axis})")
        im = im.crop((left, top, left + side, top + side))

    if im.size[0] != TARGET:
        how = "down" if im.size[0] > TARGET else "up"
        print(f"    scaling {how} to {TARGET}x{TARGET}")
        im = im.resize((TARGET, TARGET), Image.LANCZOS)

    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / f"{game_id}.png"
    im.save(dest, "PNG", optimize=True)
    print(f"    -> {dest} ({dest.stat().st_size // 1024} KB)")


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ages", type=Path, help="art for the Oracle of Ages panel")
    ap.add_argument("--seasons", type=Path, help="art for the Oracle of Seasons panel")
    ap.add_argument("--local", action="store_true",
                    help="write to build/covers/ (gitignored) instead of art/covers/")
    args = ap.parse_args(argv[1:])

    if not args.ages and not args.seasons:
        ap.print_help()
        return 2

    dest_dir = (root / "build" / "covers") if args.local else (root / "art" / "covers")
    print(f"Installing into {dest_dir}"
          f"{'  (gitignored)' if args.local else '  (committed)'}\n")

    for key, source in (("ages", args.ages), ("seasons", args.seasons)):
        if not source:
            continue
        if not source.is_file():
            print(f"  {source}: not found", file=sys.stderr)
            return 1
        install(source, GAME_IDS[key], dest_dir)

    if not args.local:
        print("\nThis directory is committed, so only put art the project can")
        print("redistribute here -- see art/covers/README.md. Use --local for")
        print("anything derived from the games' own art.")
    print("\nRestart the launcher to see it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
