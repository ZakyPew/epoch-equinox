# Epoch & Equinox v0.7.0

Prebuilt player. **No game data included** — drop your own ROMs into
`roms/` next to the binary and run it.

## The two games are one quest now

The Oracles were designed as halves of a single legend: finish one,
carry a twenty-symbol secret to the other, and only the linked quest
reaches the true ending. That carry — pause, transcribe, swap, retype —
is the one seam this player still had.

**Continue Legend**, in the launcher menu. Pick it on the game you
finished, and the other game starts and does the rest itself: walks its
own file select, chooses SECRETS on a free file, types your transfer
secret by driving the game's cursor — the same typist the Esc menu
uses, no memory writes, the game wins every disagreement — accepts it,
and stops only when your linked game is standing in a room. From Ages'
first tree to Ganon, no paper involved.

Touch nothing while it types; pressing any button hands control back to
you and stops the machine, because hands beat automation. If the free
slots are full or the other ROM is missing, it says so instead of
guessing. Ring secrets still transfer the old way (Vasu's, in-game,
where the panel already types them for you).

## Under it

`states/handoff.txt` is the whole interface between the launcher and
the in-game machine — a plain file naming the target, the free slot and
the symbols, consumed on success and left for retry on failure. The
machine itself keys only on state the carts keep honest (the secret
grid's own flags, `wScrollMode`), because the file-select scratch bytes
turned out to be reused as counters during the intro — the probe that
learned this ships in `tools/handoff_test.c`.

## Verified

`tools/handoff_test.c` runs the entire journey headlessly against the
real carts: encode a game secret from the finished Ages save in
`tests/saves`, boot Seasons cold, let the machine drive, and ask the
cart itself for the verdict — `wFileIsLinkedGame` set, a room live, the
handoff consumed. Twice, reproducibly, in ~2,400 frames. (It needs your
ROMs, so it runs locally rather than in CI; the encoder and typist it
builds on keep their CI suites.)

Also since v0.6.3: nothing — this release is the seam.
