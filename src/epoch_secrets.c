/* Auto-entered secrets. See epoch_secrets.h.
 *
 * Two halves. The encoder is a straight port of the game's own routines
 * (oracles-disasm code/bank3.s) and matches launcher/oracle_secrets.py
 * symbol for symbol -- tools/secrets_c_test.c checks the two agree.
 *
 * The typist is a feedback controller, not a script. Every frame it
 * reads the entry screen's own cursor position out of WRAM, compares it
 * to where the next symbol lives, and holds one d-pad direction for a
 * frame. When the cursor arrives it taps A. Nothing is ever written
 * into the game's memory: if the cart moves the cursor for its own
 * reasons, the next frame simply steers from wherever it ended up.
 *
 * The grid (US) is 13 columns x 5 rows of symbols, cursorPos packed as
 * row<<4 | column, with the row of options (left/right/back/OK) living
 * at cursorPos >= $50. Left/right wrap within a row; up/down move a row
 * and drop into the options row past the bottom.
 */
#include "epoch_secrets.h"

#include "platform_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) \
    do { fprintf(stderr, "[secrets] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ------------------------------------------------------------------ */
/* tables (bank3.s)                                                    */
/* ------------------------------------------------------------------ */

static const uint8_t XOR_CIPHER[48] = {
    0x15, 0x23, 0x2E, 0x04, 0x0D, 0x3F, 0x1A, 0x10,
    0x3A, 0x2F, 0x1E, 0x20, 0x0F, 0x3E, 0x36, 0x37,
    0x09, 0x29, 0x3B, 0x31, 0x02, 0x16, 0x3D, 0x38,
    0x28, 0x13, 0x34, 0x32, 0x01, 0x0B, 0x0A, 0x35,
    0x0E, 0x1B, 0x12, 0x2C, 0x21, 0x2D, 0x25, 0x30,
    0x19, 0x2A, 0x06, 0x39, 0x3C, 0x17, 0x33, 0x18,
};

/* The printable grid, in symbol-value order. Only the ASCII ones can be
 * spelled in a C string; the specials are placeholders for the menu's
 * preview text (the typist works on values, never characters). */
static const char SYMBOL_CHARS[65] =
    "BDFGHJLM" "shdc#"
    "NQRSTWY!" "otq+-"
    "bdfghjm" "$*/:~"
    "nqrstwy" "?%&<=>"
    "23456789" "^v[]@";

/* (c6xx offset, bit count) for each field, in push order. */
typedef struct { uint8_t off, bits; } EsField;

static const EsField GAME_FIELDS[] = {
    {0x13, 1}, {0x11, 1}, {0x02, 8}, {0x09, 8}, {0x03, 8}, {0x0A, 8},
    {0x0F, 6}, {0x04, 8}, {0x0B, 8}, {0x15, 1}, {0x05, 8}, {0x10, 4},
    {0x06, 8}, {0x0C, 8}, {0x12, 1}, {0x0D, 8}, {0x07, 2},
};
static const EsField RING_FIELDS[] = {
    {0x17, 8}, {0x1B, 8}, {0x1D, 8}, {0x19, 8},
    {0x16, 8}, {0x1A, 8}, {0x18, 8}, {0x1C, 8}, {0x07, 2},
};

static int type_length(EsType type) {
    switch (type) {
    case ES_TYPE_GAME:  return 20;
    case ES_TYPE_RING:  return 15;
    case ES_TYPE_SHORT: return 5;
    }
    return 0;
}

char es_symbol_char(uint8_t value) {
    /* NUL for out of range, not '?': '?' is itself symbol 45. */
    return value < 64 ? SYMBOL_CHARS[value] : '\0';
}

/* ------------------------------------------------------------------ */
/* encoder                                                             */
/* ------------------------------------------------------------------ */

/* The bit stream is built MSB-first into a 20-cell array of 6-bit
 * values; each field is pushed LSB-first, exactly as the game shifts
 * them in. */
typedef struct {
    uint8_t cells[ES_MAX_SYMBOLS];
    int     bit;                 /* next bit index, from the front */
} EsStream;

static void push_bits(EsStream* s, uint32_t value, int nbits) {
    for (int i = 0; i < nbits; i++) {
        const int bit = (value >> i) & 1;
        const int cell = s->bit / 6;
        const int shift = 5 - (s->bit % 6);
        if (cell < ES_MAX_SYMBOLS && bit) s->cells[cell] |= (uint8_t)(1 << shift);
        s->bit++;
    }
}

static uint8_t cipher_index(uint16_t game_id, int short_index) {
    const uint8_t base = (uint8_t)((game_id & 0xFF) + ((game_id >> 8) & 0xFF));
    if (short_index < 0) return base & 0x07;
    const uint8_t mixed = (uint8_t)(((short_index >> 4) & 0x0F) + base);
    return (uint8_t)((mixed ^ ((short_index & 1) << 2)) & 0x07);
}

static void apply_cipher(uint8_t* cells, int count) {
    const int off = ((cells[0] >> 3) & 0x07) * 4;
    cells[0] ^= (uint8_t)(XOR_CIPHER[off] & 0x07);
    for (int i = 1; i < count; i++) cells[i] ^= XOR_CIPHER[off + i];
}

int es_encode(const uint8_t* c6, EsType type, uint8_t short_index,
              uint8_t* out) {
    const int count = type_length(type);
    if (count == 0) return 0;

    const uint16_t game_id = (uint16_t)(c6[0x00] | ((c6[0x01] & 0x7F) << 8));
    const uint8_t cipher = cipher_index(
        game_id, type == ES_TYPE_SHORT ? short_index : -1);

    EsStream s;
    memset(&s, 0, sizeof(s));
    push_bits(&s, cipher, 3);
    push_bits(&s, (uint32_t)type & 3, 2);
    push_bits(&s, game_id & 0xFF, 8);
    push_bits(&s, (game_id >> 8) & 0x7F, 7);

    if (type == ES_TYPE_SHORT) {
        push_bits(&s, short_index, 6);
    } else if (type == ES_TYPE_RING) {
        for (size_t i = 0; i < sizeof(RING_FIELDS) / sizeof(*RING_FIELDS); i++)
            push_bits(&s, c6[RING_FIELDS[i].off], RING_FIELDS[i].bits);
    } else {
        /* generateGameTransferSecret's transform: a first playthrough's
         * secret starts a linked game; a linked or hero file's secret
         * is a hero's secret instead. */
        const uint8_t linked = c6[0x12] & 1, hero = c6[0x13] & 1;
        const uint8_t enc_hero = (uint8_t)(linked | hero);
        const uint8_t enc_linked = (uint8_t)(((linked | hero) ^ 1) | hero);
        for (size_t i = 0; i < sizeof(GAME_FIELDS) / sizeof(*GAME_FIELDS); i++) {
            const uint8_t off = GAME_FIELDS[i].off;
            const uint8_t value = (off == 0x12) ? enc_linked
                                : (off == 0x13) ? enc_hero
                                : c6[off];
            push_bits(&s, value, GAME_FIELDS[i].bits);
        }
    }
    s.bit += 4;                              /* checksum slot */
    if (s.bit != count * 6) {
        LOG("type %d packed %d bits, want %d", (int)type, s.bit, count * 6);
        return 0;
    }

    uint8_t sum = 0;
    for (int i = 0; i < count; i++) sum = (uint8_t)(sum + s.cells[i]);
    s.cells[count - 1] |= (uint8_t)(sum & 0x0F);

    apply_cipher(s.cells, count);
    memcpy(out, s.cells, (size_t)count);
    return count;
}

bool es_decode(const uint8_t* cells, int count, int* type, int* game_id) {
    if (count != 5 && count != 15 && count != 20) return false;
    uint8_t plain[ES_MAX_SYMBOLS];
    memcpy(plain, cells, (size_t)count);
    apply_cipher(plain, count);              /* self-inverse */

    const uint8_t checksum = plain[count - 1] & 0x0F;
    plain[count - 1] &= 0x30;
    uint8_t sum = 0;
    for (int i = 0; i < count; i++) sum = (uint8_t)(sum + plain[i]);
    if ((sum & 0x0F) != checksum) return false;

    int bit = 3;                             /* skip the cipher index */
    int value = 0;
    for (int i = 0; i < 2; i++, bit++)
        value |= ((plain[bit / 6] >> (5 - bit % 6)) & 1) << i;
    if (type) *type = value;
    value = 0;
    for (int i = 0; i < 15; i++, bit++)
        value |= ((plain[bit / 6] >> (5 - bit % 6)) & 1) << i;
    if (game_id) *game_id = value;
    return true;
}

/* ------------------------------------------------------------------ */
/* the entry screen                                                    */
/* ------------------------------------------------------------------ */

/* wFileSelect union, shared by both carts. */
#define A_mode                 0xCBB3
#define A_mode2                0xCBB4
#define A_textInputMode        0xCBB7
#define A_textInputMaxCursorPos 0xCBB8
#define A_cursorPos            0xCBBC
#define A_cursorPos2           0xCBBD
#define A_textInputCursorPos   0xCBBE

#define GRID_COLS 13
#define GRID_ROWS 5
#define GRID_BLANK 0xFF

/* The symbol grid as drawn (bank0.s secretSymbols, laid out 13 wide).
 * Note row 2: the list has only twelve entries there, so the seventh
 * cell is blank -- symbol value is NOT simply row*13+col past that
 * point, which is exactly the sort of thing that would have the typist
 * pressing A on the wrong character for the second half of every code. */
static const uint8_t GRID[GRID_ROWS][GRID_COLS] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12 },
    { 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25 },
    { 26, 27, 28, 29, 30, 31, GRID_BLANK, 32, 33, 34, 35, 36, 37 },
    { 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50 },
    { 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63 },
};

