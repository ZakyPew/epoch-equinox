# Epoch & Equinox v0.4.2

Prebuilt player. **No game data included** — drop your own ROMs into `roms/`
next to the binary and run it. Windows users can run the launcher exe; on
Linux run `epoch`, or `launcher/epoch_launcher.py` for the UI (needs Python +
PySide6).

A small follow-up to v0.4.1: the stream overlays now show the achievement's
own icon.

## The real icons, on stream

v0.4.1 shipped the live overlays with a medal drawn from scratch on the
unlock card and the "last earned" plaque. The packs have had real icons since
v0.4.0 — they just could not get to the browser, because icons ship as PAM
(a header and raw RGBA, which is what the player reads and no browser does).

They are mirrored to PNG now, so the card and the plaque show the right
picture: the harp for the Harp of Ages, the ring box for the full collection,
and so on. The unlock card shows it at 96×96 — twice the source, told not to
smooth, so the pixels stay pixels at 1080p.

An achievement with no icon still falls back to the drawn medal rather than a
broken image, which is what a mod's own achievement will do.

CI regenerates the mirrors and fails if they have drifted from the originals,
so an icon that gets added or edited can no longer quietly show the fallback
mid-stream.

## Also

- The unlock card grew to fit the larger icon, which crowded the framed
  layout's optional camera opening. The landscape camera is 560×315 now, and
  the vertical layout gives the card the slot below its camera. Every rail
  panel was measured against every other in all four layouts, with and
  without the camera: no overlaps, nothing off-canvas.

## Upgrading from v0.4.1

Nothing in the player changed — this is overlay files and icons. If you have
v0.4.1 unpacked already, replacing its `stream/` folder gets you the same
thing without downloading the archive.
