# Epoch & Equinox v0.4.4

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. Windows users can run the launcher exe; on
Linux run `epoch`, or `launcher/epoch_launcher.py` for the UI (needs Python +
PySide6).

Three fixes from a real setup, two of them things earlier releases claimed to
have handled and did not.

## The chase camera chases again

It followed Link's position but held whatever heading you left it at, so
walking south changed nothing about where you were looking from. That was the
shipped default since v0.4.1: the old auto-swing resumed the instant the stick
went still, which made every adjustment feel like a fight, so it went off
entirely — and took the chasing with it.

It trails him again now, with the thing that made the old one unpleasant fixed
rather than switched off:

- touching the stick buys about half a second before the camera starts
  drifting back
- the drift eases in over about a third of a second, so a corner taken at
  speed reads as the view swinging round rather than snapping
- turning on the spot still never moves it — only actually walking does
- **Trail behind Link while walking** in the display menu turns it off for
  anyone who wants the camera parked where they put it

Recentring is unchanged: click the right stick (or **C**) to swing behind him
now, hold it to keep it there.

## The overlay panel goes away when you stop playing

It was supposed to come down twelve seconds after the player stopped writing.
It never did. The guard measured when a sample last arrived, and samples kept
arriving: the player leaves `stream/live.js` on disk when it exits, so
re-reading it succeeds forever and every poll refreshed the timer with the
same hour-old numbers. Only deleting the file took the panel down, which
nothing does.

The player sends a heartbeat now, and liveness is "the numbers moved" rather
than "the file was there". An older player that sends no heartbeat still works
— the overlay falls back to the rest of the payload changing, which it does
the whole time anyone is playing.

## A wrongly-sized browser source says so

An overlay in a source of the wrong size does not scale, it gets cropped: put
a vertical layout in a 1920×1080 source and the mat stops mid-screen with the
stats panel sliced off, which reads as a broken overlay rather than a
misconfigured source. Each layout knows the canvas it was drawn for and now
prints the numbers to type across the top when they disagree.

## Verified

The camera test drives the same entry point the game does — stick input goes
through `voxel_chase_turn` now rather than being read inside the render step —
and covers trailing him round a corner, the pause after the stick, the ease
being an ease and not a snap, and holding still with the drift off. The old
assertions in it were pinning the behaviour that turned out to be the bug.

The panel's staleness was checked in a browser across all three cases: a
current player, one with no heartbeat, and the file going away. The size
warning was checked in both orientations, in both source sizes, and scaled
down.

## Upgrading

The camera and the heartbeat are in the player, so this one needs the new
binary — replacing `stream/` alone will not do it.
