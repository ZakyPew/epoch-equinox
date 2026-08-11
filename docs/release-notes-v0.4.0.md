# Epoch & Equinox v0.4.0

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. Windows users can run the launcher exe; on
Linux run `epoch`, or `launcher/epoch_launcher.py` for the UI (needs Python +
PySide6).

This release is about everything *around* the game: it now keeps itself up to
date, notices what you have done, and does the typing you should never have
had to do.

## Achievements

The player watches the game's own memory and pops a card over the window when
you earn something — Steam-style, top right, in a gilded frame. The game is
never touched: no patched ROM, no injected sprites, just the host reading WRAM
and drawing over the presented frame.

**47 achievements**, 24 for Ages and 23 for Seasons, each with its own icon.
They cover the obvious milestones (essences, sword and heart upgrades, rings,
kill counts) and the deep cuts: all ten linked-secret exchanges carried to
their reward, the four hidden golden beasts in Seasons, the Victory Ring, the
complete 64-ring collection, Vasu's hundredth appraisal, and the true Ganon
ending of a linked pair.

Unlocks live in `states/achievements-<cart>.txt` — plain text, one id per line,
delete a line to earn it again. They belong to the cart, not to a save slot, so
they survive across playthroughs.

**Achievement packs are data.** `achievements/<cart>.txt` ships with the player
and any `achievements/<cart>.<yourname>.txt` beside it loads too, so a mod adds
its own achievements without code. Six condition kinds over any WRAM address;
the format is in `achievements/README.md`. Custom icons are 48×48 PPMs in
`achievements/icons/<cart>/`, documented in `achievements/icons/README.md`.

## Secrets, generated and typed for you

Oracle secrets are not universal passwords: every save carries a random Game ID
and every code is enciphered against it, so codes from a website will not
validate on your file. The player now generates *yours*.

**In the launcher**, the Secrets page shows every code your save can produce —
the game secret, the ring secret, and all twenty NPC memory secrets with their
Farore return codes, labelled by who to tell, spelled in the games' own symbol
alphabet.

**In the game**, the new panel can type them for you. Open the game's own
secret screen, pick the secret, and the cursor walks the symbol grid and enters
it — twenty symbols without touching the d-pad. It steers by reading the game's
own cursor position every frame rather than writing into its memory, so if the
game disagrees, the game wins.

## A panel of our own

Achievements and Secrets have their own window now, on **F2**, with tabs —
rather than crowding the emulator's Esc menu, which stays what it is: display
and emulator settings.

## Self-update

The launcher checks GitHub for a newer release on startup and notes one in the
corner; the Updates page checks on demand, shows the release notes, and
installs the new build in place. It replaces what the release ships and nothing
else — your `roms/`, `mods/`, `covers/` and saves are left exactly as they are.
Being offline is not an error: the check gives up quietly.

**This is the first release the updater can see**, so v0.4.0 → v0.5.0 will be
its first real trip.

## Also

- The release workflow now takes its notes from the repository and refuses to
  publish a tag whose version disagrees with the launcher's stamp.
- Achievement packs and icons ship inside both release archives.

## Verified

Every subsystem here has a test that runs in CI: the updater (including a real
download and install over a local server), the achievements engine, both secret
encoders — the Python one and the C one, cross-checked against each other on 96
vectors — and the typist, driven against a simulation of the game's own cursor
rules for all 64 symbols. The secret generator's save parsing is checked against
a battery save the game itself wrote.

Two things only a human can confirm, both noted honestly: typing a generated
code into a real linked playthrough, and the self-update path from this release
to the next.
