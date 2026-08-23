# Auto-splits

The player watches the game's own memory and splits when you earn the
thing, so nobody has to hit a button mid-run.

It does not keep the time. **LiveSplit** does — most people already have
it open with their PB, their comparisons and their history in it, and
replacing that would be a downgrade. This decides *when*; LiveSplit
decides what that means. The overlay draws the same list if you want it
on screen.

## The files

`splits/<cart>.txt` — `tlozooa` for Ages, `tlozoos` for Seasons. Each
entry is one segment, **in the order written**, and the title is what the
split list shows.

The format is an achievement pack, because "the third essence is in the
bag" is the same question whether it lights a toast or ends a segment —
so everything in [achievements/README.md](../achievements/README.md)
applies here: the same six condition kinds, the same addresses, multiple
`when` lines ANDing together.

```
[feather]
title = Roc's Feather
desc  = Treasure $17
when  = flag c69c 7
```

The routes shipped here are essence routes with the major items in
between. Yours will differ — copy the file, cut what you skip, reorder
what you don't. A cart with no split file simply has no splits, which is
not an error.

### Finding an item's bit

`wObtainedTreasureFlags` is one bit per treasure id, low bit first, at
`$c69a` in Ages and `$c692` in Seasons. For treasure `$17` (Roc's
Feather) in Ages:

```
byte = c69a + ($17 >> 3) = c69c
bit  = $17 & 7           = 7
when = flag c69c 7
```

Treasure ids are in
[constants/common/treasure.s](https://github.com/Stewmath/oracles-disasm/blob/master/constants/common/treasure.s).

## Connecting LiveSplit

1. In LiveSplit: right-click → **Control** → **Start TCP Server**
2. Tick **Send splits to LiveSplit** on the launcher's Stream page

That writes `splits/livesplit.txt`, which the player reads on startup:

```
enabled = 1
port = 16834
```

It talks to `127.0.0.1` only — a run is not something to put on the
network by accident — and it never blocks the emulator: LiveSplit not
running is the normal case, so a failed connection is silent and retried
every couple of seconds. Starting LiveSplit mid-session gets picked up on
its own.

## What will not split you

Two things that would otherwise ruin a run, both checked in
`tools/splits_test.c`:

- **The file select screen.** It loads a save block into memory just to
  draw the preview card, so browsing your files would otherwise fire a
  whole route. Splits are gated on being in an actual room.
- **Loading a save mid-route.** Everything that file already has is not
  something you just did, so those segments are marked as behind you
  without firing. The next real one still splits.

A new file (the game's clock going backwards) starts a new run and sends
LiveSplit a reset. A savestate load nudges that clock back a little and
is deliberately *not* treated as a reset.
