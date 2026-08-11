# Achievement icons

Drop one file per achievement here and the player picks it up — in the
unlock toast and in the Esc menu's Achievements page. No icon means the
built-in gold medal, so partial sets are fine.

## The spec

| | |
|---|---|
| **Path** | `achievements/icons/<cart>/<id>.ppm` |
| **Format** | Binary PPM (`P6`), maxval 255 |
| **Size** | **48×48** (anything up to 48×48 works; smaller is scaled) |
| **Transparency** | Pure magenta `(255, 0, 255)` renders as transparent |
| **Style** | Pixel art reads best — the renderer draws each pixel as a crisp rectangle, no smoothing |

Carts: `tlozooa` (Oracle of Ages), `tlozoos` (Oracle of Seasons).

Converting from PNG is one ImageMagick call:

```sh
magick icon.png -resize 48x48 icon.ppm
```

Preview without earning anything: `EPOCH_TOAST_TEST=1 ./epoch --game tlozooa`
pops a sample toast at boot (it uses the `all-essences` icon if present).

## Wanted list

One icon per id. The two carts share several ids but the icons live in
separate folders, so twins may differ (Ages leans time/blue, Seasons
leans nature/green).

### `tlozooa/` — Oracle of Ages

| file | subject |
|---|---|
| `first-essence.ppm` | a single glowing blue Essence of Time |
| `half-essences.ppm` | four essences clustered |
| `all-essences.ppm` | all eight essences in a ring, radiant |
| `noble-sword.ppm` | an upgraded sword, gleaming |
| `ten-hearts.ppm` | a row of hearts |
| `full-hearts.ppm` | a burnished full heart, crowned |
| `first-ring.ppm` | a single gold ring |
| `ten-rings.ppm` | a pile of colored rings |
| `slayer-100.ppm` | a sword through a slime |
| `slayer-1000.ppm` | a mountain of defeated foes / skull trophy |
| `rupee-fortune.ppm` | an overflowing pile of rupees |
| `flippers.ppm` | swim flippers with water drops |
| `mermaid-suit.ppm` | the mermaid tail suit |
| `harp.ppm` | the Harp of Ages |
| `survivor.ppm` | a shield with a laurel, unscratched |
| `linked-quest.ppm` | two interlinked rings / portals, blue and green |
| `finished.ppm` | a laurel over a sunrise |
| `ganon-ending.ppm` | a dark horned silhouette behind a cracked triangle |
| `hero-file.ppm` | a heroic banner or crest |
| `maku-saved.ppm` | a great tree with a serene face |

### `tlozoos/` — Oracle of Seasons

| file | subject |
|---|---|
| `first-essence.ppm` | a single glowing green Essence of Nature |
| `half-essences.ppm` | four essences clustered |
| `all-essences.ppm` | all eight essences in a ring, radiant |
| `noble-sword.ppm` | an upgraded sword, gleaming |
| `ten-hearts.ppm` | a row of hearts |
| `full-hearts.ppm` | a burnished full heart, crowned |
| `first-ring.ppm` | a single gold ring |
| `ten-rings.ppm` | a pile of colored rings |
| `slayer-100.ppm` | a sword through a slime |
| `slayer-1000.ppm` | a mountain of defeated foes / skull trophy |
| `rupee-fortune.ppm` | an overflowing pile of rupees |
| `rod-of-seasons.ppm` | the Rod of Seasons with four-season sparkles |
| `all-seasons.ppm` | a disc quartered into spring/summer/autumn/winter |
| `flippers.ppm` | swim flippers with water drops |
| `magnet-gloves.ppm` | the polarized magnet gloves |
| `survivor.ppm` | a shield with a laurel, unscratched |
| `linked-quest.ppm` | two interlinked rings / portals, green and blue |
| `finished.ppm` | a laurel over a sunrise |
| `ganon-ending.ppm` | a dark horned silhouette behind a cracked triangle |
| `hero-file.ppm` | a heroic banner or crest |

Mods that add packs (`achievements/<cart>.<name>.txt`) use the same
folders: an icon file named for the achievement id, beside these.
