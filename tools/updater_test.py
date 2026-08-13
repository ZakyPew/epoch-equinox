#!/usr/bin/env python3
"""Checks for the launcher's self-updater. No network, no pytest:

    python3 tools/updater_test.py

The interesting half is install(): it writes over a real folder, so the
tests build a fake release archive and a fake install with a save and a mod
in it, then assert the save and the mod are still there afterwards.
"""

from __future__ import annotations

import io
import json
import os
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "launcher"))

import updater  # noqa: E402

FAILURES: list[str] = []


def check(label: str, got, want) -> None:
    if got != want:
        FAILURES.append(f"{label}: got {got!r}, wanted {want!r}")


def check_raises(label: str, exc_type, fn) -> None:
    try:
        fn()
    except exc_type:
        return
    except Exception as exc:  # noqa: BLE001
        FAILURES.append(f"{label}: raised {type(exc).__name__}, wanted {exc_type.__name__}")
        return
    FAILURES.append(f"{label}: raised nothing, wanted {exc_type.__name__}")


# --------------------------------------------------------------------------
# versions
# --------------------------------------------------------------------------


def test_versions() -> None:
    check("parse v-prefix", updater.parse_version("v0.3.0"), (0, 3, 0))
    check("parse bare", updater.parse_version("1.2.3"), (1, 2, 3))
    check("parse pre-release", updater.parse_version("0.4.0-rc1"), (0, 4, 0))
    check("parse rubbish", updater.parse_version("nightly"), None)
    check("parse empty", updater.parse_version(""), None)

    check("newer patch", updater.is_newer("v0.3.1", "0.3.0"), True)
    check("newer minor", updater.is_newer("v0.4.0", "0.3.9"), True)
    check("newer major", updater.is_newer("v1.0.0", "0.9.9"), True)
    check("same", updater.is_newer("v0.3.0", "0.3.0"), False)
    check("older", updater.is_newer("v0.2.0", "0.3.0"), False)
    # (0, 4) and (0, 4, 0) are the same release written two ways.
    check("short equal", updater.is_newer("v0.4", "0.4.0"), False)
    check("short newer", updater.is_newer("v0.4", "0.3.0"), True)
    # A launcher on an unrecognisable build sits still rather than guessing.
    check("unparseable current", updater.is_newer("v9.9.9", "dev"), False)
    check("unparseable candidate", updater.is_newer("nightly", "0.3.0"), False)


# --------------------------------------------------------------------------
# release payloads
# --------------------------------------------------------------------------


def release(tag: str, names: list[str]) -> dict:
    return {
        "tag_name": tag,
        "body": "notes for " + tag,
        "html_url": f"https://github.com/{updater.REPO}/releases/tag/{tag}",
        "assets": [
            {"name": n, "browser_download_url": f"https://example.invalid/{n}", "size": 10}
            for n in names
        ],
    }


ASSETS = [
    "epoch-equinox-linux-x64.tar.gz",
    "epoch-equinox-windows-x64.zip",
    "epoch-equinox-macos-arm64.tar.gz",
]


def test_release_reading() -> None:
    suffix = updater.platform_asset_suffix()
    if suffix is None:
        print("[updater_test] no release asset for this platform; skipping asset checks")
        return

    got = updater.update_from_release(release("v0.9.0", ASSETS), "0.3.0")
    check("newer release offered", got is not None, True)
    if got is not None:
        check("version stripped of v", got.version, "0.9.0")
        check("asset matches platform", got.asset_name.endswith(suffix), True)
        check("notes carried", got.notes, "notes for v0.9.0")

    check(
        "same version offers nothing",
        updater.update_from_release(release("v0.3.0", ASSETS), "0.3.0"),
        None,
    )
    check(
        "older version offers nothing",
        updater.update_from_release(release("v0.2.0", ASSETS), "0.3.0"),
        None,
    )
    # A release that built only the other platform is not an update here.
    other = [a for a in ASSETS if not a.endswith(suffix)]
    check(
        "no asset for this platform",
        updater.update_from_release(release("v0.9.0", other), "0.3.0"),
        None,
    )
    check(
        "no assets at all",
        updater.update_from_release(release("v0.9.0", []), "0.3.0"),
        None,
    )


