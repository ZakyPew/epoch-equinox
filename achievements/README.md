# Achievement packs

An achievement pack is a text file the player watches while a cart runs.
When every condition on an entry holds, a toast slides over the window —
the game itself is never touched, and the emulated machine never knows.

The player loads `achievements/<cart>.txt` (the packs shipped here), plus
any `achievements/<cart>.<anything>.txt` beside them — so a mod adds its
own achievements by dropping in one file, e.g.
`achievements/tlozooa.my-romhack.txt`. Cart ids are `tlozooa` (Ages) and
`tlozoos` (Seasons).

Unlocks are remembered per cart in `states/achievements-<cart>.txt`, one
id per line. It is plain text on purpose: delete a line (or the file) to
earn it again.

## Format

```
[an-id-unique-within-the-cart]
title = What the toast says
desc  = The line under it
when  = byte c6ab >= 40
when  = bcd c61e == 0
```

Multiple `when` lines AND together. A malformed condition drops its whole
entry (with a note on stderr) rather than half-watching it.

### Conditions

| form | meaning |
|---|---|
| `byte ADDR OP N` | one byte at ADDR |
| `word ADDR OP N` | two bytes at ADDR, little-endian |
| `bcd ADDR OP N` | two bytes of packed BCD (the death counter) |
| `bits ADDR OP N` | number of set bits in the byte at ADDR |
| `bitset ADDR LEN OP N` | set bits across LEN bytes from ADDR |
| `flag ADDR BIT` | true when bit BIT (0–7) of ADDR is set |

`ADDR` is a hex WRAM address (`c000`–`dfff`, no `0x` needed); `OP` is one
of `== != >= <= > <`; `N` is decimal. Addresses outside WRAM are refused.

Evaluation is gated on `wLinkMaxHealth` being nonzero, so nothing can
unlock from the title screen or file select.

## Finding addresses

The [oracles disassembly](https://github.com/Stewmath/oracles-disasm)'s
`include/wram.s` names everything; comments like `$c6aa/$c6a2` give the
Ages/Seasons addresses in that order. Useful anchors, common to both
carts: `wRingsObtained` `c616` (8-byte bitset), `wDeathCounter` `c61e`
(BCD), `wTotalEnemiesKilled` `c620` (word), `wTotalRupeesCollected`
`c627` (word).

Treasure flags are one bit per treasure id starting at
`wObtainedTreasureFlags` (`c69a` Ages / `c692` Seasons): treasure `T`
lives at byte `base + T/8`, bit `T%8`, ids in the disassembly's
`constants/common/treasure.s`. That is how `flag c69f 6` means "has the
flippers" in Ages: flippers are treasure `$2e`, so byte `c69a+5`, bit 6.
