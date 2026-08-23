"""Oracle secret generation from a battery save.

Secrets in the Oracle games are not universal passwords: every save file
carries a random 15-bit Game ID, and every code is encoded against it
with an XOR cipher and a checksum, so codes from a website will not
validate on your file. This module regenerates them from *your* save —
the same codes the NPCs would speak, spelled in the same symbols.

Everything here is a faithful port of the game's own routines in the
oracles disassembly (code/bank3.s): the bit packer, the per-type field
tables, the XOR cipher table, the checksum, and the file layout that
tells us where a save keeps its c6xx block. No network, no Qt — plain
functions over bytes, so tools/secrets_test.py can drive every path.

A secret is 5, 15 or 20 six-bit symbols:

  [3b cipher idx][2b type][15b game ID][payload...][4b checksum]

The bit stream is built by pushing each field LSB-first; symbols are the
stream chopped into sixes from the front. The whole thing is then XORed
with one of eight 4-byte-offset slices of a fixed table, chosen by the
cipher-index bits (which are themselves left unciphered so the decoder
can find them).
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# tables from the disassembly
# ---------------------------------------------------------------------------

# code/bank0.s secretSymbols (US). Index = the 6-bit value. The game's
# grid deliberately has no lowercase L; the specials are drawn here with
# the unicode the manual uses.
SYMBOLS = (
    list("BDFGHJLM") + list("♠♥♦♣#") +
    list("NQRSTWY!") + list("●▲■+-") +
    list("bdfghjm") + list("$*/:~") +
    list("nqrstwy") + list("?%&<=>") +
    list("23456789") + list("↑↓←→@")
)
assert len(SYMBOLS) == 64

# code/bank3.s secretXorCipher (US region).
XOR_CIPHER = [
    0x15, 0x23, 0x2E, 0x04, 0x0D, 0x3F, 0x1A, 0x10,
    0x3A, 0x2F, 0x1E, 0x20, 0x0F, 0x3E, 0x36, 0x37,
    0x09, 0x29, 0x3B, 0x31, 0x02, 0x16, 0x3D, 0x38,
    0x28, 0x13, 0x34, 0x32, 0x01, 0x0B, 0x0A, 0x35,
    0x0E, 0x1B, 0x12, 0x2C, 0x21, 0x2D, 0x25, 0x30,
    0x19, 0x2A, 0x06, 0x39, 0x3C, 0x17, 0x33, 0x18,
]

# Secret types (wSecretType) and their symbol counts.
TYPE_GAME = 0        # game-transfer ("hero's") secret
TYPE_RING = 2
TYPE_SHORT = 3       # the 5-symbol NPC and return secrets
SECRET_LENGTHS = {0: 20, 1: 20, 2: 15, 3: 5}

# secretDataToEncodeTable payloads: (c6xx offset, bit count), push order.
# The header (cipher idx, type, game ID) is handled inline.
GAME_SECRET_FIELDS = [
    (0x13, 1),   # wFileIsHeroGame (transformed, see below)
    (0x11, 1),   # wWhichGame
    (0x02, 8), (0x09, 8),           # wLinkName[0], wKidName[0]
    (0x03, 8), (0x0A, 8),           # wLinkName[1], wKidName[1]
    (0x0F, 6),                      # wChildStatus
    (0x04, 8), (0x0B, 8),           # wLinkName[2], wKidName[2]
    (0x15, 1),                      # wObtainedRingBox
    (0x05, 8),                      # wLinkName[3]
    (0x10, 4),                      # wAnimalCompanion
    (0x06, 8), (0x0C, 8),           # wLinkName[4], wKidName[3]
    (0x12, 1),                      # wFileIsLinkedGame (transformed)
    (0x0D, 8),                      # wKidName[4]
    (0x07, 2),                      # wLinkName[5], always 0
]
RING_SECRET_FIELDS = [
    (0x17, 8), (0x1B, 8), (0x1D, 8), (0x19, 8),
    (0x16, 8), (0x1A, 8), (0x18, 8), (0x1C, 8),   # wRingsObtained, permuted
    (0x07, 2),                                     # always 0
]

# constants/common/secrets.s: index -> (name, cart the code is typed into).
# The base secrets are spoken by NPCs in one game and typed into the NPC
# named here in the other; each NPC then answers with the +$10 "return"
# secret to carry back to Farore.
AGES_NPC_SECRETS = {          # typed into Oracle of Ages
    0x00: "King Zora", 0x01: "Great Fairy", 0x02: "Troy", 0x03: "Plen",
    0x04: "Library", 0x05: "Tokay", 0x06: "Mamamu Yan", 0x07: "Tingle",
    0x08: "Elder", 0x09: "Symmetry City",
}
SEASONS_NPC_SECRETS = {       # typed into Oracle of Seasons
    0x20: "Clock Shop", 0x21: "Graveyard", 0x22: "Subrosian", 0x23: "Diver",
    0x24: "Smith", 0x25: "Pirate", 0x26: "Temple", 0x27: "Deku Scrub",
    0x28: "Biggoron", 0x29: "Ruul Village",
}
RETURN_OFFSET = 0x10

# ---------------------------------------------------------------------------
# the bit machine
# ---------------------------------------------------------------------------


def _cipher_offset(first_symbol: int) -> int:
    """The table slice, read the way the game reads it: bits 3-5 of the
    first symbol, times four."""
    return ((first_symbol >> 3) & 0x07) * 4


def _apply_cipher(cells: list[int]) -> list[int]:
    """XOR every symbol with the table slice; self-inverse. The first
    symbol keeps its upper three bits so the slice stays findable."""
    off = _cipher_offset(cells[0])
    out = [cells[0] ^ (XOR_CIPHER[off] & 0x07)]
    for i in range(1, len(cells)):
        out.append(cells[i] ^ XOR_CIPHER[off + i])
    return out


def encode_secret(secret_type: int, cipher_idx: int, game_id: int,
                  fields: list[tuple[int, int]]) -> list[int]:
    """Pack header + fields + checksum into symbol values (each 0-63)."""
    n = SECRET_LENGTHS[secret_type & 3]
    pushes = [
        (cipher_idx, 3), (secret_type & 3, 2),
        (game_id & 0xFF, 8), ((game_id >> 8) & 0x7F, 7),
    ] + fields

    stream = 0
    total = 0
    for value, nbits in pushes:
        for i in range(nbits):               # LSB first, like the game
            stream = (stream << 1) | ((value >> i) & 1)
            total += 1
    stream <<= 4                             # checksum placeholder
    total += 4
    if total != n * 6:
        raise ValueError(f"type {secret_type}: {total} bits, want {n * 6}")

    cells = [(stream >> (6 * (n - 1 - c))) & 0x3F for c in range(n)]
    cells[-1] |= sum(cells) & 0x0F
    return _apply_cipher(cells)


def decode_secret(cells: list[int]) -> dict:
    """Un-cipher, verify the checksum, and split out the header. Returns
    {type, game_id, payload_bits} or raises ValueError."""
    n = len(cells)
    if n not in (5, 15, 20):
        raise ValueError(f"a secret has 5, 15 or 20 symbols, not {n}")
    plain = _apply_cipher(list(cells))

    checksum = plain[-1] & 0x0F
    plain[-1] &= 0x30
    if sum(plain) & 0x0F != checksum:
        raise ValueError("checksum mismatch")

    stream = 0
    for c in plain:
        stream = (stream << 6) | c
    bits_left = n * 6

    def read(nbits: int) -> int:
        nonlocal stream, bits_left
        value = 0
        for i in range(nbits):               # first bit out = bit 0
            bits_left -= 1
            value |= ((stream >> bits_left) & 1) << i
        return value

    read(3)                                  # cipher index; already used
    stype = read(2)
    game_id = read(8) | (read(7) << 8)
    payload = [read(1) for _ in range(bits_left - 4)]
    return {"type": stype, "game_id": game_id, "payload_bits": payload}


def to_text(cells: list[int]) -> str:
    """Symbol values -> the string the player reads, spaced in fives."""
    groups = ["".join(SYMBOLS[c] for c in cells[i:i + 5])
              for i in range(0, len(cells), 5)]
    return " ".join(groups)


def from_text(text: str) -> list[int]:
    """The player's string back to symbol values; raises on unknown."""
    cells = []
    for ch in text:
        if ch.isspace():
            continue
        if ch not in SYMBOLS:
            raise ValueError(f"not a secret symbol: {ch!r}")
        cells.append(SYMBOLS.index(ch))
    return cells