# --------------------------------------------------------------------------
# install
# --------------------------------------------------------------------------


def make_release_tar(path: Path, root: str, files: dict[str, str]) -> None:
    with tarfile.open(path, "w:gz") as tf:
        for name, text in files.items():
            data = text.encode()
            info = tarfile.TarInfo(f"{root}/{name}")
            info.size = len(data)
            info.mode = 0o755 if "/" not in name else 0o644
            tf.addfile(info, io.BytesIO(data))


def make_install(root: Path) -> None:
    """A folder as a player would have it: the old build, plus their stuff."""
    (root / "roms").mkdir(parents=True)
    (root / "mods" / "my-mod").mkdir(parents=True)
    (root / "covers").mkdir(parents=True)
    (root / "launcher").mkdir(parents=True)
    (root / "epoch").write_text("old player")
    (root / "launcher" / "epoch_launcher.py").write_text("old launcher")
    (root / "README.md").write_text("old readme")
    (root / "roms" / "tlozooa.gbc").write_text("the player's own cart")
    (root / "mods" / "my-mod" / "patch.ips").write_text("hand-made mod")
    (root / "covers" / "ages.png").write_text("custom art")
    (root / "tlozooa.sav").write_text("120 hours of save")


def test_install_preserves_player_files() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        install_dir = tmp / "install"
        make_install(install_dir)

        archive = tmp / "epoch-equinox-linux-x64.tar.gz"
        make_release_tar(
            archive,
            "epoch-equinox-linux-x64",
            {
                "epoch": "new player",
                "README.md": "new readme",
                "launcher/epoch_launcher.py": "new launcher",
                "launcher/updater.py": "brand new file",
                # The archives ship these folders with only a note in them.
                "roms/README.txt": "drop your ROMs here",
                "covers/ages.png": "shipped art",
            },
        )
        updater.install(archive, install_dir)

        # The build is replaced...
        check("player replaced", (install_dir / "epoch").read_text(), "new player")
        check("readme replaced", (install_dir / "README.md").read_text(), "new readme")
        check(
            "launcher replaced",
            (install_dir / "launcher" / "epoch_launcher.py").read_text(),
            "new launcher",
        )
        check(
            "new file added",
            (install_dir / "launcher" / "updater.py").read_text(),
            "brand new file",
        )
        # ...and everything the player owns is untouched.
        check(
            "ROM kept",
            (install_dir / "roms" / "tlozooa.gbc").read_text(),
            "the player's own cart",
        )
        check(
            "mod kept",
            (install_dir / "mods" / "my-mod" / "patch.ips").read_text(),
            "hand-made mod",
        )
        check("save kept", (install_dir / "tlozooa.sav").read_text(), "120 hours of save")
        # covers/ is preserved wholesale, so custom art wins over shipped art.
        check(
            "custom cover kept",
            (install_dir / "covers" / "ages.png").read_text(),
            "custom art",
        )
        check(
            "no ROM note written into roms/",
            (install_dir / "roms" / "README.txt").exists(),
            False,
        )
        # Staging is cleaned up whatever happens.
        leftovers = [p.name for p in install_dir.iterdir() if p.name.startswith(".update-")]
        check("staging cleaned", leftovers, [])


def test_install_survives_a_locked_executable() -> None:
    """Windows will not overwrite the running launcher; it renames instead.

    os.replace is forced to fail the way Windows fails, to prove the
    rename-aside path leaves a working new build behind.
    """
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        install_dir = tmp / "install"
        make_install(install_dir)
        archive = tmp / "epoch-equinox-linux-x64.tar.gz"
        make_release_tar(archive, "epoch-equinox-linux-x64", {"epoch": "new player"})

        real_replace = os.replace
        locked = install_dir / "epoch"
        state = {"refused": False}

        def fussy_replace(src, dst, *a, **kw):
            if Path(dst) == locked and not state["refused"]:
                state["refused"] = True          # refuse once, as Windows would
                raise PermissionError(32, "in use")
            return real_replace(src, dst, *a, **kw)

        updater.os.replace = fussy_replace
        try:
            updater.install(archive, install_dir)
        finally:
            updater.os.replace = real_replace

        check("locked file was refused once", state["refused"], True)
        check("new player installed anyway", locked.read_text(), "new player")
        retired = install_dir / ("epoch" + updater.OLD_SUFFIX)
        check("old player retired", retired.exists(), True)
        check("old player content", retired.read_text(), "old player")

        check("sweep removes it", updater.sweep_old_files(install_dir), 1)
        check("retired file gone", retired.exists(), False)
        check("player still there", locked.read_text(), "new player")


