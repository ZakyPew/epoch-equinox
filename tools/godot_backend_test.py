#!/usr/bin/env python3
from __future__ import annotations

import json
import tempfile
from pathlib import Path

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launcher"))

from godot_backend import discover_godot_backend  # noqa: E402


def touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")


with tempfile.TemporaryDirectory(prefix="epoch-godot-backend-") as temporary:
    root = Path(temporary)
    epoch = root / "epoch"
    backend_root = epoch / "backends" / "ooa-godot"
    executable = backend_root / "oracle-of-ages.exe"
    touch(executable)

    discovered = discover_godot_backend(epoch, environment={})
    assert discovered.backend is not None
    assert discovered.backend.executable == executable.resolve()
    assert discovered.backend.mods_directory == (backend_root / "mods").resolve()
    assert discovered.backend.command() == [
        str(executable.resolve()),
        "--",
        f"--mods-dir={(backend_root / 'mods').resolve()}",
    ]

    custom = root / "custom-mods"
    overridden = discover_godot_backend(
        epoch, explicit=executable, mods_override=custom, environment={}
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
