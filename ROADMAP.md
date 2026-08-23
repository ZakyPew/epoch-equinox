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
| **Voxel diorama** | Terrain from the game's real quarter-cell collision masks, 3× internal resolution, game-state skies, domed foliage, textured faces |
| **Chase camera** | Third-person perspective raycast, camera on the right stick |
| **Room overrides** | Hand-sculpt any room's heights in a text file; `VOXEL_EDIT=1` writes the template |
| **Object-aware terrain** | `wRoomLayout` names what each cell *is*: original tree art becomes separate hard canopy/trunk tiles, tufts become shallow reliefs, enclosed cliff ground becomes a raised plateau, and exposed height edges get straight tile-textured faces |
| **Persistent world** | Every visited room is remembered; the chase cam draws remembered neighbours past the room border, so the world fills in as you explore |
| **Mod vegetation art** | A mod's `voxel/tree.ppm` / `voxel/tuft.ppm` replaces the source art while keeping the default world-space voxel geometry |
| **Rewind** | Hold `R` for ~12 s of history; `F9` restarts the current room |
| **Self-update** | The launcher notices a new release, shows its notes, and installs it over itself — ROMs, mods, covers and saves untouched |
| **Achievements** | Data-driven packs watched over WRAM, popped as Steam-style toasts over the window; moddable, one text file per pack |
| **Auto-entered secrets** | The in-game panel walks the password grid and types any secret for you |
| **Secret generator** | Every code your save can produce — game, ring, and all twenty NPC secrets — generated in the launcher with the game's own cipher |
| **Stream overlays** | The player writes live game state to disk; OBS-ready layouts (bar, framed, vertical) read it over `file://`, configured from the launcher's Stream page, rendering-tested in CI in a real Chromium |
| **Speedrun kit** | Auto-splits watched over WRAM drive LiveSplit's TCP server; optional run timer, split list, item tracker and input display on the overlays; routes verified against real finished saves |
| **In-game sculpting** | F4 in any voxel mode: paint the cell in front of Link with the number keys, Backspace undoes per room; writes the same override files hands do |
| **Saves** | Import a `.sav` from any emulator (validated by content), export copies, and a backup trail where every replacement is one click to undo |
| **Quality of life** | 40 ms audio latency, instant boot past the splashes, display options with a live scale readout |

---

## Next up

Highest value per unit of work, and nothing here is blocked.

### Dungeon terrain — **shipped, with a route for the rest**
The collision data indoors was always fine; the rendering caught up.
Large scrolling rooms (15 columns, a camera loose in `hCameraY`) now map
screen to room correctly — `wScreenOffset` is page bookkeeping in those
rooms, not displacement — sprites the game parks invisibly behind the HUD
bar no longer billboard as phantom pillars on the horizon, and Link
renders textured (`docs/dungeon-chase.png` is a current capture inside
Veran's tower). The route that made it iterable is documented at the top
of `tools/vox_shot.c`: script the file select over a parked test save and
you are inside a dungeon with live state by frame ~1560, headless. What
remains here is polish an eye will find faster than a probe: play a
dungeon in chase cam and report what looks wrong.

### Dynamic props — **S**
The opening-scene chest demonstrates why an object cannot be identified from
the finished overworld map alone. Normal open/closed chest metatiles are BG
layout objects (`$F0`/`$F1`), but the chest used in that scripted scene is an
OAM pair (`$60`/`$62`). It therefore needs an OAM-aware compound-object pass:
group the two authored tiles, preserve their live palette and pixels, give the
body/lid real depth, and sort the whole object against Link by one world-space
footpoint. The workflow and captured-state debugging steps are in
[`docs/VOXEL_CONTRIBUTING.md`](docs/VOXEL_CONTRIBUTING.md).

### Item icons from the player's ROM — **M** *(parked, honestly)*
The overlay item tracker names items and tiers in text; the game's own
sprites would be better. Two dead ends so far, documented in the tree:
`treasureDisplayData` chains four-plus hops to the wrong sprite, and the
attract demo never populates the state a capture route needs. The
remaining route is scripted file-select input, which is slow but not
impossible. Never commits art — icons rip from the player's own ROM at
runtime or not at all.

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
**room sculpting** (now doable from inside the game — F4 and the number
keys), **dungeon saves** (park a save somewhere interesting, drop it in
`tests/saves/`), and **eyes on the sculpt HUD** — the gold brush tint and
its HUD shipped in v0.6.0 verified headlessly but have never been seen in
a live session; the first person to press F4 should say what looks off.

See [Contributing](README.md#contributing--wed-love-more-hands) for the whole
project, and the [voxel contributor guide](docs/VOXEL_CONTRIBUTING.md) for the
renderer architecture and visual-test loop.
