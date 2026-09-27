"""Read and update the separate ooa-godot generated-asset mod manifests."""
from __future__ import annotations

import json
import os
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$", re.ASCII)


@dataclass(frozen=True)
class NativeMod:
    mod_id: str
    name: str
    version: str
    priority: int
    enabled: bool
    manifest: Path


@dataclass(frozen=True)
class ModScan:
    mods: tuple[NativeMod, ...]
    diagnostics: tuple[str, ...]


def _parse_manifest(path: Path) -> tuple[NativeMod, dict[str, Any]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"could not read manifest: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError("manifest root must be a JSON object")
    mod_id = data.get("id")
    version = data.get("version")
    if not isinstance(mod_id, str) or not _ID.fullmatch(mod_id):
        raise ValueError("id must start with an ASCII letter or digit and contain only letters, digits, '.', '_' or '-'")
    if not isinstance(version, str) or not version.strip():
        raise ValueError("version must be a non-empty string")
    name = data.get("name", mod_id)
    priority = data.get("priority", 100)
    enabled = data.get("enabled", True)
    name = mod_id if name is None else name
    priority = 100 if priority is None else priority
    enabled = True if enabled is None else enabled
    if not isinstance(name, str):
        raise ValueError("name must be a string")
    if isinstance(priority, bool) or not isinstance(priority, int) or not -(2**31) <= priority < 2**31:
        raise ValueError("priority must be a 32-bit integer")
    if not isinstance(enabled, bool):
        raise ValueError("enabled must be a boolean")
    return NativeMod(mod_id, name, version, priority, enabled, path), data


def scan_native_mods(directory: Path) -> ModScan:
    """Scan immediate children in ordinal folder order, matching Godot's catalog."""
    if not directory.is_dir():
        return ModScan((), ())
    mods: list[NativeMod] = []
    diagnostics: list[str] = []
    seen: set[str] = set()
    try:
        children = sorted(directory.iterdir(), key=lambda path: path.name)
    except OSError as exc:
        return ModScan((), (f"{directory}: could not list mods directory: {exc}",))
    for child in children:
        if not child.is_dir():
            continue
        manifest = child / "manifest.json"
        if not manifest.exists():
            continue
        try:
            mod, _ = _parse_manifest(manifest)
        except ValueError as exc:
            diagnostics.append(f"{manifest}: {exc}")
            continue
        if mod.mod_id in seen:
            diagnostics.append(f"{manifest}: duplicate mod id {mod.mod_id!r}; first manifest wins")
            continue
        seen.add(mod.mod_id)
        mods.append(mod)
    mods.sort(key=lambda mod: (mod.priority, mod.mod_id))
    return ModScan(tuple(mods), tuple(diagnostics))


def set_native_mod_enabled(manifest: Path, mod_id: str, enabled: bool) -> None:
    """Update only enabled, preserving other current manifest fields atomically."""
    current, data = _parse_manifest(manifest)
    if current.mod_id != mod_id:
        raise ValueError(f"manifest now contains id {current.mod_id!r}, expected {mod_id!r}; refresh and try again")
    if current.enabled == enabled:
        return
    data["enabled"] = enabled
    temporary: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=manifest.parent,
            prefix=".manifest-", suffix=".tmp", delete=False,
        ) as stream:
            temporary = stream.name
            json.dump(data, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, manifest)
        temporary = None
    finally:
        if temporary is not None:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
