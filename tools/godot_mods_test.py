#!/usr/bin/env python3
"""Contract tests for Epoch's separate ooa-godot mod manifest manager."""
from __future__ import annotations

import json
import tempfile
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launcher"))

from godot_mods import scan_native_mods, set_native_mod_enabled  # noqa: E402


def manifest(root: Path, folder: str, value: object) -> Path:
    path = root / folder / "manifest.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


with tempfile.TemporaryDirectory(prefix="epoch-native-mods-") as temporary:
    root = Path(temporary)
    assert scan_native_mods(root / "missing").mods == ()

    alpha = manifest(root, "a", {"id": "shared", "version": "1.0", "enabled": False})
    manifest(root, "b", {"id": "shared", "version": "2.0"})
    manifest(root, "c", {"id": "bad id", "version": "1"})
    manifest(root, "d", {"id": "broken", "version": ""})
    manifest(root, "e", {"id": "last", "version": "1", "priority": -2})
    (root / "f").mkdir()
    (root / "f" / "manifest.json").write_text("{oops", encoding="utf-8")
    (root / "stray.txt").write_text("ignored", encoding="utf-8")

    scan = scan_native_mods(root)
    assert [mod.mod_id for mod in scan.mods] == ["last", "shared"]
    assert scan.mods[0].priority == -2 and scan.mods[0].enabled is True
    assert scan.mods[1].enabled is False and scan.mods[1].name == "shared"
    assert len(scan.diagnostics) == 4
    assert "duplicate mod id" in scan.diagnostics[0]

    # The dialog writes only enabled, preserves fields added by mod authors,
    # and does nothing when the manifest already has the requested value.
    original = json.loads(alpha.read_text(encoding="utf-8"))
    original["author"] = "test author"
    original["custom"] = {"keep": True}
    alpha.write_text(json.dumps(original), encoding="utf-8")
    set_native_mod_enabled(alpha, "shared", True)
    updated = json.loads(alpha.read_text(encoding="utf-8"))
    assert updated["enabled"] is True
    assert updated["author"] == "test author" and updated["custom"] == {"keep": True}
    assert not list(root.glob("**/.manifest-*.tmp"))
    set_native_mod_enabled(alpha, "shared", True)
    try:
        set_native_mod_enabled(alpha, "other", False)
    except ValueError as exc:
        assert "expected 'other'" in str(exc)
    else:
        raise AssertionError("stale manifest ID should be rejected")

print("Godot native mod manifest tests passed")
