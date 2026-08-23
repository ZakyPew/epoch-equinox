"""Check GitHub for a newer release and install it over this one.

The releases carry a whole folder -- player, launcher, cover art -- and the
player keeps its ROMs, mods and saves in that same folder. So an update is a
*merge*, not a replace: anything the release ships is overwritten, and
anything the player put there is left alone (see PRESERVED).

Nothing here needs a network to import, and every entry point raises
UpdateError rather than surfacing urllib's exception zoo. The launcher's
startup check swallows that error entirely -- being offline is not a
problem worth a dialog.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

REPO = "ZakyPew/epoch-equinox"
RELEASES_API = f"https://api.github.com/repos/{REPO}/releases/latest"
RELEASES_PAGE = f"https://github.com/{REPO}/releases"
USER_AGENT = "epoch-equinox-launcher"

# Folders that belong to the player rather than to the build. The release
# archives contain empty versions of the first three; overwriting them with
# those would be harmless today, but skipping them outright means a future
# release that ships an example mod can never bury someone's own.
PRESERVED_DIRS = {"roms", "mods", "covers", "saves", "save-backups",
                  "screenshots"}
# Saves and sculpted rooms sit loose beside the binary.
PRESERVED_SUFFIXES = {".sav", ".srm", ".state", ".rtc", ".voxel"}

# A file that could not be overwritten in place gets renamed to this and
# swept away on the next start. Windows refuses to overwrite a running
# executable but is happy to rename one.
OLD_SUFFIX = ".old-update"

CHUNK = 64 * 1024


class UpdateError(Exception):
    """Anything that stopped an update from being checked or applied."""


@dataclass(frozen=True)
class Update:
    tag: str            # "v0.4.0"
    version: str        # "0.4.0"
    notes: str
    page_url: str
    asset_name: str
    asset_url: str
    asset_size: int


# --------------------------------------------------------------------------
# versions
# --------------------------------------------------------------------------


def parse_version(text: str) -> tuple[int, ...] | None:
    """"v0.3.0" -> (0, 3, 0). None if it is not a version at all.

    A pre-release suffix ("0.4.0-rc1") is dropped rather than ordered: the
    launcher only ever compares against `releases/latest`, which GitHub
    already excludes pre-releases from.
    """
    if not text:
        return None
    cleaned = text.strip().lstrip("vV").split("+")[0].split("-")[0]
    if not cleaned:
        return None
    parts = []
    for piece in cleaned.split("."):
        if not piece.isdigit():
            return None
        parts.append(int(piece))
    return tuple(parts) if parts else None


def is_newer(candidate: str, current: str) -> bool:
    """True when `candidate` is a release later than `current`.

    An unparseable version on either side means "no update": a launcher
    running an odd build should sit still rather than guess.
    """
    new = parse_version(candidate)
    old = parse_version(current)
    if new is None or old is None:
        return False
    # Compare (0, 4) against (0, 4, 0) on equal footing.
    width = max(len(new), len(old))
    new += (0,) * (width - len(new))
    old += (0,) * (width - len(old))
    return new > old


# --------------------------------------------------------------------------
# release lookup
# --------------------------------------------------------------------------


def platform_asset_suffix() -> str | None:
    """The release asset this platform can install, if any."""
    if sys.platform.startswith("win"):
        return "-windows-x64.zip"
    if sys.platform.startswith("linux"):
        return "-linux-x64.tar.gz"
    if sys.platform == "darwin":
        return "-macos-arm64.tar.gz"
    return None


def asset_for_platform(assets: Iterable[dict]) -> dict | None:
    suffix = platform_asset_suffix()
    if suffix is None:
        return None
    for asset in assets:
        if str(asset.get("name", "")).endswith(suffix):
            return asset
    return None


def update_from_release(release: dict, current_version: str) -> Update | None:
    """Read a GitHub release payload. None when it is nothing to install."""
    tag = str(release.get("tag_name") or "")
    if not is_newer(tag, current_version):
        return None
    asset = asset_for_platform(release.get("assets") or [])
    if asset is None:
        return None
    version = parse_version(tag)
    return Update(
        tag=tag,
        version=".".join(str(n) for n in version) if version else tag,
        notes=str(release.get("body") or "").strip(),
        page_url=str(release.get("html_url") or RELEASES_PAGE),
        asset_name=str(asset.get("name") or ""),
        asset_url=str(asset.get("browser_download_url") or ""),
        asset_size=int(asset.get("size") or 0),
    )


def check(current_version: str, timeout: float = 8.0) -> Update | None:
    """Ask GitHub for the latest release. None when already up to date."""
    request = urllib.request.Request(
        RELEASES_API,
        headers={"User-Agent": USER_AGENT, "Accept": "application/vnd.github+json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            release = json.load(response)
    except urllib.error.HTTPError as exc:
        raise UpdateError(f"GitHub answered {exc.code} for the release list") from exc
    except urllib.error.URLError as exc:
        raise UpdateError(f"Could not reach GitHub: {exc.reason}") from exc
    except (OSError, ValueError) as exc:
        raise UpdateError(f"Could not read the release list: {exc}") from exc
    if not isinstance(release, dict):
        raise UpdateError("The release list was not in the expected shape")
    return update_from_release(release, current_version)


# --------------------------------------------------------------------------
# download
# --------------------------------------------------------------------------


def download(
    update: Update,
    into: Path,
    progress: Callable[[int, int], None] | None = None,
    cancelled: Callable[[], bool] | None = None,
    timeout: float = 30.0,
) -> Path:
    """Stream the release asset into `into`. Returns the downloaded path.

    `progress` is called with (bytes so far, total bytes; 0 when unknown).
    `cancelled` is polled between chunks so the UI can back out of a
    half-finished download without leaving the file behind.
    """
    if not update.asset_url:
        raise UpdateError("That release has no downloadable file for this platform")
    into.mkdir(parents=True, exist_ok=True)
    target = into / update.asset_name
    request = urllib.request.Request(update.asset_url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            total = int(response.headers.get("Content-Length") or update.asset_size or 0)
            done = 0
            with open(target, "wb") as out:
                while True:
                    if cancelled is not None and cancelled():
                        raise UpdateError("Download cancelled")
                    chunk = response.read(CHUNK)
                    if not chunk:
                        break
                    out.write(chunk)
                    done += len(chunk)
                    if progress is not None:
                        progress(done, total)
    except UpdateError:
        target.unlink(missing_ok=True)
        raise
    except (urllib.error.URLError, OSError) as exc:
        target.unlink(missing_ok=True)
        raise UpdateError(f"Download failed: {exc}") from exc
    return target


# --------------------------------------------------------------------------
# install
# --------------------------------------------------------------------------


def is_preserved(name: str) -> bool:
    return name in PRESERVED_DIRS or Path(name).suffix.lower() in PRESERVED_SUFFIXES


def _safe_names(names: Iterable[str], kind: str) -> None:
    """Refuse archives that would write outside the folder we extract into."""
    for name in names:
        path = Path(name)
        if path.is_absolute() or ".." in path.parts:
            raise UpdateError(f"The {kind} contains an unsafe path: {name}")


def _extract(archive: Path, into: Path) -> None:
    try:
        if archive.name.endswith(".zip"):
            with zipfile.ZipFile(archive) as zf:
                _safe_names(zf.namelist(), "archive")
                zf.extractall(into)
        elif archive.name.endswith((".tar.gz", ".tgz")):
            with tarfile.open(archive, "r:gz") as tf:
                _safe_names(tf.getnames(), "archive")
                try:
                    tf.extractall(into, filter="data")
                except TypeError:  # filter= arrived in 3.12
                    tf.extractall(into)
        else:
            raise UpdateError(f"Cannot unpack {archive.name}")
    except UpdateError:
        raise
    except (OSError, zipfile.BadZipFile, tarfile.TarError) as exc:
        raise UpdateError(f"Could not unpack {archive.name}: {exc}") from exc


def _payload_root(staged: Path) -> Path:
    """The release archives hold one top-level folder; find it."""
    entries = [p for p in staged.iterdir() if not p.name.startswith(".")]
    if len(entries) == 1 and entries[0].is_dir():
        return entries[0]
    return staged


def _replace_file(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.replace(src, dest)
        return
    except PermissionError:
        pass
    # Windows holds a lock on the running executable. It cannot be
    # overwritten, but it can be renamed out of the way -- the new file then
    # takes its place and the stale one goes on the next start.
    retired = dest.with_name(dest.name + OLD_SUFFIX)
    try:
        retired.unlink(missing_ok=True)
    except OSError:
        pass
    try:
        os.replace(dest, retired)
        os.replace(src, dest)
    except OSError as exc:
        raise UpdateError(f"Could not replace {dest.name}: {exc}") from exc


def _merge_tree(src: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for item in sorted(src.iterdir()):
        if is_preserved(item.name):
            continue
        target = dest / item.name
        if item.is_dir():
            _merge_tree(item, target)
        else:
            _replace_file(item, target)


def install(archive: Path, install_dir: Path) -> None:
    """Unpack `archive` over `install_dir`, keeping the player's own files.

    Staging happens inside `install_dir` so the final moves stay on one
    filesystem -- an os.replace across devices fails, and a half-applied
    update is the one outcome worth going out of the way to avoid.
    """
    install_dir.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".update-", dir=install_dir))
    try:
        _extract(archive, staging)
        _merge_tree(_payload_root(staging), install_dir)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def sweep_old_files(install_dir: Path) -> int:
    """Delete executables retired by a previous update. Returns how many."""
    swept = 0
    for path in install_dir.rglob("*" + OLD_SUFFIX):
        try:
            path.unlink()
            swept += 1
        except OSError:
            pass  # still locked; next start will get it
    return swept
