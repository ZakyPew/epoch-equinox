#!/usr/bin/env python3
from __future__ import annotations

import json
import tempfile
from pathlib import Path

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launcher"))

from godot_backend import discover_godot_backend  # noqa: E402
from epoch_launcher import launcher_menu_items  # noqa: E402

assert "Start native Ages" not in launcher_menu_items()
assert "Native mods" not in launcher_menu_items()
assert "Start native Ages" in launcher_menu_items(developer_backends=True)
assert "Native mods" in launcher_menu_items(developer_backends=True)


def touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")


with tempfile.TemporaryDirectory(prefix="epoch-godot-backend-") as temporary:
    root = Path(temporary)
    epoch = root / "epoch"
    backend_root = epoch / "backends" / "ages-godot"
    project = backend_root
    touch(project / "project.godot")
    editor = root / "Godot.exe"
    touch(editor)

    discovered = discover_godot_backend(epoch, environment={"GODOT4": str(editor)})
    assert discovered.backend is not None
    assert discovered.backend.executable == editor.resolve()
    assert discovered.backend.project_directory == project.resolve()
    assert discovered.backend.mods_directory == (backend_root / "mods").resolve()
    assert discovered.backend.command() == [
        str(editor.resolve()),
        "--path",
        str(project.resolve()),
        "--",
        f"--mods-dir={(backend_root / 'mods').resolve()}",
    ]

    # A neighboring full reconstruction takes precedence over the smaller
    # Epoch lab, so the launcher reuses the real game when both are present.
    neighboring_game = root / "ooa-godot"
    touch(neighboring_game / "project.godot")
    prototype = epoch / "backends" / "ages-godot"
    touch(prototype / "project.godot")
    preferred = discover_godot_backend(epoch, environment={"GODOT4": str(editor)})
    assert preferred.backend is not None
    assert preferred.backend.project_directory == neighboring_game.resolve()

    bundled_game = epoch / "backends" / "ooa-godot"
    touch(bundled_game / "project.godot")
    bundled = discover_godot_backend(epoch, environment={"GODOT4": str(editor)})
    assert bundled.backend is not None
    assert bundled.backend.project_directory == bundled_game.resolve()

    built_runner_root = epoch / "build" / "Release"
    nested = discover_godot_backend(
        built_runner_root, environment={"GODOT4": str(editor)}
    )
    assert nested.backend is not None
    assert nested.backend.project_directory == bundled_game.resolve()

    exported = backend_root / "oracle-of-ages.exe"
    touch(exported)
    exported_discovery = discover_godot_backend(epoch, explicit=backend_root, environment={})
    assert exported_discovery.backend is not None
    assert exported_discovery.backend.executable == exported.resolve()

    custom = root / "custom-mods"
    overridden = discover_godot_backend(
        epoch, explicit=exported, mods_override=custom, environment={}
    )
    assert overridden.backend is not None
    assert overridden.backend.mods_directory == custom.resolve()

    descriptor_root = root / "descriptor"
    editor = descriptor_root / "godot.exe"
    project = descriptor_root / "checkout"
    touch(editor)
    touch(project / "project.godot")
    (descriptor_root / "backend.json").write_text(
        json.dumps(
            {
                "executable": "godot.exe",
                "project": "checkout",
                "mods_dir": "native-mods",
                "arguments": ["--editor-pid", "0"],
            }
        ),
        encoding="utf-8",
    )
    configured = discover_godot_backend(
        epoch, explicit=descriptor_root, environment={}
    )
    assert configured.backend is not None
    assert configured.backend.command() == [
        str(editor.resolve()),
        "--editor-pid",
        "0",
        "--path",
        str(project.resolve()),
        "--",
        f"--mods-dir={(descriptor_root / 'native-mods').resolve()}",
    ]

    broken_root = root / "broken"
    broken_root.mkdir()
    (broken_root / "backend.json").write_text(
        '{"executable":"missing.exe"}', encoding="utf-8"
    )
    broken = discover_godot_backend(epoch, explicit=broken_root, environment={})
    assert broken.backend is None
    assert broken.diagnostics and "missing.exe" in broken.diagnostics[0]

print("Godot backend discovery tests passed.")
