# Bundled cover art

Panel art shipped **with the project**, used for every install that doesn't
override it.

```
art/covers/tlozooa.png     Oracle of Ages panel
art/covers/tlozoos.png     Oracle of Seasons panel
```

`ages.png` and `seasons.png` work too, as do `.jpg` and `.webp` — the cart
ids are what the runner reports, but they're not names anyone would guess.

(The directory is `art/`, not `assets/`, on purpose: the runtime creates its
own `assets/<game_id>/` next to the binary for extracted ROM sections, and
having two different `assets/` in play was a trap waiting to happen.)

The launcher looks in three places, most specific first:

1. `covers/<id>.png` next to the binary — a user's own art. Gitignored.
2. `art/covers/<id>.png` — this directory, committed.
3. neither — the built-in procedural motif.

So anything here is a default, and any user can still override it locally
without touching the repo.

## Installing

```sh
python3 tools/install_cover_art.py --ages ages.png --seasons seasons.png
```

That names, centre-crops and resizes both correctly. Add `--local` to write
to `build/covers/` (gitignored) instead, for anything that shouldn't ship.

**Size: 1600 × 1600 PNG, square. Minimum 1000 × 1000.** Keep the subject in
the middle 70%. Full reasoning in [`../../examples/covers/README.md`](../../examples/covers/README.md).

## What can go here

Only art the project is free to redistribute:

- original work you drew or commissioned, with the rights to ship it
- abstract or landscape art that doesn't depict any character or logo

## What can't

Anything derived from the games' own art, regardless of how it was produced:

- box scans, official key art, screenshots, promotional renders
- sprite or tile rips
- **AI-generated images depicting Link, Zelda characters, the Triforce, or
  other recognisable elements.** What matters is what the image shows, not
  what drew it — a generated picture of Link is still derived from Nintendo's
  character.

Abstract AI-generated art (landscapes, an hourglass, a season wheel, no
characters or logos) is fine here. Note that purely AI-generated images are
generally not copyrightable in the US, so the project can ship them but can't
claim ownership of them.

If in doubt, keep it in `covers/` locally instead. Nothing is lost — the
launcher prefers that path anyway.