def test_install_refuses_escaping_archives() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        install_dir = tmp / "install"
        install_dir.mkdir()

        evil_tar = tmp / "evil.tar.gz"
        with tarfile.open(evil_tar, "w:gz") as tf:
            data = b"pwned"
            info = tarfile.TarInfo("../escaped.txt")
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
        check_raises(
            "tar escaping the folder", updater.UpdateError,
            lambda: updater.install(evil_tar, install_dir),
        )

        evil_zip = tmp / "evil.zip"
        with zipfile.ZipFile(evil_zip, "w") as zf:
            zf.writestr("../escaped.txt", "pwned")
        check_raises(
            "zip escaping the folder", updater.UpdateError,
            lambda: updater.install(evil_zip, install_dir),
        )

        check("nothing escaped", (tmp / "escaped.txt").exists(), False)

        check_raises(
            "unknown archive kind", updater.UpdateError,
            lambda: updater.install(tmp / "release.rar", install_dir),
        )


# --------------------------------------------------------------------------
# the real thing, served locally
# --------------------------------------------------------------------------

# Trimmed from the actual api.github.com response for v0.3.0 -- real field
# names, real asset names, and a few fields we do not read, so that parsing
# is tested against GitHub's shape rather than against a tidied-up guess.
REAL_RELEASE = {
    "tag_name": "v0.3.0",
    "target_commitish": "main",
    "name": "v0.3.0",
    "draft": False,
    "prerelease": False,
    "id": 368046331,
    "html_url": "https://github.com/ZakyPew/epoch-equinox/releases/tag/v0.3.0",
    "body": "Prebuilt player. **No game data included**",
    "assets": [
        {
            "id": 509290562,
            "name": "epoch-equinox-linux-x64.tar.gz",
            "state": "uploaded",
            "content_type": "application/gzip",
            "size": 9355960,
            "download_count": 0,
            "browser_download_url":
                "https://github.com/ZakyPew/epoch-equinox/releases/download/"
                "v0.3.0/epoch-equinox-linux-x64.tar.gz",
        },
        {
            "id": 509290564,
            "name": "epoch-equinox-windows-x64.zip",
            "state": "uploaded",
            "content_type": "application/zip",
            "size": 53617839,
            "download_count": 0,
            "browser_download_url":
                "https://github.com/ZakyPew/epoch-equinox/releases/download/"
                "v0.3.0/epoch-equinox-windows-x64.zip",
        },
    ],
}


def test_real_release_payload() -> None:
    if updater.platform_asset_suffix() is None:
        return
    got = updater.update_from_release(REAL_RELEASE, "0.2.0")
    check("real payload parsed", got is not None, True)
    if got is not None:
        check("real version", got.version, "0.3.0")
        check("real tag", got.tag, "v0.3.0")
        check("real asset url is github", got.asset_url.startswith("https://github.com/"), True)
        check("real size", got.asset_size > 0, True)
    check(
        "real payload is not an update for itself",
        updater.update_from_release(REAL_RELEASE, "0.3.0"),
        None,
    )


