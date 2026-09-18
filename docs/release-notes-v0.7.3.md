# v0.7.3 — Atmosphere

The pass asked for in Discussions: fog that belongs to the place you are
standing, and mist on the ground.

## Fog takes its colour from the environment

Outdoors the chase camera's distance fog was already the sky's own
horizon tone. Indoors it was a fixed dark grey, whatever the room —
which is most of the games. Now a room with no sky borrows its own
palette instead: the renderer averages the tiles on screen and sinks the
result toward dark, so a blue-stone dungeon hazes blue-black, a lava cave
hazes ember, a wooden house hazes warm. The dark backdrop behind the
world wears the same tint, so the haze and the void it dissolves into
finally agree.

## Ground mist

A new **Ground mist** slider (default on, gently) lays a pale veil over
water and low ground in the distance — full over water and floor,
thinning with height, gone on anything bush-height or taller — in the
chase camera and in the tilted dioramas. It starts a step behind the
ground Link stands on, so his surroundings stay crisp; the far lake and
the meadow beyond it sit in it. Outdoors the mist is a shade toward the
sky's clouds; indoors it is the room's palette, lifted.

## The knobs, findable

The fog sliders used to sit unlabelled at the bottom of the voxel menu.
The Esc menu now has an **Atmosphere** section — fog begins, fog
strength, ground mist — with a line saying where the colours come from.
Everything saves to `voxel/tuning.ini` as before.

## A renderer test that needs no cartridge

`tools/vox_synth.c` builds three scenes by hand — a meadow with a lake
under a spring sky, a blue-stone crypt, a red-brick forge — and pushes
them through the real renderer in the chase camera and the 45° diorama.
It checks with numbers that mist lifts the lake and leaves Link's ground
untouched, and that the crypt's far fog is bluer than red while the
forge's is redder than blue. CI runs it on every push: the first
renderer check that does not need a ROM.