# ---------------------------------------------------------------------------
# a save file's view of all this
# ---------------------------------------------------------------------------

FILE_SIZE = 0x550
SLOT_OFFSETS = [0x010, 0x560, 0xAB0]          # getFileAddress1, minus $a000
BACKUP_OFFSETS = [0x1000, 0x1550, 0x1AA0]
VERIFICATION = {b"Z21216-0": "ages", b"Z11216-0": "seasons"}
C6_BASE = 0x50                                 # wc600Block - wFileStart


@dataclass
class SaveFile:
    slot: int                  # 0-2
    game: str                  # "ages" / "seasons"
    hero_name: str
    game_id: int
    c6: bytes                  # the $100-byte c6xx block

    @property
    def is_linked(self) -> bool:
        return self.c6[0x12] != 0

    @property
    def is_hero(self) -> bool:
        return self.c6[0x13] != 0

    # Progress, read at the same offsets the player's stream feed reads
    # them live (src/epoch_stream.c, addresses from the disassembly's
    # include/wram.s; the c6 block is WRAM $c6xx, so offset = addr-$c600).
    # All of them verified against the real endgame saves in tests/saves.

    @property
    def essences(self) -> int:
        """wEssencesObtained is a bitmask, at $c6bf (Ages) / $c6bb."""
        off = 0xBF if self.game == "ages" else 0xBB
        return bin(self.c6[off]).count("1")

    @property
    def hearts(self) -> int:
        """wLinkMaxHealth counts quarter-hearts, at $c6ab / $c6a3."""
        off = 0xAB if self.game == "ages" else 0xA3
        return self.c6[off] // 4

    @property
    def deaths(self) -> int:
        """wDeathCounter: two bytes of packed BCD, low byte first."""
        lo, hi = self.c6[0x1E], self.c6[0x1F]
        return ((hi >> 4) * 1000 + (hi & 0xF) * 100
                + (lo >> 4) * 10 + (lo & 0xF))

    @property
    def rupees_collected(self) -> int:
        """wTotalRupeesCollected, a little-endian word -- lifetime, not
        the purse."""
        return self.c6[0x27] | (self.c6[0x28] << 8)

    @property
    def playtime(self) -> str:
        """wPlaytimeCounter counts frames at ~60 Hz; shown as h:mm."""
        seconds = int.from_bytes(self.c6[0x22:0x26], "little") // 60
        return f"{seconds // 3600}:{seconds // 60 % 60:02d}"