def test_end_to_end_over_a_local_server() -> None:
    """check() -> download() -> install(), against a server on localhost.

    The live API is not reachable from a test run, but the code path that
    matters -- urllib, the JSON, the streaming download, the unpack -- is
    the same one. Only the hostname differs.
    """
    suffix = updater.platform_asset_suffix()
    if suffix is None:
        return

    import http.server
    import threading

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        asset_name = "epoch-equinox" + suffix
        archive = tmp / asset_name
        if suffix.endswith(".zip"):
            player_name = "epoch.exe"
            with zipfile.ZipFile(archive, "w") as zf:
                zf.writestr(
                    "epoch-equinox-windows-x64/epoch.exe",
                    "served player",
                )
        else:
            player_name = "epoch"
            make_release_tar(
                archive,
                "epoch-equinox-linux-x64",
                {player_name: "served player"},
            )
        payload = archive.read_bytes()

        install_dir = tmp / "install"
        make_install(install_dir)

        seen: dict[str, int] = {}

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):  # keep the test output clean
                pass

            def do_GET(self):
                seen[self.path] = seen.get(self.path, 0) + 1
                if self.path == "/releases/latest":
                    body = json.dumps({
                        "tag_name": "v9.9.9",
                        "body": "served notes",
                        "html_url": "http://127.0.0.1/releases/v9.9.9",
                        "assets": [{
                            "name": asset_name,
                            "size": len(payload),
                            "browser_download_url": f"http://{self.headers['Host']}/asset",
                        }],
                    }).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                elif self.path == "/asset":
                    self.send_response(200)
                    self.send_header("Content-Type", "application/gzip")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                else:
                    self.send_error(404)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        host = f"127.0.0.1:{server.server_port}"

        api_before = updater.RELEASES_API
        no_proxy_before = os.environ.get("no_proxy")
        updater.RELEASES_API = f"http://{host}/releases/latest"
        os.environ["no_proxy"] = "127.0.0.1,localhost"
        try:
            found = updater.check("0.3.0", timeout=10.0)
            check("served release found", found is not None, True)
            if found is None:
                return
            check("served version", found.version, "9.9.9")

            ticks: list[tuple[int, int]] = []
            got = updater.download(found, tmp / "dl", progress=lambda d, t: ticks.append((d, t)))
            check("downloaded intact", got.read_bytes(), payload)
            check("progress reported", len(ticks) > 0, True)
            check("progress ends at the total", ticks[-1][0], len(payload))

            updater.install(got, install_dir)
            check(
                "installed from download",
                (install_dir / player_name).read_text(),
                "served player",
            )
            check("save survived the round trip",
                  (install_dir / "tlozooa.sav").read_text(), "120 hours of save")
            check("both endpoints were hit", sorted(seen), ["/asset", "/releases/latest"])
        finally:
            updater.RELEASES_API = api_before
            if no_proxy_before is None:
                os.environ.pop("no_proxy", None)
            else:
                os.environ["no_proxy"] = no_proxy_before
            server.shutdown()
            server.server_close()


def test_download_cancels_cleanly() -> None:
    """A cancelled download leaves nothing half-written behind."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        update = updater.Update(
            tag="v9.9.9", version="9.9.9", notes="", page_url="",
            asset_name="epoch-equinox-linux-x64.tar.gz",
            asset_url="http://127.0.0.1:1/never", asset_size=1,
        )
        check_raises(
            "unreachable host is an UpdateError", updater.UpdateError,
            lambda: updater.download(update, tmp / "dl", timeout=2.0),
        )
        leftovers = list((tmp / "dl").glob("*")) if (tmp / "dl").exists() else []
        check("no partial file left", leftovers, [])


def test_preserved_names() -> None:
    for name in ("roms", "mods", "covers", "saves", "tlozooa.sav", "room.voxel"):
        check(f"{name} preserved", updater.is_preserved(name), True)
    for name in ("epoch", "epoch.exe", "launcher", "README.md", "covers.png"):
        check(f"{name} not preserved", updater.is_preserved(name), False)


def main() -> int:
    test_versions()
    test_release_reading()
    test_install_preserves_player_files()
    test_install_survives_a_locked_executable()
    test_install_refuses_escaping_archives()
    test_real_release_payload()
    test_end_to_end_over_a_local_server()
    test_download_cancels_cleanly()
    test_preserved_names()

    if FAILURES:
        print(f"[updater_test] {len(FAILURES)} check(s) failed:")
        for failure in FAILURES:
            print(f"  - {failure}")
        return 1
    print("[updater_test] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
