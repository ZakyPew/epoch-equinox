# Stream overlay

`overlay.html` is a 1920×1080 transparent overlay for OBS: project name,
one-line description, and the repository link, plus a "now building" pill
you can change without touching OBS.

## Add it in OBS

1. **Sources → + → Browser**
2. Tick **Local file**, choose `stream/overlay.html`
3. Width **1920**, height **1080**
4. Leave "Shutdown source when not visible" **off** so the ticker keeps polling

That's it — the background is transparent, so it composites over whatever
you're capturing.

## Change the "now building" line mid-stream

Edit `stream/now.txt`, save, and the overlay picks it up within a few
seconds. One line of plain text:

```
chasing down the tree shapes
```

If the file is missing the overlay just keeps showing the last thing it
read, so nothing breaks.

## Notes

- The glyph is the project's own mark (a split disc — two games, one
  world). No game art is used anywhere in the overlay, which keeps it safe
  to put on a thumbnail, a Linktree, or anywhere else.
- Colours are the renderer's own sky palettes: the Ages daylight blue and
  the autumn gold.
