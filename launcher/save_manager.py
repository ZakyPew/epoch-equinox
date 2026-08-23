"""Import, export and back up the games' battery saves.

The runner keeps each game's save as '<ROM header title>.sav' beside the
binary -- the platform layer derives that name from the cartridge title
bytes, so this module derives it the same way and lands imports under a
name the runner will actually load, even before the game has ever saved.

Every operation that replaces a save writes a timestamped backup into
save-backups/ first, so nothing here can lose a file: an import you
regret is one restore away, and the restore itself backs up what it
replaces.

No Qt in here on purpose: the launcher dialog is a thin skin over these
functions, and tools/save_manager_test.py exercises them headlessly
against the real endgame saves in tests/saves.
"""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import oracle_secrets

# What the Oracle carts battery-back. Emulators sometimes pad the file or
# append an RTC footer; anything at least this long carries a whole save.
SAVE_SIZE = 0x2000

BACKUP_DIR = "save-backups"

# game id -> the name oracle_secrets gives slots parsed from that cart.
GAME_OF_ID = {"tlozooa": "ages", "tlozoos": "seasons"}
TITLE_OF_GAME = {"ages": "Oracle of Ages", "seasons": "Oracle of Seasons"}


class SaveError(Exception):
    """A save operation that must not proceed, with a reason to show."""


@dataclass
class Verdict:
    """What a candidate file turned out to hold."""

    slots: list                    # oracle_secrets.SaveFile, possibly empty
    games: set[str]                # {"ages"}, {"seasons"}, both, or empty

    @property
    def empty(self) -> bool:
        return not self.slots


def rom_save_path(root: Path, rom_relpath: str) -> Path | None:
    """The .sav path the runner will use, derived the way it derives it:
    the cartridge title bytes, cut at the first character that is not
    printable ASCII (the platform layer zeroes those, and its copy stops
    at the first zero)."""
    rom = root / rom_relpath
    try:
        with open(rom, "rb") as f:
            f.seek(0x134)
            raw = f.read(16)
    except OSError:
        return None
    if len(raw) < 16:
        return None
    title = ""
    for b in raw:
        if not 0x20 <= b <= 0x7E:
            break
        title += chr(b)
    if not title:
        return None
    return root / f"{title}.sav"


def current_save(root: Path, game_id: str,
                 rom_relpath: str | None = None) -> Path | None:
    """The game's save beside the binary, if one exists yet."""
    found = oracle_secrets.find_saves(root).get(game_id)
    if found is not None:
        return found
    # No save yet: the ROM still tells us where one would go.
    if rom_relpath:
        derived = rom_save_path(root, rom_relpath)
        if derived is not None and derived.exists():
            return derived
    return None


def inspect(path: Path) -> Verdict:
    """Parse a candidate .sav the way the secrets code already does."""
    try:
        slots = oracle_secrets.read_save(path)
    except OSError as exc:
        raise SaveError(f"Could not read {path.name}: {exc}") from exc
    return Verdict(slots=slots, games={s.game for s in slots})


def check_import(source: Path, game_id: str) -> Verdict:
    """Everything that can be known about an import before it happens.

    Raises SaveError for a file that must not be imported; returns a
    Verdict whose .empty flags a file the caller should confirm (no
    recognizable file slots -- a fresh save, or not a save at all).
    """
    want = GAME_OF_ID.get(game_id)
    if want is None:
        raise SaveError(f"Unknown game id {game_id!r}.")
    try:
        size = source.stat().st_size
    except OSError as exc:
        raise SaveError(f"Could not read {source.name}: {exc}") from exc
    if size < SAVE_SIZE:
        raise SaveError(
            f"{source.name} is {size} bytes; a whole save is at least "
            f"{SAVE_SIZE}. This file is truncated."
        )
    verdict = inspect(source)
    if verdict.games and want not in verdict.games:
        held = ", ".join(sorted(TITLE_OF_GAME[g] for g in verdict.games))
        raise SaveError(
            f"{source.name} holds {held} files, not "
            f"{TITLE_OF_GAME[want]}. Import it from the other game's page."
        )
    return verdict


def _backup_name(game_id: str) -> str:
    return f"{game_id}-{datetime.now():%Y%m%d-%H%M%S}.sav"


def backup(root: Path, game_id: str, save_path: Path) -> Path:
    """Copy the save into save-backups/, never overwriting an earlier one."""
    folder = root / BACKUP_DIR
    folder.mkdir(exist_ok=True)
    base = _backup_name(game_id)
    dest = folder / base
    n = 1
    while dest.exists():                     # two backups in one second
        dest = folder / f"{base[:-4]}.{n}.sav"
        n += 1
    shutil.copy2(save_path, dest)
    return dest


def list_backups(root: Path, game_id: str) -> list[Path]:
    """This game's backups, newest first."""
    folder = root / BACKUP_DIR
    if not folder.is_dir():
        return []
    return sorted(folder.glob(f"{game_id}-*.sav"),
                  key=lambda p: p.name, reverse=True)


def _place(source: Path, dest: Path) -> None:
    """Copy into place atomically: a crash mid-copy must not leave the
    runner a half-written save to load."""
    tmp = dest.with_name(dest.name + ".tmp")
    shutil.copy2(source, tmp)
    os.replace(tmp, dest)


def import_save(root: Path, game_id: str, rom_relpath: str | None,
                source: Path) -> tuple[Path, Path | None]:
    """Put source in place as the game's save. Returns (save path,
    backup path or None). Validation is check_import's job -- callers run
    it first so a confirmation can sit between the two."""
    dest = current_save(root, game_id, rom_relpath)
    backed = None
    if dest is not None and dest.exists():
        backed = backup(root, game_id, dest)
    elif rom_relpath:
        dest = rom_save_path(root, rom_relpath)
    if dest is None:
        raise SaveError(
            "No save exists yet and the ROM is not installed, so there is "
            "no way to know the file name the game will load. Install the "
            "ROM first."
        )
    _place(source, dest)
    return dest, backed


def export_save(save_path: Path, dest: Path) -> None:
    shutil.copy2(save_path, dest)


def restore(root: Path, game_id: str, rom_relpath: str | None,
            backup_path: Path) -> tuple[Path, Path | None]:
    """A restore is an import whose source is one of our own backups."""
    if not backup_path.exists():
        raise SaveError(f"{backup_path.name} is gone.")
    return import_save(root, game_id, rom_relpath, backup_path)


def export_name(game_id: str) -> str:
    """A default file name for an exported copy."""
    return f"epoch-{game_id}-{datetime.now():%Y%m%d-%H%M}.sav"
