# Credits

This project stands on other people's work. Here is exactly what came from
where, and what is original to this repository.

## Built on

### [GB-Recomp/gb-recompiled](https://github.com/GB-Recomp/gb-recompiled) — MIT

The static recompiler (`gbrecomp`) and the runtime (`gbrt`) that everything
here depends on. `gbrecomp` is what turns your ROM into C; `gbrt` provides the
CPU/PPU/APU emulation, SDL platform layer, GLES renderer, savestates, cheats,
link cable, SGB support and the Esc menu.

Itself a fork of [arcanite24/gb-recompiled](https://github.com/arcanite24/gb-recompiled),
which is where the recompiler originates.

CMake fetches it at configure time; it isn't vendored here. `patches/gbrt-frame-hook.patch`
adds a ~27-line frame hook to it so a host can substitute its own rendering —
that patch is small, upstreamable, and offered back to them.

### [Stewmath/oracles-disasm](https://github.com/Stewmath/oracles-disasm)

The Oracles disassembly. Not required to build, but if you drop its `.sym`
files into `roms/` the recompiler uses them, and generated functions get real
names instead of addresses.

## History

This repository began as a fork of [GB-Recomp/tlozooa](https://github.com/GB-Recomp/tlozooa),
which packaged pre-generated Oracle of Ages C together with a small launcher.
The Seasons equivalent is [GB-Recomp/tlozoos](https://github.com/GB-Recomp/tlozoos).

It no longer contains any of that generated code — nor generates it.
tools/interp_probe.c showed the recompiled output was never executed (the
runtime interprets the loaded ROM directly, byte-identically), so the
project became what it functionally always was: a native player. What
remains is the launcher, the mod loader, the renderer and the platform
work — all original. The GB-Recomp packaging is still what started this
and is why it exists at all.

## Original to this project

- `launcher/` — the launcher application, and its gamepad support
- `src/mod_loader.{c,h}` — manifest parsing, IPS and BPS patching, overlays
- `src/voxel/` — the voxel diorama renderer
- `cmake/GenerateCarts.cmake` — build-time recompilation from your own ROM
- `runner_main.cpp`, `setup.sh`, `tools/`, `patches/`

### On the voxel renderer

It is **not** a port of [DramaticShapeVoxelMod](https://github.com/DramaticShape/DramaticShapeVoxelMod).
That mod targets [pokemon-gen1-recomp-project](https://github.com/bryanthaboi/pokemon-gen1-recomp-project),
a Lua/LÖVE reimplementation with an engine-level mod API and a real scene
graph — a fundamentally different architecture from a static recompilation,
which has only emulated VRAM to read. Its code is also not redistributable.
No code, data or asset from it is used here.

The shared idea is the obvious one that predates both: extrude a top-down
game's own tile and sprite data into a diorama.

## Not included

**No ROM data, ever.** Not in the repository, not in releases, not in CI
artifacts. The Legend of Zelda: Oracle of Ages and Oracle of Seasons are
© Nintendo / Capcom. You supply your own dumps of games you own; hashes are
verified before anything is recompiled.

Game Boy and Game Boy Color are trademarks of Nintendo.
