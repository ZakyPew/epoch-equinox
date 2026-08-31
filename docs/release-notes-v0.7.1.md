# v0.7.1 — The handoff skips the intro

One change, the one you feel: **Continue the Legend now runs flat out.**

v0.7.0's linked-game handoff drove the second cart's menus for you, but
it drove them at normal speed — splash screens, the title, the file
select, the symbol grid, all in real time. Nobody should watch that.

Now the moment the handoff arms, the emulator switches to max speed (the
same lever as the player's own turbo toggle) and stays there for the
whole drive: boot, file select, SECRETS, the twenty symbols, the
confirmations. Normal speed returns the instant the machine finishes —
your linked game starts standing in a room at 60 fps, exactly where
v0.7.0 left you, just seconds after launch instead of the better part of
a minute.

The cancel rule is unchanged and still instant: touch any button while
the machine drives and it stops, restores normal speed, and hands you
the controls.

Under the hood this is a new runtime patch (`gbrt-turbo-api.patch`)
exporting the platform layer's max-speed flag to the host, plus three
call sites in the handoff machine: on at arm, off at success, off at
failure or takeover. The end-to-end test still passes with the same
deterministic finish frame, so turbo compresses wall-clock time without
moving a single input.
