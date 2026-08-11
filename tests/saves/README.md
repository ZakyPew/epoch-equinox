# Test saves

Battery saves parked somewhere useful. Drop one next to the binary under
the name the runner expects and it loads like any other save:

| this file | copy to | where it is |
|---|---|---|
| `ages-veran-tower.sav` | `ZELDA NAYRUAZ8E.sav` | Oracle of Ages, inside Veran's Black Tower |
| `seasons-room-of-rites.sav` | `ZELDA DIN.sav` | Oracle of Seasons, the vortex outside the Room of Rites |

```sh
cp tests/saves/ages-veran-tower.sav "build/ZELDA NAYRUAZ8E.sav"
./build/epoch --game tlozooa --voxel 4
```

## What is in them

Both are endgame files, which is what makes them useful — most of what
the player watches for is already true, so a feature can be checked
without playing to it.

| | Ages | Seasons |
|---|---|---|
| Essences | 8 / 8 | 8 / 8 |
| Heart containers | 14 | 16 |
| Rings | 28 / 64 | **64 / 64** |
| Deaths | 0 | 0 |
| Enemies killed | 1000 | 1000 |
| Rupees collected | 9106 | 12865 |
| Linked game | no | **yes** |
| Game ID | `0000` (never used a secret) | `54F3` |

Between them they exercise most of the interesting edges: a full ring
collection against a partial one, a linked file against a plain one, a
Game ID of zero (which the game accepts on any file) against a real one,
and a rupee total either side of the 10,000 achievement threshold.

They are also both parked **inside a dungeon**, which is the one place
the voxel renderer had never been checked.

## Where they came from

Downloaded saves, authored by "Rey" and dated 2001, supplied with the
`.frm` descriptors the archives ship. They contain no game code — a
battery save is the player's own progress, not cartridge data.
