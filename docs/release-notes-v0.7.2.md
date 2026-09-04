# v0.7.2 — Continue the Legend, made tight

A hardening pass on the linked-game handoff before anything gets built
on top of it. Nine end-to-end scenarios now run against both real carts
— both directions, every file slot, a cart that has never been saved, a
slot that turns out to be occupied, and the takeover rules — and one of
them caught a bug v0.7.0 and v0.7.1 shipped with.

## The bug

After the other cart accepts your secret it writes the linked file and
returns to the **file select with the cursor on file 1** — not on the
file it just made. The old machine pressed A there. If file 1 was empty
that was fine; if it already held a game (you had played the other
cart before), your *old* game started, with the new linked file sitting
unopened in file 2 or 3. The test that passed at the time was fooled the
same way: the neighbouring file happened to be linked and named Link
too. The machine now reads the file cursor back from the cart and steps
it to the slot it linked into before pressing A, then pages through the
linked prologue until Link is standing in a room. The test now checks
the file that started carries the sender's Game ID and has no playtime —
something only the new file can satisfy.

## The rest of the pass

- **A banner.** The handoff's messages were never drawn. Now a strip in
  the top-right says what the machine is doing (waking the cart, opening
  file N, choosing SECRETS, typing, accepting, opening the linked file),
  then the verdict for a few seconds after it stops.
- **One shot.** `states/handoff.txt` is consumed the moment it is read,
  not on success. A handoff that stopped halfway — you took over, a menu
  was not where it should be — no longer lies in wait and drives the
  cart again on some later launch.
- **Occupied slot.** If the slot the launcher called free holds a game
  after all, the cart loads it instead of showing SECRETS. The machine
  now notices a room going live before it should and stops at once, with
  your own file on screen and normal speed restored.
- **Held keys.** The Enter or A that confirmed the launcher's dialog can
  still be down when the game window opens. That no longer counts as a
  takeover; you have to let go once before a press means anything.
- **Frame-counted.** The machine advances on the emulator's own frame
  counter rather than on polls, so the runner's frame slicing can never
  shift a timed press.

`tools/handoff_test.c` documents the scenarios (`HANDOFF_TO`,
`HANDOFF_SLOT`, `HANDOFF_FRESH`, `HANDOFF_EXPECT`) and a `HANDOFF_TRACE`
mode that prints the file-select state the machine keys on.