static uint8_t wram0(GBContext* ctx, uint16_t addr) {
    return ctx->wram[addr - 0xC000];
}

static bool symbol_cell(uint8_t value, int* row, int* col) {
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            if (GRID[r][c] == value) { *row = r; *col = c; return true; }
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* the typist                                                          */
/* ------------------------------------------------------------------ */

/* Active-low, matching the runtime's joypad globals. */
#define BTN_A     0x01
#define DPAD_R    0x01
#define DPAD_L    0x02
#define DPAD_U    0x04
#define DPAD_D    0x08

/* One frame pressed, one released: the game reads edges through
 * getInputWithAutofire, so a held button would register once and then
 * autofire unpredictably. */
#define HOLD_FRAMES    2
#define GAP_FRAMES     2
#define STALL_LIMIT    240   /* ~4 s without progress: give up */

static struct {
    uint8_t cells[ES_MAX_SYMBOLS];
    int     count;
    int     done;             /* symbols entered so far */
    bool    active;
    int     hold;             /* frames left holding hold_dpad/buttons */
    int     gap;              /* frames left with nothing pressed */
    uint8_t hold_dpad, hold_buttons;
    int     stall;
    char    message[96];
} g;

bool epoch_secrets_busy(void) { return g.active; }

void epoch_secrets_cancel(void) {
    if (g.active) snprintf(g.message, sizeof(g.message), "Cancelled");
    g.active = false;
}

bool epoch_secrets_type(const uint8_t* cells, int count) {
    if (g.active) return false;
    if (count <= 0 || count > ES_MAX_SYMBOLS) return false;
    memset(&g, 0, sizeof(g));
    memcpy(g.cells, cells, (size_t)count);
    g.count = count;
    g.active = true;
    snprintf(g.message, sizeof(g.message), "Typing %d symbols...", count);
    return true;
}

void epoch_secrets_status(int* done, int* total, const char** message) {
    if (done) *done = g.done;
    if (total) *total = g.count;
    if (message) *message = g.message;
}

bool epoch_secrets_type_from_wram(GBContext* ctx, EsType type,
                                  uint8_t short_index) {
    if (!ctx || !ctx->wram) return false;
    uint8_t cells[ES_MAX_SYMBOLS];
    const int n = es_encode(&ctx->wram[0x0600], type, short_index, cells);
    if (n == 0) return false;
    return epoch_secrets_type(cells, n);
}

/* Is a text-input screen up and taking input? wFileSelect.mode2 == 1 is
 * the state that runs runTextInput; textInputMode's top bit marks the
 * secret grid rather than the name grid. */
static bool entry_screen_ready(GBContext* ctx) {
    return wram0(ctx, A_mode2) == 1 && (wram0(ctx, A_textInputMode) & 0x80);
}

/* The Esc menu draws without a context; the tick has one every frame. */
static GBContext* g_ctx = NULL;

bool epoch_secrets_type_current(EsType type, uint8_t short_index) {
    return epoch_secrets_type_from_wram(g_ctx, type, short_index);
}

bool epoch_secrets_have_save(void) {
    if (!g_ctx || !g_ctx->wram) return false;
    /* wLinkMaxHealth nonzero means a file's data is really in WRAM.
     * Ages keeps it at $c6ab, Seasons at $c6a3; either will do here. */
    return wram0(g_ctx, 0xC6AB) != 0 || wram0(g_ctx, 0xC6A3) != 0;
}

void epoch_secrets_tick(GBContext* ctx, const char* game_id) {
    (void)game_id;
    g_ctx = ctx;

    /* EPOCH_SECRET_TEST=<index>: once a secret screen appears, type that
     * short secret unprompted. Exists so the typist can be exercised
     * headlessly against a real cart. */
    {
        static bool armed = true;
        const char* want = getenv("EPOCH_SECRET_TEST");
        if (want && armed && ctx && ctx->wram && !g.active &&
            entry_screen_ready(ctx)) {
            armed = false;
            const int idx = (int)strtol(want, NULL, 0);
            if (epoch_secrets_type_from_wram(ctx, ES_TYPE_SHORT,
                                             (uint8_t)idx)) {
                LOG("test: typing short secret %02X", idx);
            }
        }
    }
    if (!g.active || !ctx || !ctx->wram) return;

    if (!entry_screen_ready(ctx)) {
        /* Wait for the player to open a secret screen, but not forever. */
        if (++g.stall > STALL_LIMIT * 4) {
            snprintf(g.message, sizeof(g.message),
                     "No secret screen open -- cancelled");
            g.active = false;
        }
        return;
    }

    if (g.hold > 0) {
        /* Keep the button down for the whole press window: the globals
         * are rebuilt from the keyboard every poll, so they have to be
         * rewritten every frame we want them held. */
        g_joypad_dpad = g.hold_dpad;
        g_joypad_buttons = g.hold_buttons;
        if (--g.hold == 0) g.gap = GAP_FRAMES;
        return;
    }
    if (g.gap > 0) { g.gap--; return; }      /* released: let it register */

    if (g.done >= g.count) {
        snprintf(g.message, sizeof(g.message),
                 "Typed %d symbols -- press Start to accept", g.count);
        g.active = false;
        return;
    }

    /* Where is the cursor, and where does it need to be? */
    const uint8_t pos = wram0(ctx, A_cursorPos);
    int want_row, want_col;
    if (!symbol_cell(g.cells[g.done], &want_row, &want_col)) {
        snprintf(g.message, sizeof(g.message), "Bad symbol; cancelled");
        g.active = false;
        return;
    }

    uint8_t dpad = 0xFF, buttons = 0xFF;
    if (pos >= 0x50) {
        /* Cursor is down in the options row; come back up to the grid. */
        dpad &= (uint8_t)~DPAD_U;
    } else {
        const int row = (pos >> 4) & 0x0F, col = pos & 0x0F;
        if (row != want_row) {
            dpad &= (uint8_t)~(row < want_row ? DPAD_D : DPAD_U);
        } else if (col != want_col) {
            /* Rows wrap, so step whichever way is shorter. */
            const int right = (want_col - col + GRID_COLS) % GRID_COLS;
            const int left  = (col - want_col + GRID_COLS) % GRID_COLS;
            dpad &= (uint8_t)~(right <= left ? DPAD_R : DPAD_L);
        } else {
            buttons &= (uint8_t)~BTN_A;      /* arrived: tap it */
            g.done++;
            g.stall = 0;
            snprintf(g.message, sizeof(g.message),
                     "Typing... %d/%d", g.done, g.count);
        }
    }

    if (dpad != 0xFF || buttons != 0xFF) {
        g.hold_dpad = dpad;
        g.hold_buttons = buttons;
        g.hold = HOLD_FRAMES;
        g_joypad_dpad = dpad;                /* this frame counts too */
        g_joypad_buttons = buttons;
    }

    /* A cursor that never reaches its target (an unexpected screen, a
     * dialog stealing input) should not spin forever. */
    if (++g.stall > STALL_LIMIT) {
        snprintf(g.message, sizeof(g.message),
                 "Cursor did not move -- cancelled at %d/%d", g.done, g.count);
        g.active = false;
    }
}
