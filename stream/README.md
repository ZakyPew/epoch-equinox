# Stream overlays

Two layouts, both transparent, both driven by the same `now.txt`:

| file | canvas | for |
|---|---|---|
| `overlay.html` | 1920×1080 | Twitch / YouTube landscape |
| `overlay-vertical.html` | 1080×1920 | TikTok, Reels, Shorts, vertical Twitch |

Each carries the project name, a one-line description and the repository
link, plus a "now building" pill you can change without touching OBS.

The vertical layout keeps the middle of the canvas empty for gameplay and a
facecam, and stays out of the two places mobile platforms draw their own UI:
the right-hand button column and the bottom caption strip. Both insets are
CSS variables at the top of the file (`--side-safe`, `--bottom-safe`) if your
platform wants more or less room.

![vertical overlay](../docs/stream-overlay-vertical.png)

![overlay composited over gameplay](../docs/stream-overlay.png)

## Add it in OBS

1. **Sources → + → Browser**
2. Tick **Local file**, choose `stream/overlay.html` (or `overlay-vertical.html`)
3. Width **1920**, height **1080** — or **1080**×**1920** for the vertical one
4. Leave "Shutdown source when not visible" **off** so the ticker keeps polling

That's it — the background is transparent, so it composites over whatever
you're capturing.

## Change the "now building" line mid-stream

Edit `stream/now.js`, save, and the overlay picks it up within a few
seconds. One line:

```js
NOW("chasing down the tree shapes");
```

If the file is missing the overlay keeps its default text, so nothing
breaks.

(It is a `.js` file rather than plain text for an annoying reason: OBS
loads a local overlay over `file://`, and browsers block `fetch()` against
`file://` URLs. Loading a script from the same folder is allowed, so this
is the version that actually works.)

## Cover art in the emblem

The mark in the corner is your two covers as one split disc. Put
`cover-ages.png` and `cover-seasons.png` next to the HTML — square crops,
a few hundred pixels is plenty. Without them the emblem falls back to a
plain gradient and nothing breaks.

## Notes

- The glyph is the project's own mark (a split disc — two games, one
  world). No game art is used anywhere in the overlay, which keeps it safe
  to put on a thumbnail, a Linktree, or anywhere else.
- Colours are the renderer's own sky palettes: the Ages daylight blue and
  the autumn gold.
