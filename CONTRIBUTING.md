# Contributing to Epoch Equinox

Thanks for helping improve Epoch & Equinox. The easiest way to contribute is to pick one small, observable task, attach before/after evidence when it is visual, and keep each pull request focused.

## Choose a track

| Track | Good first contribution |
| --- | --- |
| Godot Ages | Fix one reconstruction behavior or add a source-backed regression in the `ooa-godot` submodule |
| Voxel art | Sculpt one room or repair one prop using the in-game voxel editor |
| Rendering | Improve depth ordering, camera behavior, or a compound object such as a chest |
| Testing and tools | Add a reproducible screenshot route or a small diagnostic |
| Documentation | Clarify a workflow, capture a known issue, or improve contributor onboarding |

Start with the [Epoch contributor starter tasks](docs/CONTRIBUTOR_STARTER_TASKS.md), or the [native Ages launcher and Godot starter tasks](docs/GODOT_STARTER_TASKS.md). The native launcher task is safe to contribute to under Epoch's root license; work in the separate Godot submodule is subject to its unresolved rights review below.

## Quick start

1. Clone the repository and build the project using the commands in [README.md](README.md).
2. Run a known-good flat-mode comparison before changing voxel code.
3. Pick one starter task or open an issue using one of the templates.
4. Make the smallest useful change and include the exact test or screenshot route you used.
5. Open a pull request with a short summary, reproduction steps, and before/after evidence.

The detailed voxel workflow, coordinate conventions, debug flags, and legal asset rules are in [docs/VOXEL_CONTRIBUTING.md](docs/VOXEL_CONTRIBUTING.md).

For the native Godot Ages backend, follow the [Godot build and contribution guide](docs/GODOT_BACKEND_CONTRIBUTING.md). Gameplay code and its tests belong in the submodule; Epoch owns the launcher integration.

The Godot submodule is currently unlicensed and was imported from a separate project. Until maintainers resolve code provenance and licensing authority, community members can help by reporting issues, discussing the starter tasks, and reviewing proposals—but we should not advertise the submodule as open source or merge/redistribute outside contributions as if rights were settled. Do not add a license covering Nintendo game content. See the rights-status section in the Godot guide.

## What makes a useful PR

- One behavior, visual fix, tool, or documentation change per PR.
- A clear reproduction route: save state, room/area, camera mode, and relevant flags.
- Flat mode and voxel mode checked when the change affects rendering.
- Screenshots or a short capture for visual changes.
- No unrelated generated files, local configuration, or build output.
- Tests or a focused smoke check described in the PR body.

For renderer changes, please call out whether the result depends on a specific screen, room height, sprite/OAM object, camera angle, or cutscene state. Those details make visual regressions much easier to reproduce.

## Asset and ROM safety

Do not commit or attach a ROM, ROM-derived tileset, extracted copyrighted asset, or a screenshot that exposes more game content than is needed to demonstrate the issue. Contributors should supply their own legally obtained game files locally. Small legal save files may be used in `tests/saves/` when they are already part of the project workflow; never include private credentials or personal save data.

## Need help?

Open an issue with the closest template, include what you expected and what you saw, and attach the smallest useful evidence. If you are unsure whether an asset or save is appropriate to share, ask before uploading it.
