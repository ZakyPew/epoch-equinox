"""Discovery and command construction for the optional ooa-godot backend."""
from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping


EXPORTED_NAMES = (
    "oracle-of-ages.exe",
    "Oracle of Ages.exe",
    "ooa-godot.exe",
    "oracle-of-ages",
    "ooa-godot",
)


@dataclass(frozen=True)
class GodotBackend:
    executable: Path
    working_directory: Path
    mods_directory: Path
    project_directory: Path | None = None
    arguments: tuple[str, ...] = ()

    def command(self) -> list[str]:
        command = [str(self.executable), *self.arguments]
        if self.project_directory is not None:
            command.extend(("--path", str(self.project_directory)))
        command.extend(("--", f"--mods-dir={self.mods_directory}"))
        return command


@dataclass(frozen=True)
class BackendDiscovery:
    backend: GodotBackend | None
    diagnostics: tuple[str, ...]


def discover_godot_backend(
    epoch_root: Path,
    explicit: Path | None = None,
    mods_override: Path | None = None,
    environment: Mapping[str, str] | None = None,
) -> BackendDiscovery:
    """Find an exported game or a development checkout without merging repos."""
    env = os.environ if environment is None else environment
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    elif env.get("OOA_GODOT_PATH"):
        candidates.append(Path(env["OOA_GODOT_PATH"]))
    else:
        candidates.extend(
            (
                epoch_root / "backends" / "ooa-godot",
                epoch_root / "ooa-godot",
                epoch_root.parent / "ooa-godot",
            )
        )

    diagnostics: list[str] = []
    seen: set[Path] = set()
    for raw_candidate in candidates:
        candidate = raw_candidate.expanduser().resolve()
        if candidate in seen:
            continue
        seen.add(candidate)
        try:
            backend = _backend_from_candidate(candidate, mods_override, env)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            diagnostics.append(f"{candidate}: {exc}")
            continue
        if backend is not None:
            return BackendDiscovery(backend, tuple(diagnostics))

    if explicit is not None and not diagnostics:
        diagnostics.append(f"{explicit}: no ooa-godot executable or project was found")
    return BackendDiscovery(None, tuple(diagnostics))


def _backend_from_candidate(
    candidate: Path,
    mods_override: Path | None,
    environment: Mapping[str, str],
) -> GodotBackend | None:
    if candidate.is_file():
        return _exported_backend(candidate, mods_override)
    if not candidate.is_dir():
        return None

    descriptor = candidate / "backend.json"
    if descriptor.is_file():
        return _descriptor_backend(descriptor, mods_override)

    for name in EXPORTED_NAMES:
        executable = candidate / name
        if executable.is_file():
            return _exported_backend(executable, mods_override)

    project = candidate / "project.godot"
    if not project.is_file():
        return None
    editor = _find_godot(environment)
    if editor is None:
        raise ValueError(
            "project.godot was found, but no Godot executable is available; "
            "set GODOT4 or add Godot to PATH"
        )
    mods = (mods_override or candidate / "mods").expanduser().resolve()
    return GodotBackend(editor, candidate, mods, candidate)


def _exported_backend(executable: Path, mods_override: Path | None) -> GodotBackend:
    resolved = executable.expanduser().resolve()
    mods = (mods_override or resolved.parent / "mods").expanduser().resolve()
    return GodotBackend(resolved, resolved.parent, mods)


def _descriptor_backend(descriptor: Path, mods_override: Path | None) -> GodotBackend:
    data = json.loads(descriptor.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("backend.json must contain a JSON object")
    root = descriptor.parent
    executable_value = data.get("executable")
    if not isinstance(executable_value, str) or not executable_value.strip():
        raise ValueError("backend.json requires a non-empty 'executable' string")
    executable = _relative(root, executable_value)
    if not executable.is_file():
        raise ValueError(f"configured executable does not exist: {executable}")

    project_value = data.get("project")
    project = None
    if project_value is not None:
        if not isinstance(project_value, str) or not project_value.strip():
            raise ValueError("'project' must be a non-empty string when present")
        project = _relative(root, project_value)
        if not (project / "project.godot").is_file():
            raise ValueError(f"configured project has no project.godot: {project}")

    arguments_value = data.get("arguments", [])
    if not isinstance(arguments_value, list) or not all(
        isinstance(argument, str) for argument in arguments_value
    ):
        raise ValueError("'arguments' must be an array of strings")

    configured_mods = data.get("mods_dir", "mods")
    if not isinstance(configured_mods, str) or not configured_mods.strip():
        raise ValueError("'mods_dir' must be a non-empty string")
    mods = (mods_override or _relative(root, configured_mods)).expanduser().resolve()
    working = project or executable.parent
    return GodotBackend(
        executable,
        working,
        mods,
        project,
        tuple(arguments_value),
    )


def _relative(root: Path, value: str) -> Path:
    path = Path(value).expanduser()
    return (path if path.is_absolute() else root / path).resolve()


def _find_godot(environment: Mapping[str, str]) -> Path | None:
    for variable in ("GODOT4", "GODOT"):
        value = environment.get(variable)
        if value and Path(value).expanduser().is_file():
            return Path(value).expanduser().resolve()
    for name in ("godot4", "godot", "Godot_v4.7.1-stable_mono_win64.exe"):
        found = shutil.which(name)
        if found:
            return Path(found).resolve()
    return None
