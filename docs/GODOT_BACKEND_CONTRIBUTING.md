# Godot Ages backend: contributor and build guide

Epoch includes the Godot reconstruction as the separate `backends/ooa-godot`
Git submodule. Make gameplay changes in that repository; make launcher
discovery, launch, and UI changes in Epoch's `launcher/` code. The parent Epoch
repository records the exact submodule commit it uses.

## Get the source

Clone Epoch with its submodule:

```powershell
git clone --recurse-submodules https://github.com/ZakyPew/epoch-equinox.git
cd epoch-equinox
```

For an existing clone that is missing the backend:

```powershell
git submodule update --init --recursive
```

The submodule is pinned to `ZakyPew/ooa-godot` on `codex/mod-asset-overlays`.
Do not flatten its files into Epoch or change `.gitmodules` to point at a
personal checkout. The small `backends/ages-godot` project is only a launcher
fallback and is not where reconstruction gameplay should be developed.

## Requirements and build

- Godot 4.7.1 with .NET support
- .NET 8 SDK
- PowerShell for the supplied validation scripts
- For generating local game data: a legally obtained clean US Ages ROM and a
  local vanilla `oracles-disasm` checkout; neither belongs in Git

From the Godot project directory:

```powershell
cd backends/ooa-godot
dotnet build
```

`dotnet build` compiles the game, importer tools, mod tests, and validation
assembly. It does not require a ROM when already generated assets are present.
For a fresh checkout, follow the submodule's README to import local assets; do
not hand-edit generated files under `assets/oracle/`.

Set `$godot` to the installed Godot .NET console executable, then boot a
known room headlessly:

```powershell
$godot = 'C:\path\to\Godot_v4.7.1-stable_mono_win64_console.exe'
& $godot --headless --path . --quit-after 10 -- --group=0 --room=8a
```

Run the full registered gameplay suite (currently 610 scenarios) with the
project's parallel runner:

```powershell
& .\tools\validate_parallel.ps1 -Godot $godot
```

The script's default Godot path is specific to one developer's machine; pass
`-Godot` on other systems. For one registered validation, use
`-- --validate --validate-only=ValidateMethodName` with the headless command
shown in the submodule's development guide.

To launch the full game visibly, use the .NET Godot executable:

```powershell
$godotGui = 'C:\path\to\Godot_v4.7.1-stable_mono_win64.exe'
& $godotGui --path .
```

Development-only room arguments follow `--`, e.g. `-- --group=0 --room=8a`.
They bypass title/file selection and normal progression, so do not use them as
evidence of retail flow.

## Launch through Epoch

From the Epoch root, point to the Godot .NET executable and start the launcher
with the developer-only native backend actions enabled:

```powershell
$env:GODOT4 = 'C:\path\to\Godot_v4.7.1-stable_mono_win64.exe'
python launcher\epoch_launcher.py `
  --runner build-vcpkg\Release\epoch.exe `
  --dev-native-backend
```

The launcher prefers the embedded submodule and passes its `mods/` directory
to the game's native mod loader. `OOA_GODOT_PATH` can select another checkout;
`--godot-mods-dir` can select a separate mod collection. The native option is
hidden from the normal player menu while the backend remains a developer
preview.

The developer menu's **Native mods** action opens Epoch's separate Godot
manifest manager. It can refresh the list, open the active native mods folder,
show malformed/duplicate manifest diagnostics, and toggle `enabled`; changes
take effect on the next native launch. It does not install ROM patches or alter
the classic Mods dialog. See the submodule's
[modding guide](../backends/ooa-godot/docs/modding.md) for manifest fields,
priority rules, and the currently supported complete `.tsv` / `.png` overlays.

## Contribute safely

Start with the [Godot starter task list](GODOT_STARTER_TASKS.md) to choose a
bounded first issue. Read the submodule's project principles and subsystem
guide before changing gameplay; a coding agent can also use its `AGENTS.md` as
a rigorous workflow checklist.

1. Agree on a small gameplay, validation, or tooling task and identify its
   subsystem in the Godot project's documentation index.
2. Create a feature branch inside `backends/ooa-godot`, not at the Epoch root.
   Trace the source behavior and add a focused validation with code changes.
3. Build and run the relevant validation; for gameplay changes run the full
   parallel suite. Report the exact command and result in the PR.
4. For an external contribution, fork `ZakyPew/ooa-godot` on GitHub, add that
   fork as a remote inside the submodule checkout, push the feature branch
   there, and open a PR targeting `ZakyPew/ooa-godot`. For example, from the
   Epoch root:

   ```powershell
   cd backends/ooa-godot
   git remote -v
   git remote add my-fork https://github.com/YOUR-ACCOUNT/ooa-godot.git
   git switch -c my-feature
   # make and test the change, then commit it
   git push -u my-fork my-feature
   ```

   If `my-fork` already exists, use it rather than adding it again. Keep
   gameplay/runtime changes in that Godot PR. If Epoch also needs to adopt the
   resulting commit, open a separate Epoch PR that updates the submodule
   pointer and links the Godot PR/commit. Launcher-only changes go directly
   through an Epoch PR.
5. Keep ROMs, save files, generated ROM-derived art/data, build output, and
   local Godot settings out of commits. The Godot repository currently has no
   tracked `LICENSE` file. Do not copy Epoch's root license into the submodule
   or imply that a code license covers Nintendo's game, disassembly, or
   ROM-derived content. Before adding a license, maintainers need to confirm
   they have authority to license the original contributions and identify
   third-party/derived files that must be excluded or separately attributed.
   This is project guidance, not legal advice; get qualified review if the
   intended distribution is commercial or broad public release.

The Epoch PR should describe changes in both repositories separately: the
Godot submodule commit and any parent-repository launcher/docs changes.

For a starter task, comment on its GitHub issue with the task you want to
claim, your planned scope, and the check you intend to run. Wait for a
maintainer to confirm ownership before doing broad or multi-week work; this
keeps contributors from duplicating effort or accidentally widening a task.

## Current rights status

The Epoch repository's root `LICENSE` explicitly excludes the Nintendo game;
it is not the license for `backends/ooa-godot`. That submodule was imported
from a separate project and currently has no tracked license, so do not tell
contributors that it is open-source or that a Nintendo ROM/asset license is
included. Until maintainers confirm the provenance and authority to license
the submodule's original code, keep participation to issue reports, task
discussion, and review proposals; do not merge or redistribute third-party
contributions as though a license were settled. This is not legal advice.
