# Native Ages launcher and Godot starter tasks

These are bounded starting points for community contributors. The project goal
is faithful Oracle of Ages behavior, not a redesign: compare against a clean
US game and the matching `oracles-disasm` source, then add a focused automated
regression with any behavior change. Never commit ROMs, generated ROM-derived
assets, or personal saves.

## Epoch-only task: add UI tests for native mod management

**Difficulty:** beginner/intermediate; does not require a ROM or editing the
Godot submodule.

The launcher now has a separate `NativeModsDialog`. Add an offscreen Qt test
that creates temporary manifests and checks that valid mods are listed,
malformed/duplicate manifests are diagnosed, toggling and saving changes only
the `enabled` field, Cancel writes nothing, and Refresh reflects files added
while the dialog is open. Use temporary directories only; do not touch the
checked-in backend's `mods/` folder. Put the test in `tools/` and add it to the
existing launcher CI job.

**Done when:** the test runs with `QT_QPA_PLATFORM=offscreen`, passes in CI,
and fails if the dialog accidentally calls the classic ROM-mod state writer.
The root Epoch MIT license covers launcher files, as described in `LICENSE`;
this does not license the Godot submodule or Nintendo content.

## Godot gameplay and tooling tasks

The original project creator has authorized Epoch to modify and publicly
redistribute its maintained fork and accept community contributions. These
tasks are ready to claim; submit implementation PRs to `ZakyPew/ooa-godot`
targeting `codex/mod-asset-overlays`, following the [Godot build and
contribution guide](GODOT_BACKEND_CONTRIBUTING.md). Preserve upstream
attribution. This permission does not grant rights to Nintendo content or
provide a blanket license for third-party reuse outside this project.

For modding work, start from the checked-in [synthetic priority demo](../backends/ooa-godot/docs/modding.md#run-the-synthetic-priority-example)
and its automated test rather than using ROM-derived example assets.

### Reproduce one player-animation discrepancy

**Difficulty:** beginner-friendly investigation; small code fix only if the
comparison reveals a specific defect.

Compare Link's walking animation in the Godot game and a clean US Oracle of Ages
run in the same top-down room, first on flat ground and then while entering a
water/slow terrain tile. Record the room, starting position, held input, number
of original 60 Hz updates, and the observed animation/frame sequence. Use the
existing player validation helpers and source references rather than judging
from two differently scaled screenshots alone.

**Done when:** the issue or PR contains a deterministic reproduction; any
claimed mismatch is traced to its source behavior; and a focused validation
checks the relevant frame/timing boundary. Keep movement, sprite artwork, and
render scaling as separate issues if they turn out to be separate causes.

### Gameplay slice: Mermaid Suit deep-water transition

**Difficulty:** intermediate.

Top-down surface seawater entry, Mermaid movement, water exit, and re-entry now
have an initial source-backed path. Deep-water transitions remain unfinished
in [implementation status](../backends/ooa-godot/docs/implementation-status.md).
Trace `linkUpdateDiving` and its `checkForUnderwaterTransition@levelDown`
caller in `link.s`, including the `wDisableScreenTransitions` gate and the
`TILEINDEX_DEEP_WATER` test. Implement one transition boundary only; do not
attempt every underwater room or all Mermaid Suit behavior at once. Extend the
player validation with a focused scenario for the selected dive/transition
boundary and the no-transition case.

**Done when:** the transition matches the traced source timing, destination,
Link position, and retained dive state; the disabled-transition case stays
local; expectations are independently derived from the ROM/disassembly;
`dotnet build` has no warnings/errors; and the complete Godot validation suite
passes.

## Where to work and how to submit

- Gameplay, generated-asset mod loader, and its tests: work inside
  `backends/ooa-godot`; external contributors fork `ZakyPew/ooa-godot` and
  open a PR targeting that repository as explained in the build/contribution
  guide.
- Launcher discovery, launch behavior, or Epoch UI: work in Epoch's
  `launcher/` and tests. Do not copy Godot gameplay code into the launcher.
- A change spanning both: keep commits and review scopes separate, then update
  Epoch's submodule pointer to the reviewed Godot commit.
- Use the [build and contribution guide](GODOT_BACKEND_CONTRIBUTING.md) for
  setup and exact validation commands. State what you ran and what remains
  unverified in the PR.

The Godot repository has no tracked general-purpose software license.
Community contributions are welcome under the creator's project-specific
authorization, but contributors should not assume this grants them blanket
rights to reuse the repository outside Epoch. This task list does not grant
rights to Nintendo game content or assets.