def _decode_name(raw: bytes) -> str:
    """Names are stored in the game's text encoding, which keeps ASCII
    for the characters a name can contain."""
    out = ""
    for b in raw:
        if b == 0:
            break
        out += chr(b) if 0x20 <= b < 0x7F else "?"
    return out


def _file_checksum(block: bytes) -> int:
    total = 0
    for i in range(2, 2 + 0x2A7 * 2, 2):
        total += block[i] | (block[i + 1] << 8)
    return total & 0xFFFF


def read_save(path: Path) -> list[SaveFile]:
    """Every valid file slot in a .sav, primary copies preferred."""
    data = path.read_bytes()
    saves = []
    for slot in range(3):
        for base in (SLOT_OFFSETS[slot], BACKUP_OFFSETS[slot]):
            if base + FILE_SIZE > len(data):
                continue
            block = data[base:base + FILE_SIZE]
            game = VERIFICATION.get(bytes(block[2:10]))
            if game is None:
                continue
            stored = block[0] | (block[1] << 8)
            if stored != _file_checksum(block):
                continue
            c6 = block[C6_BASE:C6_BASE + 0x100]
            saves.append(SaveFile(
                slot=slot,
                game=game,
                hero_name=_decode_name(c6[0x02:0x08]) or f"File {slot + 1}",
                game_id=c6[0x00] | (c6[0x01] << 8),
                c6=c6,
            ))
            break                              # primary copy was good
    return saves


def find_saves(root: Path) -> dict[str, Path]:
    """The runner keeps '<internal title>.sav' beside the binary."""
    found = {}
    for path in sorted(root.glob("*.sav")):
        upper = path.stem.upper()
        if upper.startswith("ZELDA NAYRU"):
            found["tlozooa"] = path
        elif upper.startswith("ZELDA DIN"):
            found["tlozoos"] = path
    return found


# ---------------------------------------------------------------------------
# the generators
# ---------------------------------------------------------------------------


def _default_cipher_idx(game_id: int, short_index: int | None = None) -> int:
    """@determineXorCipher: vary the cipher with the game ID (and, for
    5-symbol secrets, with which secret it is)."""
    base = ((game_id & 0xFF) + ((game_id >> 8) & 0xFF)) & 0xFF
    if short_index is None:
        return base & 0x07
    mixed = (((short_index >> 4) & 0x0F) + base) & 0xFF
    return (mixed ^ (((short_index & 1) << 2))) & 0x07


def game_secret(save: SaveFile) -> list[int]:
    """The 20-symbol game-transfer secret Farore gives at the end.

    generateGameTransferSecret's transform: the secret from a first
    playthrough is marked "linked" (it starts the linked game); from a
    linked or hero file it is marked as a hero's secret instead.
    """
    linked, hero = save.c6[0x12] & 1, save.c6[0x13] & 1
    enc_hero = linked | hero
    enc_linked = ((linked | hero) ^ 1) | hero

    fields = []
    for off, nbits in GAME_SECRET_FIELDS:
        if off == 0x12:
            value = enc_linked
        elif off == 0x13:
            value = enc_hero
        else:
            value = save.c6[off]
        fields.append((value, nbits))
    return encode_secret(TYPE_GAME, _default_cipher_idx(save.game_id),
                         save.game_id, fields)


def ring_secret(save: SaveFile) -> list[int]:
    fields = [(save.c6[off], nbits) for off, nbits in RING_SECRET_FIELDS]
    return encode_secret(TYPE_RING, _default_cipher_idx(save.game_id),
                         save.game_id, fields)


def short_secret(save: SaveFile, index: int) -> list[int]:
    return encode_secret(TYPE_SHORT,
                         _default_cipher_idx(save.game_id, index),
                         save.game_id, [(index, 6)])


def ring_count(save: SaveFile) -> int:
    return sum(bin(b).count("1") for b in save.c6[0x16:0x1E])
