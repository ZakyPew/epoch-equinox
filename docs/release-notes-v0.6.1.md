# Epoch & Equinox v0.6.1

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. Everything v0.6.0 shipped, plus a week of
polish on top of it.

## Saves, managed

A **Saves** page in the launcher menu. Import a `.sav` from any emulator —
checked by content before it lands (a Seasons save on the Ages page is
refused by what it holds, not its filename), and it works even before your
first in-game save, since the file's name is derived from the ROM the same
way the player derives it. Export copies anywhere. And nothing on the page
can lose a file: anything that replaces the save — an import, a restore —
backs the old one up first into `save-backups/`, listed right there with
one-click Restore. Each slot shows its run at a glance: essences, hearts,
rings, deaths, playtime.

## The item tracker wears the game's own icons

`python3 tools/rip_item_icons.py` boots your carts headlessly over your
save, walks the game's menus, equips every item in the bag and reads each
icon out of the HUD's own sprites — real palettes, transparent background.
The overlay tracker shows them greyed until the run collects each item,
and falls back to text for anything unripped. The icons are your
cartridge's art: they stay on your machine, gitignored, never shipped.

## Dungeons render right

Live dungeon frames went under the renderer for the first time, and three
fixes came out: large scrolling rooms (Veran's tower is 15×11 cells with a
roaming camera) no longer sample their terrain rows off; the phantom
pillars on the horizon are gone (they were HUD indicator sprites the game
parks invisibly behind the bar, billboarded into the world); and HUD tiles
no longer receive terrain heights. Link was already textured indoors —
`docs/dungeon-chase.png` is a current capture.

## Sculpt mode: undo

**Backspace** un-paints, newest first, per room — and it puts back exactly
what each paint replaced, including hand-authored values that `0` (reset
to collision) could never restore. The HUD shows how many undos the room
you are standing in holds.

## Also

- Contributor onboarding: `CONTRIBUTING.md`, issue templates, and
  `docs/CONTRIBUTOR_STARTER_TASKS.md` backing the `help wanted` issues —
  each with its files, its fast headless loop, and what done looks like.
- The headless tooling can now reach real gameplay: park a save and
  script the file select (`tools/vox_shot.c` documents the route). This
  is what unlocked the dungeon fixes and the icon rip.
- README front page has a *What's inside* table that actually covers the
  project; the roadmap answers the Discussions request for fog and
  shaders (short version: chase-cam fog already exists and is
  live-tunable — an environment-coloured atmosphere pass is on the list).

## Verified

`save_manager_test` (30 checks) proves the backup trail byte-for-byte
against real endgame saves; `voxedit_test` grew to 42 with the undo
trail; the dungeon fixes pass all five render/logic suites; and the 52
overlay render checks pass with and without ripped icons present.
