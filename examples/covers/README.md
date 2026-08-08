# Cover art

Drop your own art next to the **game binary** (not in the source tree), in a
folder called `covers/`:

```
build/
  oracles              <- the game binary
  covers/
    tlozooa.png        <- Oracle of Ages panel
    tlozoos.png        <- Oracle of Seasons panel
  roms/
    tlozooa.gbc
    tlozoos.gbc
```

The filename must match the game id exactly: `tlozooa.png` and `tlozoos.png`.
Restart the launcher to pick up a change.

## Dimensions

**Recommended: 1600 x 1600 PNG. Minimum: 1000 x 1000.**

Square, because of how the layout works. Each game gets a diagonal slice of
the window, and the widest that slice ever gets is about 62% of the window
width by its full height — roughly **1.1 : 1**, near enough to square. The
image is scaled to *cover* that slice and centre-cropped, so:

| you supply | what happens |
|---|---|
| square (1:1) | fills the panel, barely any crop — best case |
| landscape (16:9) | fills the height, sides cropped hard |
| portrait (3:4 box art) | fills the width, top and bottom cropped |

Any aspect ratio *works*, it's just a question of how much you lose. Square
loses least.

1600 x 1600 stays sharp with the window maximised on a 4K display. Smaller
than 1000 x 1000 starts to soften once maximised.

## Safe zone

Keep the subject inside the **middle 70%**:

- the **inner** edge is cut by the diagonal seam between the two games
- the **outer** edge sits under the title and the menu, which are backed by
  a dark scrim for legibility

A centred character or crest reads well. A wide landscape with detail near
the edges does not.

## Format

PNG. Transparency is composited over the panel's own colour wash, so a PNG
with alpha blends into the theme rather than punching a hole.

Art is drawn at 78% opacity on the highlighted side and 24% on the other, so
it reads as a backdrop and the menu stays legible. Without a `covers/` file
each panel falls back to its procedural motif — tidal rings and an hourglass
for Ages, the Rod's four-season wheel for Seasons.

## Licensing

`covers/` is gitignored on purpose. Scans and official key art are not ours
to redistribute, so keep them local — they should not end up in a commit or
a release.
