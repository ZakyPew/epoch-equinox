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
| **Object-aware terrain** | `wRoomLayout` names what each cell *is*: trees stand as trunk-and-canopy billboards wearing their own tile art, tufts hug the ground, cliff faces tile their own rock courses |
| **Persistent world** | Every visited room is remembered; the chase cam draws remembered neighbours past the room border, so the world fills in as you explore |
| **Mod billboard art** | A mod's `voxel/tree.ppm` / `voxel/tuft.ppm` dresses every billboard in the chase cam |
| **Rewind** | Hold `R` for ~12 s of history; `F9` restarts the current room |
| **Self-update** | The launcher notices a new release, shows its notes, and installs it over itself — ROMs, mods, covers and saves untouched |
| **Achievements** | Data-driven packs watched over WRAM, popped as Steam-style toasts over the window; moddable, one text file per pack |
| **Auto-entered secrets** | The in-game panel walks the password grid and types any secret for you |
| **Secret generator** | Every code your save can produce — game, ring, and all twenty NPC secrets — generated in the launcher with the game's own cipher |
| **Quality of life** | 40 ms audio latency, instant boot past the splashes, display options with a live scale readout |

---

## Next up

Highest value per unit of work, and nothing here is blocked.

### Dungeon terrain — **S**
The voxel mode has only ever been verified outdoors. Interiors use the same
collision data, so this is mostly *checking* rather than building: do holes
sink, do walls read, does the scroll gate hold in large scrolling rooms.
**Blocked on:** a battery save parked in a dungeon (`tests/saves/`).

### Save import / export — **S**
The runner already keeps ordinary battery saves (`<title>.sav` beside the
binary). Add launcher buttons to import a `.sav` from any emulator (with a
backup of what it replaces) and export the current one.

---

## Bigger swings

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
