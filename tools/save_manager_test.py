#!/usr/bin/env python3
"""save_manager against the real endgame saves in tests/saves.

The module's whole promise is "nothing here can lose a file", so most of
what this checks is the backup trail: an import backs up what it
replaces, a restore backs up what *it* replaces, and both land byte-for-
byte copies. The refusals are checked with the same real files -- the
Seasons save refused on the Ages page is not a synthetic wrong-magic
blob, it is the actual mistake a person would make.

Run from anywhere: python3 tools/save_manager_test.py
"""

import shutil
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "launcher"))

import save_manager as sm  # noqa: E402

AGES_SAV = REPO / "tests/saves/ages-veran-tower.sav"
SEASONS_SAV = REPO / "tests/saves/seasons-room-of-rites.sav"

failures = 0


def check(cond, name):
    global failures
    print(("ok   " if cond else "FAIL ") + name)
    if not cond:
        failures += 1


def fake_rom(path: Path, title: bytes) -> None:
    """A ROM that is nothing but a header with a title in it."""
    data = bytearray(0x150)
    data[0x134:0x134 + len(title)] = title
    path.write_bytes(bytes(data))


def main() -> int:
    root = Path(tempfile.mkdtemp(prefix="save_manager_test_"))
    (root / "roms").mkdir()
    fake_rom(root / "roms/ages.gbc", b"ZELDA NAYRUAZ8E")
    fake_rom(root / "roms/seasons.gbc", b"ZELDA DINAZ7E\x80DE")

    # -- the derived name is the runner's name --------------------------------
    p = sm.rom_save_path(root, "roms/ages.gbc")
    check(p is not None and p.name == "ZELDA NAYRUAZ8E.sav",
          "the Ages save name comes out exactly as the runner writes it")
    p = sm.rom_save_path(root, "roms/seasons.gbc")
    check(p is not None and p.name == "ZELDA DINAZ7E.sav",
          "the title is cut at the first non-ASCII byte, like the runner")
    check(sm.rom_save_path(root, "roms/missing.gbc") is None,
          "a missing ROM derives nothing rather than raising")

    # -- import into an empty root -------------------------------------------
    verdict = sm.check_import(AGES_SAV, "tlozooa")
    check(not verdict.empty and verdict.games == {"ages"},
          "the real Ages save is recognized as Ages")
    dest, backed = sm.import_save(root, "tlozooa", "roms/ages.gbc", AGES_SAV)
    check(dest.name == "ZELDA NAYRUAZ8E.sav" and dest.exists(),
          "the first import lands under the runner's name")
    check(backed is None, "with nothing to replace, no backup is invented")
    check(dest.read_bytes() == AGES_SAV.read_bytes(),
          "and the bytes are the source's bytes")

    # -- the wrong game is refused, by content not by name --------------------
    on_ages_page = SEASONS_SAV.parent / SEASONS_SAV.name
    try:
        sm.check_import(on_ages_page, "tlozooa")
        check(False, "a Seasons save is refused on the Ages page")
    except sm.SaveError as exc:
        check("Seasons" in str(exc),
              "a Seasons save is refused on the Ages page")

    # -- truncation is refused ------------------------------------------------
    stub = root / "short.sav"
    stub.write_bytes(AGES_SAV.read_bytes()[:0x1000])
    try:
        sm.check_import(stub, "tlozooa")
        check(False, "a truncated file is refused")
    except sm.SaveError as exc:
        check("truncated" in str(exc), "a truncated file is refused")

    # -- unrecognizable content is flagged, not refused -----------------------
    blank = root / "blank.sav"
    blank.write_bytes(b"\xff" * sm.SAVE_SIZE)
    verdict = sm.check_import(blank, "tlozooa")
    check(verdict.empty,
          "a fresh or foreign save is flagged for confirmation instead")

    # -- replacing a save backs it up first -----------------------------------
    dest2, backed2 = sm.import_save(root, "tlozooa", "roms/ages.gbc", blank)
    check(dest2 == dest, "a second import replaces the same file")
    check(backed2 is not None and backed2.parent.name == sm.BACKUP_DIR,
          "and what it replaced went into save-backups/")
    check(backed2 is not None
          and backed2.read_bytes() == AGES_SAV.read_bytes(),
          "as a byte-for-byte copy")
    check(dest.read_bytes() == blank.read_bytes(),
          "while the new content is in place")

    # -- backups list newest first, and restore round-trips -------------------
    backs = sm.list_backups(root, "tlozooa")
    check(backs and backs[0] == backed2, "the backup shows up in the list")
    check(sm.list_backups(root, "tlozoos") == [],
          "under its own game only")
    dest3, backed3 = sm.restore(root, "tlozooa", "roms/ages.gbc", backed2)
    check(dest3.read_bytes() == AGES_SAV.read_bytes(),
          "restoring the backup brings the old save back")
    check(backed3 is not None
          and backed3.read_bytes() == blank.read_bytes(),
          "and the save it displaced got its own backup")
    check(len(sm.list_backups(root, "tlozooa")) == 2,
          "so the trail now has both")

    # -- two backups in one second do not collide -----------------------------
    third = sm.backup(root, "tlozooa", dest)
    fourth = sm.backup(root, "tlozooa", dest)
    check(third != fourth and third.exists() and fourth.exists(),
          "same-second backups get distinct names")

    # -- export is a plain copy ----------------------------------------------
    out = root / "exported.sav"
    sm.export_save(dest, out)
    check(out.read_bytes() == dest.read_bytes(), "an export is a plain copy")
    check(sm.export_name("tlozooa").startswith("epoch-tlozooa-"),
          "with a default name that says what it is")

    # -- no ROM, no save: the error explains itself ---------------------------
    bare = Path(tempfile.mkdtemp(prefix="save_manager_bare_"))
    try:
        sm.import_save(bare, "tlozooa", None, AGES_SAV)
        check(False, "importing with no ROM and no save explains itself")
    except sm.SaveError as exc:
        check("Install the ROM" in str(exc),
              "importing with no ROM and no save explains itself")

    # -- nothing litters the save folder --------------------------------------
    check(not list(root.glob("*.tmp")), "no temp files are left behind")

    shutil.rmtree(root)
    shutil.rmtree(bare)
    if failures:
        print(f"{failures} check(s) FAILED")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
