#!/usr/bin/env python3
"""One command into Veran's tower: the reproducible dungeon screenshot
route (issue #83).

Dungeon rooms use a different render job than the overworld -- a
15-column layout, a camera loose inside the room, HUD-parked sprites --
so camera, height, billboard and depth-ordering regressions show up
there first. This script makes those visible without manual searching:

    python3 tools/dungeon_shot.py

It stages the endgame save from tests/saves beside the vox_shot binary
(backing up and restoring whatever save was already there), boots the
Ages ROM headlessly, scripts the file select, and captures the entrance
of Veran's tower in flat, 45-degree and chase views. Before writing
anything it VERIFIES the state: the room dump must say group 04 room F3
with live terrain -- so a failure names the route or state that broke
instead of silently shooting an unrelated scene.

Output: <out>/dungeon-{flat,45,chase}.ppm (plus .png when Pillow is
around). The flat capture is the reference; the other two are what the
renderer made of the same frame.

Requires a built tree (build/vox_shot -- see docs/VOXEL_CONTRIBUTING.md)
and your own tlozooa ROM installed. No ROM or ROM-derived asset is ever
committed; the captures stay on your machine.

Why frame numbers work here: the route touches no combat before the
capture frame (the essence dialog is location-scripted, not timed), and
the boot path is deterministic to the frame -- verified by repeated
runs. Rooms beyond the first snake room drift (knockback timing), which
is exactly why the capture stops at the entrance.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SAVE = os.path.join(ROOT, "tests", "saves", "ages-veran-tower.sav")
SAVE_NAME = "ZELDA NAYRUAZ8E.sav"        # the runner's name for the Ages save
SCRIPT = "650:S:10,1050:S:10,1180:A:10,1320:A:10,1460:A:10"
FRAME = "1600"
EXPECT_ROOM = ("04", "F3")               # group, room -- Veran's tower entrance

MODES = {"45": "3", "chase": "4"}


def run_capture(tool, rom, mode, prefix, env):
    r = subprocess.run(
        [tool, rom, FRAME, mode, prefix, SCRIPT],
        cwd=os.path.dirname(tool), env=env,
        capture_output=True, text=True, timeout=900)
    return r.stderr + r.stdout


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", default=os.path.join(ROOT, "build", "vox_shot"))
    ap.add_argument("--rom", default=os.path.join("roms", "tlozooa.gbc"),
                    help="relative to the tool's folder, like the player")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "dungeon-route"))
    args = ap.parse_args(argv)

    if not os.path.exists(args.tool):
        print(f"vox_shot not found at {args.tool} -- build it first "
              "(see docs/VOXEL_CONTRIBUTING.md)", file=sys.stderr)
        return 1
    tooldir = os.path.dirname(os.path.abspath(args.tool))
    if not os.path.exists(os.path.join(tooldir, args.rom)):
        print(f"no ROM at {os.path.join(tooldir, args.rom)} -- install "
              "your own tlozooa.gbc first", file=sys.stderr)
        return 1

    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "offscreen")
    env["VOX_DUMP_ROOM"] = "1"

    os.makedirs(args.out, exist_ok=True)
    for stray in os.listdir(args.out):     # leftovers from a broken run
        if stray.startswith("cap-"):
            os.remove(os.path.join(args.out, stray))

    # Stage the known save; put the player's own back no matter what.
    target = os.path.join(tooldir, SAVE_NAME)
    backup = target + ".dungeon-route-backup"
    had_save = os.path.exists(target)
    if had_save:
        shutil.copy2(target, backup)
    shutil.copy2(SAVE, target)
    try:
        outputs = {}
        for label, mode in MODES.items():
            prefix = os.path.join(args.out, f"cap-{label}")
            log = run_capture(os.path.abspath(args.tool), args.rom,
                              mode, prefix, env)

            # The whole point: verify we shot the scene we claim.
            m = re.search(r"\[room\] group (\w+) room (\w+)", log)
            live = f"frame {FRAME}: terrain LIVE" in log
            if not m or (m.group(1), m.group(2)) != EXPECT_ROOM or not live:
                got = f"group {m.group(1)} room {m.group(2)}" if m \
                    else "no room dump at all"
                print(f"ROUTE BROKE ({label} view): expected group "
                      f"{EXPECT_ROOM[0]} room {EXPECT_ROOM[1]} with live "
                      f"terrain, got {got}."
                      f"\nThe boot script or the save changed -- see the "
                      f"route recipe in tools/vox_shot.c's header.",
                      file=sys.stderr)
                return 1
            outputs[label] = f"{prefix}-{FRAME}-vox.ppm"
            outputs.setdefault("flat", f"{prefix}-{FRAME}-flat.ppm")

        # Stable names, so diffs across runs line up.
        final = {}
        for label, src in outputs.items():
            dst = os.path.join(args.out, f"dungeon-{label}.ppm")
            shutil.move(src, dst)
            final[label] = dst
        for stray in os.listdir(args.out):
            if stray.startswith("cap-"):
                os.remove(os.path.join(args.out, stray))

        try:
            from PIL import Image
            for label, path in final.items():
                Image.open(path).save(path[:-4] + ".png")
        except ImportError:
            pass

        print(f"route verified: group {EXPECT_ROOM[0]} room "
              f"{EXPECT_ROOM[1]}, terrain live")
        for label in ("flat", "45", "chase"):
            print(f"  {final[label]}")
        return 0
    finally:
        if had_save:
            shutil.move(backup, target)
        elif os.path.exists(target):
            os.remove(target)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
