#!/usr/bin/env python3
"""Checks for the Oracle secret generator. No network, no pytest:

    python3 tools/secrets_test.py

The encoder is validated by round-tripping through the decoder (both are
independent ports of the game's encode and unpack paths), by tamper
checks, and — when a battery save the game itself wrote is available in
build/ — by parsing that real save, which proves the file layout and
file checksum against ground truth.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "launcher"))

import oracle_secrets as osec  # noqa: E402

FAILURES: list[str] = []


def check(label: str, got, want=True) -> None:
    if got != want:
        FAILURES.append(f"{label}: got {got!r}, wanted {want!r}")


def check_raises(label: str, fn) -> None:
    try:
        fn()
    except ValueError:
        return
    FAILURES.append(f"{label}: no ValueError raised")


def fields_from_payload(bits: list[int], table) -> list[int]:
    """Reassemble field values from decoded payload bits (pushed
    LSB-first, so the first bit of a field is its bit 0)."""
    out, pos = [], 0
    for _, nbits in table:
        value = 0
        for i in range(nbits):
            value |= bits[pos] << i
            pos += 1
        out.append(value)
    return out


def make_save(game_id: int, **over) -> osec.SaveFile:
    c6 = bytearray(0x100)
    c6[0x00], c6[0x01] = game_id & 0xFF, (game_id >> 8) & 0x7F
    for i, ch in enumerate(b"LINK\x00"):
        c6[0x02 + i] = ch
    for i, ch in enumerate(b"KID\x00"):
        c6[0x09 + i] = ch
    c6[0x0F] = over.get("child", 0x15)
    c6[0x10] = over.get("animal", 0x0B)
    c6[0x11] = over.get("which_game", 1)
    c6[0x12] = over.get("linked", 0)
    c6[0x13] = over.get("hero", 0)
    c6[0x15] = over.get("ring_box", 1)
    for i, b in enumerate(over.get("rings", bytes(8))):
        c6[0x16 + i] = b
    return osec.SaveFile(slot=0, game="ages", hero_name="LINK",
                         game_id=game_id, c6=bytes(c6))


def main() -> int:
    check("64 distinct symbols", len(set(osec.SYMBOLS)), 64)

    # -- round trips over all cipher slices ---------------------------
    for game_id in (0x0001, 0x1234, 0x2B67, 0x7FFF, 0x00FF, 0x4000,
                    0x0F0F, 0x7070):
        save = make_save(game_id)
        cells = osec.game_secret(save)
        check(f"game secret length ({game_id:#x})", len(cells), 20)
        got = osec.decode_secret(osec.from_text(osec.to_text(cells)))
        check(f"game id survives ({game_id:#x})", got["game_id"], game_id)
        check(f"type survives ({game_id:#x})", got["type"], osec.TYPE_GAME)

    # -- the payload comes back byte-for-byte -------------------------
    save = make_save(0x2B67, rings=bytes([1, 2, 3, 4, 5, 6, 7, 8]))
    got = osec.decode_secret(osec.ring_secret(save))
    values = fields_from_payload(got["payload_bits"],
                                 osec.RING_SECRET_FIELDS)
    expect = [save.c6[off] for off, _ in osec.RING_SECRET_FIELDS]
    check("ring bytes round-trip in table order", values, expect)
    check("ring count", osec.ring_count(save), 13)   # popcounts of 1..8

    # The game-transfer transform: a first playthrough's secret is
    # marked linked; a linked or hero file's secret is marked hero.
    got = osec.decode_secret(osec.game_secret(make_save(0x11)))
    vals = fields_from_payload(got["payload_bits"], osec.GAME_SECRET_FIELDS)
    hero, linked = vals[0], vals[14]
    check("first playthrough -> linked secret", (hero, linked), (0, 1))
    got = osec.decode_secret(osec.game_secret(make_save(0x11, linked=1)))
    vals = fields_from_payload(got["payload_bits"], osec.GAME_SECRET_FIELDS)
    check("linked file -> hero secret", (vals[0], vals[14]), (1, 0))

    # -- short secrets: every NPC index, both carts -------------------
    for index in list(osec.AGES_NPC_SECRETS) + list(osec.SEASONS_NPC_SECRETS):
        for idx2 in (index, index + osec.RETURN_OFFSET):
            cells = osec.short_secret(save, idx2)
            check(f"short secret {idx2:#04x} length", len(cells), 5)
            got = osec.decode_secret(cells)
            value = fields_from_payload(got["payload_bits"], [(0, 6)])[0]
            check(f"short secret {idx2:#04x} index", value, idx2)
            check(f"short secret {idx2:#04x} game id",
                  got["game_id"], save.game_id)

    # -- tampering ----------------------------------------------------
    cells = osec.short_secret(save, 0x04)
    bad = list(cells)
    bad[2] ^= 0x01
    check_raises("flipped symbol fails the checksum",
                 lambda: osec.decode_secret(bad))
    check_raises("wrong length rejected",
                 lambda: osec.decode_secret(cells[:4]))
    check_raises("junk text rejected", lambda: osec.from_text("BDFGl"))
    check("spaced text accepted",
          osec.from_text(osec.to_text(cells)), cells)

    # Distinct ciphers actually vary the visible text.
    texts = {osec.to_text(osec.short_secret(make_save(g), 0x04))
             for g in range(1, 9)}
    check("cipher varies with game id", len(texts) > 1)

    # -- the C encoder must agree symbol for symbol -------------------
    # src/epoch_secrets.c generates the codes the player types for you;
    # this module generates the ones you read. Two ports of one game
    # routine is two chances to be wrong, so compare them directly.
    import subprocess
    build = Path(__file__).resolve().parent.parent / "build"
    c_test = build / "secrets_c_test"
    if not c_test.exists():
        print("[secrets_test] note: build/secrets_c_test not built; "
              "C/Python cross-check skipped")
    else:
        out = subprocess.run([str(c_test), "--dump"], capture_output=True,
                             text=True, cwd=build)
        rows = [l.split() for l in out.stdout.splitlines() if l.strip()]
        check("C dump produced vectors", len(rows) > 0)
        for row in rows:
            game_id, linked, kind, index = int(row[0], 16), int(row[1]), row[2], int(row[3])
            cells = [int(v) for v in row[4:]]
            save = make_save(game_id, linked=linked,
                             rings=bytes([1, 2, 3, 4, 5, 6, 7, 8]))
            if kind == "game":
                mine = osec.game_secret(save)
            elif kind == "ring":
                mine = osec.ring_secret(save)
            else:
                mine = osec.short_secret(save, index)
            check(f"C == Python for {kind} {index:#04x} id {game_id:#06x} "
                  f"linked={linked}", mine, cells)

    # -- ground truth: real battery saves ------------------------------
    # tests/saves holds two endgame files (see its README); anything the
    # runner has written into build/ is checked too.
    root = Path(__file__).resolve().parent.parent
    real = dict(osec.find_saves(root / "build"))
    for name, cart in (("ages-veran-tower.sav", "tlozooa"),
                       ("seasons-room-of-rites.sav", "tlozoos")):
        p = root / "tests" / "saves" / name
        if p.exists():
            real[cart] = p
    if not real:
        print("[secrets_test] note: no real .sav beside build/; "
              "ground-truth checks skipped")
    for cart, path in real.items():
        saves = osec.read_save(path)
        print(f"[secrets_test] {path.name}: {len(saves)} valid file(s)")
        for s in saves:
            check(f"{path.name} slot {s.slot} game matches cart",
                  s.game, "ages" if cart == "tlozooa" else "seasons")
            # Generate + decode everything against the real file.
            got = osec.decode_secret(osec.game_secret(s))
            check(f"{path.name} slot {s.slot} game secret round-trips",
                  got["game_id"], s.game_id)
            got = osec.decode_secret(osec.ring_secret(s))
            check(f"{path.name} slot {s.slot} ring secret round-trips",
                  got["game_id"], s.game_id)

    if FAILURES:
        print(f"[secrets_test] {len(FAILURES)} check(s) failed:")
        for failure in FAILURES:
            print(f"  - {failure}")
        return 1
    print("[secrets_test] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
