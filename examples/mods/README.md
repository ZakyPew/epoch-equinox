# Example mods

Copy a directory from here into `mods/` next to the `oracles` binary, drop the
patch file in beside its `manifest.json`, and it shows up in the launcher's
Mods dialog.

```
mods/
  example-randomizer/
    manifest.json
    seed.bps          <- the patch named by "patch"
    overlay/          <- optional, "<hex-offset>.bin" raw splices
      03f200.bin
```

## Fields

| key | meaning |
|---|---|
| `id` | stable identifier; the launcher's `mods/state.json` keys off it |
| `name` / `version` | shown in the launcher |
| `games` | cart ids this applies to (`tlozooa`, `tlozoos`). Omit for both |
| `patch` | `.ips` or `.bps` file, relative to the mod directory |
| `overlay` | directory of `<hex-offset>.bin` raw byte splices |
| `priority` | lower applies first; default 100 |
| `enabled` | default state before the user toggles it |

Unknown keys are ignored, so a manifest written for a newer loader still loads.

## Notes

- BPS patches carry a checksum of the ROM they were built from. One built for a
  different revision is rejected with a message rather than silently producing
  a broken ROM.
- A patch that changes the ROM's *size* cannot work here — the recompiled C is
  bound to the original bank layout. The loader reports it and skips the mod.
- Toggling a mod off fully undoes it: the launcher keeps a pristine
  `assets/<id>/rom.bin.orig` and rebuilds from that every launch.
