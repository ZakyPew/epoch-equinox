# Epoch & Equinox v0.6.3

Prebuilt player. **No game data included** — drop your own ROMs into
`roms/` next to the binary and run it.

One fix, worth shipping fast: **the chase camera's pixel-cube shading no
longer draws a dot grid across the ground.** The sub-texel edge bevel
that looks right in the tilted diorama modes aliases at the chase view's
ray-step granularity — on a large window it smeared into a visible
lattice. The perspective ground and riser faces now shade whole blocks
instead (a per-texel checker that cannot moiré) and let it fade before
the fog takes over. The tilt modes keep the true edge bevel; the *Pixel
cubes* slider still governs everything.

Reported live within the hour of v0.6.2 — thanks for the screenshot.
