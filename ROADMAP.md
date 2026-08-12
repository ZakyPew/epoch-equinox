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
| **Quality of life** | 40 ms audio latency, instant boot past the splashes, display options with a live scale readout |

---

## Next up

Highest value per unit of work, and nothing here is blocked.

### Dungeon terrain — **S** *(no longer blocked)*
`tests/saves/` has two endgame files parked inside a dungeon, and
`VOX_DUMP_ROOM=1` prints what the renderer works from. That evidence
corrects an earlier guess made from a screenshot: **the collision data
indoors is fine.** In Veran's tower the gate passes and the grid comes back
as a clean room — walls `$0F` classified solid, floor `$00` flat, doorways
where doorways are, `$FF` boundary fill down the unused column:

```
  0F 0F 0F 0F 0F 0F 00 00 00 0F 0F 0F 0F 0F 0F FF   |OOOOOO...OOOOOO.|
  0F 00 00 00 00 00 00 00 00 00 00 00 00 00 0F FF   |O.............O.|
```

So this is a *rendering* job, not a classification one. Two things to chase:
the room reads 15 columns wide against the overworld's 10, and Link's
camera-space anchor sits at y=169 in a room the height field treats as 128
tall — a large scrolling room, which the screen→room mapping was never
exercised against. Link's sprite also comes out as an untextured slab in
there (`docs/dungeon-chase.png`), which is a billboard problem, not terrain.

### Dynamic props — **S**
The opening-scene chest demonstrates why an object cannot be identified from
the finished overworld map alone. Normal open/closed chest metatiles are BG
layout objects (`$F0`/`$F1`), but the chest used in that scripted scene is an
OAM pair (`$60`/`$62`). It therefore needs an OAM-aware compound-object pass:
group the two authored tiles, preserve their live palette and pixels, give the
body/lid real depth, and sort the whole object against Link by one world-space
footpoint. The workflow and captured-state debugging steps are in
[`docs/VOXEL_CONTRIBUTING.md`](docs/VOXEL_CONTRIBUTING.md).

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

See [Contributing](README.md#contributing--wed-love-more-hands) for the whole
project, and the [voxel contributor guide](docs/VOXEL_CONTRIBUTING.md) for the
renderer architecture and visual-test loop.
