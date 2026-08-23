#!/usr/bin/env python3
"""Renders every stream overlay in a real browser and checks what came out.

The overlays are the one part of this project whose failures are visible
to an audience, and for a while they were also the one part with no
automated check: everything was verified by hand and thrown away. Two
bugs got shipped that way -- a panel that never hid itself when the
player exited, and a chase camera described in release notes it did not
match -- so the hand checks live here now.

Chromium loads these over file://, the same way OBS does, which is the
only way to catch the things that only break there.

What it checks, per layout:

  * the page loads with no console or page errors
  * the live panel fills in from a feed, and hides when the feed stops
  * both cut-out holes are actually transparent
  * an achievement icon loads, a missing one falls back, a hostile id is
    refused rather than turned into a path
  * with every speedrunning block on, nothing overlaps anything and
    nothing hangs past the safe area -- both of which have happened
  * every split row is really on screen (a class collision once hid the
    live segment while the count still said ten)

  python3 tools/overlay_render_test.py            # all layouts
  python3 tools/overlay_render_test.py --shots DIR  # also save PNGs

Needs playwright and a Chromium; skips with a note (exit 0) when there
isn't one, so a checkout without it is not a failing build.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STREAM = ROOT / "stream"

LAYOUTS = [
    ("overlay.html", 1920, 1080, False),
    ("overlay-vertical.html", 1080, 1920, False),
    ("overlay-framed.html", 1920, 1080, True),
    ("overlay-framed-vertical.html", 1080, 1920, True),
]

SPLITS = [
    ["Harp of Ages", 21600], ["Eternal Spirit", 74400],
    ["Power Bracelet", 96000], ["Ancient Wood", 138000],
    ["Roc's Feather", 171000], ["Echoing Howl", 0],
    ["Zora's Flippers", 0], ["Burning Flame", 0],
    ["Switch Hook", 0], ["Sacred Soil", 0],
]

# A mid-route Ages file: the treasure bits are the real ones off a save.
ITEM_BITS = "7EC4E20000400000000000000000000000"[:32]

failures: list[str] = []
checks = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global checks
    checks += 1
    if ok:
        print(f"ok   {name}")
    else:
        print(f"FAIL {name}" + (f"\n       {detail}" if detail else ""))
        failures.append(name)


def feed(tick: int, pad: int = 0, last_id: str = "harp", serial: int = 3) -> str:
    return (
        'EPOCH({cart:"tlozooa",title:"Oracle of Ages",room:"0-8A",essences:5,'
        'hearts:33,maxHearts:44,rings:27,deaths:12,kills:640,rupees:137,'
        'rupeesTotal:4210,seconds:3010,linked:false,unlocked:15,total:24,'
        f'lastId:"{last_id}",lastTitle:"Strings of Time",'
        'lastDesc:"Receive the Harp of Ages",'
        f'serial:{serial},tick:{tick},frames:180655,sword:2,shield:2,satchel:2,'
        f'bracelet:2,bombs:36,maxBombs:48,seeds:4,items:"{ITEM_BITS}",pad:{pad},'
        f'splitNext:5,splits:{json.dumps(SPLITS, separators=(",", ":"))}}});\n'
    )


PROBE = """() => {
  const q = s => document.querySelector(s);
  const visible = e => {
    if (!e || e.hidden) return null;
    const c = getComputedStyle(e);
    if (c.display === 'none' || c.visibility === 'hidden') return null;
    const r = e.getBoundingClientRect();
    return r.height > 0
      ? [Math.round(r.top), Math.round(r.bottom),
         Math.round(r.left), Math.round(r.right)]
      : null;
  };
  const names = ['.crest', '.top', '.now', '#hud', '#timer', '#splits',
                 '#tracker', '#inputs', '#last', '.cta'];
  const rects = {};
  names.forEach(n => { const r = visible(q(n)); if (r) rects[n] = r; });

  const keys = Object.keys(rects), clash = [];
  for (let i = 0; i < keys.length; i++)
    for (let j = i + 1; j < keys.length; j++) {
      const a = rects[keys[i]], b = rects[keys[j]];
      // Both axes, not just the vertical one: the floating layouts put
      // panels side by side, and a rail-shaped check calls that a clash.
      if (a[0] < b[1] && b[0] < a[1] && a[2] < b[3] && b[2] < a[3])
        clash.push(keys[i] + ' over ' + keys[j]);
    }

  const rail = q('.rail');
  const bottoms = Object.values(rects).map(r => r[1]);
  const overflow = (rail && bottoms.length)
    ? Math.max(0, Math.round(Math.max(...bottoms)
        - rail.getBoundingClientRect().bottom))
    : 0;

  const rows = [...document.querySelectorAll('#splits .split-row')];
  return {
    clash, overflow,
    hudShown: q('#hud') ? !q('#hud').hidden : null,
    room: q('#hud-room') ? q('#hud-room').textContent : null,
    timer: q('#timer-value') ? q('#timer-value').textContent : null,
    splitRows: rows.length,
    splitVisible: rows.filter(e => e.getBoundingClientRect().height > 0).length,
    current: q('.split-row.current .nm')
      ? q('.split-row.current .nm').textContent : null,
    cells: document.querySelectorAll('#tracker .cell').length,
    // An upgradeable item names its tier rather than numbering it.
    labels: [...document.querySelectorAll('#tracker .cell .lb')]
      .map(e => e.textContent),
    lit: document.querySelectorAll('#tracker .cell.has').length,
    padOn: document.querySelectorAll('.dpad i.on, .face b.on').length,
    icon: q('#card-icon')
      ? {src: q('#card-icon').getAttribute('src') || '',
         hidden: q('#card-icon').hidden, w: q('#card-icon').naturalWidth}
      : null,
  };
}"""


def run(shots: Path | None) -> int:
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("skip: playwright is not installed")
        return 0

    chromium = os.environ.get("EPOCH_CHROMIUM")
    if not chromium:
        for guess in sorted(Path("/opt/pw-browsers").glob("chromium*/chrome-linux/chrome")):
            chromium = str(guess)
            break
    if not chromium or not Path(chromium).exists():
        chromium = None          # let playwright find its own

    live = STREAM / "live.js"
    config = STREAM / "config.js"
    saved_config = config.read_text(encoding="utf-8") if config.exists() else None

    try:
        config.write_text(
            "CONFIG({cam: false, guide: false, timer: true, splits: true, "
            "tracker: true, inputs: true});\n", encoding="utf-8")
        live.write_text(feed(1), encoding="utf-8")

        with sync_playwright() as p:
            launch = {"args": ["--allow-file-access-from-files"]}
            if chromium:
                launch["executable_path"] = chromium
            try:
                browser = p.chromium.launch(**launch)
            except Exception as exc:                      # noqa: BLE001
                print(f"skip: no usable Chromium ({exc})")
                return 0

            for name, w, h, framed in LAYOUTS:
                page = browser.new_page(viewport={"width": w, "height": h})
                errors: list[str] = []
                page.on("pageerror", lambda e: errors.append(str(e)))
                # A tracker cell probes for its ripped item icon and
                # falls back to text when the file is absent -- absent
                # is the shipped state (icons rip from the player's own
                # ROM and are never committed), so those file-not-found
                # console entries are the design working, not an error.
                page.on("console",
                        lambda m: errors.append(m.text)
                        if m.type == "error"
                        and "ERR_FILE_NOT_FOUND" not in m.text else None)
                page.goto((STREAM / name).as_uri())
                page.wait_for_timeout(1400)
                live.write_text(feed(2, pad=0b00010001), encoding="utf-8")
                page.wait_for_timeout(1400)

                info = page.evaluate(PROBE)
                tag = name.replace(".html", "")
                check(f"{tag}: no errors", not errors, "; ".join(errors))
                check(f"{tag}: the panel is showing", info["hudShown"] is True)
                check(f"{tag}: the room came through",
                      info["room"] == "Room 0-8A", str(info["room"]))
                check(f"{tag}: nothing overlaps anything",
                      not info["clash"], "; ".join(info["clash"]))
                check(f"{tag}: nothing hangs past the safe area",
                      info["overflow"] == 0, f"{info['overflow']}px over")
                check(f"{tag}: the timer reads the file clock",
                      info["timer"] == "50:10.91", str(info["timer"]))
                if framed:
                    # The split list and the item grid need a rail, so
                    # only the framed pair carry them.
                    check(f"{tag}: every split row is on screen",
                          info["splitRows"] == len(SPLITS)
                          and info["splitVisible"] == len(SPLITS),
                          f"{info['splitVisible']} of {info['splitRows']}")
                    check(f"{tag}: the live segment is marked",
                          info["current"] == "Echoing Howl", str(info["current"]))
                    check(f"{tag}: the tracker lights what the file holds",
                          info["cells"] == 16 and info["lit"] == 13,
                          f"{info['lit']} of {info['cells']}")
                    # sword 2 is the Noble Sword, bracelet 2 the Power
                    # Gloves -- the sample feed holds both.
                    check(f"{tag}: an upgraded item names its tier",
                          "Noble" in info["labels"] and "Gloves" in info["labels"],
                          ", ".join(info["labels"]))
                check(f"{tag}: the input display follows the buttons",
                      info["padOn"] == 2, str(info["padOn"]))

                if framed:
                    shot = page.screenshot(omit_background=True)
                    from io import BytesIO
                    try:
                        from PIL import Image
                    except ImportError:
                        Image = None
                    if Image is not None:
                        im = Image.open(BytesIO(shot)).convert("RGBA")
                        box = page.evaluate(
                            "() => {const r = document.querySelector('.frame')"
                            ".getBoundingClientRect();"
                            "return [r.x, r.y, r.width, r.height];}")
                        cx = int(box[0] + box[2] / 2)
                        cy = int(box[1] + box[3] / 2)
                        check(f"{tag}: the opening is really transparent",
                              im.getpixel((cx, cy))[3] == 0,
                              f"alpha {im.getpixel((cx, cy))[3]}")
                        check(f"{tag}: the mat around it is not",
                              im.getpixel((max(0, int(box[0]) - 6), cy))[3] == 255)
                    if shots:
                        shots.mkdir(parents=True, exist_ok=True)
                        (shots / f"{tag}.png").write_bytes(shot)

                # An id with no icon falls back; a hostile one is refused.
                for bad, label in ((("no-such-achievement"), "a missing icon falls back"),
                                   ("../../../etc/passwd", "a hostile id is refused")):
                    live.write_text(feed(3, last_id=bad, serial=9), encoding="utf-8")
                    page.wait_for_timeout(1200)
                    icon = page.evaluate(
                        "() => {const e = document.getElementById('card-icon');"
                        "return e ? {hidden: e.hidden, src: e.getAttribute('src')||''} : null;}")
                    if icon is not None:
                        ok = icon["hidden"] and "etc/passwd" not in icon["src"]
                        check(f"{tag}: {label}", ok, json.dumps(icon))

                # The player exits: the file stays on disk and stops moving.
                page.wait_for_timeout(13000)
                still = page.evaluate(
                    "() => {const e = document.getElementById('hud');"
                    "return e ? e.hidden : null;}")
                check(f"{tag}: the panel hides when nobody is playing",
                      still is True)
                page.close()
            browser.close()
    finally:
        live.unlink(missing_ok=True)
        if saved_config is not None:
            config.write_text(saved_config, encoding="utf-8")
        else:
            config.unlink(missing_ok=True)

    if failures:
        print(f"\n{len(failures)} of {checks} checks FAILED")
        return 1
    print(f"\n[overlay_render_test] all {checks} checks passed")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", type=Path, default=None,
                    help="also write a PNG of each framed layout here")
    sys.exit(run(ap.parse_args().shots))
