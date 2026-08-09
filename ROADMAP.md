# Roadmap

Where Epoch &amp; Equinox is going, what each piece costs, and what blocks what.
Sizes are rough: **S** ≈ a session, **M** ≈ a few sessions, **L** ≈ a project.

---

## Shipped

The player itself is done and distributable — everything below builds on it.

| | |
|---|---|
| **Native player** | Both Oracle carts from one binary, no game data in the repo or releases |
| **Launcher** | Cover art, mod toggles, ROM install, controller navigation |
| **Mods** | IPS, BPS, file overlays; stock ROM kept pristine, mods applied in memory |
| **Voxel diorama** | Terrain from the game's real collision data, 3× internal resolution, game-state skies, domed foliage, textured faces |
| **Chase camera** | Third-person perspective raycast, camera on the right stick |
| **Room overrides** | Hand-sculpt any room's heights in a text file; `VOXEL_EDIT=1` writes the template |
| **Rewind** | Hold `R` for ~12 s of history; `F9` restarts the current room |
| **Quality of life** | 40 ms audio latency, instant boot past the splashes, display options with a live scale readout |

---

## Next up

Highest value per unit of work, and nothing here is blocked.

### Dungeon terrain — **S**
The voxel mode has only ever been verified outdoors. Interiors use the same
collision data, so this is mostly *checking* rather than building: do holes
sink, do walls read, does the scroll gate hold in large scrolling rooms.
**Blocked on:** a battery save parked in a dungeon (`tests/saves/`).

### Object-aware terrain — **M**
The single biggest visual gap to the reference voxel mods. Today every solid
cell extrudes into the same block. The game keeps a second map, `wRoomLayout`,
naming *what each cell is* — and multi-cell objects carry a distinct id per
quadrant, so a 2×2 tree is identifiable as one tree. Using it means trees can
be trunk-plus-canopy instead of a mesa.
**Unlocks:** per-face texturing (bark on the sides, canopy on top), which is
the other half of the same gap.

### Secret generator — **M**
See [Secrets](#secrets) below. Generate valid codes from the player's own save
and show them in the launcher.

### Achievements — **M**
The memory-watch machinery already exists for the renderer. What's missing is a
condition format, a definitions file, and a toast. Naturally moddable: an
achievement pack is just a data file.

---

## Bigger swings

### Auto-entered secrets — **M**, after the generator
Drive the in-game password screen with the runtime's input-script system so the
player never types a code. Fiddly (cursor navigation on a symbol grid) but the
infrastructure exists — it is how the renderer gets tested headlessly.

### Room cache / persistent world — **M**
The games keep one room in memory, so the 3D view ends at the room border.
Caching each room's heightfield as it is visited lets the world extend to the
horizon and fill in as you explore.

### Voice packs — **L**
Dialogue open/close is already detected and text ids sit beside the flag.
Needs: a transcript logger (which doubles as the recording script), an audio
mixer over the existing SDL output, and a pack format. The content is the
mountain, not the code — but a pack does not have to be complete to ship.

### Mod scripting API — **L**
IPS/BPS patch bytes; the next step is a scripting surface over the games' named
WRAM symbols. The disassembly names hundreds; the renderer already reads a
dozen live. This is what turns "romhack" into "mod".

### First-person camera — **M**
Same raycaster as the chase camera with the eye at Link's position. Honest
caveat: 8-pixel tiles across a full view are *very* chunky, and it is still a
four-direction grid game underneath — a novelty toggle rather than a new way to
play.

---

## Secrets

Worth stating plainly, because it shapes the design: **Oracle secrets are not
universal passwords.** Every save generates a random Game ID, and every code is
encoded against that ID with a checksum, so a code from a website will not
validate on your file. Any menu has to *generate*, not look up.

Three kinds, in increasing order of how much automating them helps:

| Kind | What it does | Why automate |
|---|---|---|
| **Game secret** | Starts a linked playthrough in the other game — carries your progress, changes the story, opens the true ending | One-time, but the highest-stakes code in the games |
| **Ring secret** | Carries your ring collection across to the linked game | One-time companion to the above |
| **NPC / memory secrets** | ~15 small exchanges: take a code from a character in one game, enter it in the other, get an upgrade and often a code to bring back | The tedious ones. This is where a generator earns its keep |

Build order: **generate and display** first (immediately useful, and testable
against the community's open-source implementations), **auto-entry** second
once the codes are proven correct.

---

## Not planned

- **Widescreen.** The games only keep one screen of map in memory; there is
  nothing to show at the edges.
- **Redrawn or AI-generated art.** The look comes from the cartridge's own
  tiles, and that is the point.
- **Shipping any game data.** Not now, not ever.

---

## How to help

Every item above is open. The ones that need an eye rather than a compiler:
**room sculpting** (a text file per room, no build step) and **dungeon saves**
(park a save somewhere interesting, drop it in `tests/saves/`).

See [Contributing](README.md#contributing--wed-love-more-hands) for where each
subsystem lives.
