/* Checks the C secret encoder against the Python one.
 *
 * launcher/oracle_secrets.py generates the codes you read off the
 * screen; src/epoch_secrets.c generates the ones the player types for
 * you. Two ports of the same game routine is two chances to get it
 * wrong, so this dumps the C side's output for a set of synthetic saves
 * and tools/secrets_test.py compares it against the Python side.
 *
 * Build like the other tools:
 *
 *   cc -O2 -o secrets_c_test ../tools/secrets_c_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 *
 *   ./secrets_c_test            # self-checks, exits nonzero on failure
 *   ./secrets_c_test --dump     # machine-readable vectors for the python side
 */
#include "epoch_secrets.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

/* The same synthetic saves the python test builds. */
static void make_c6(uint8_t* c6, unsigned game_id, int linked, int hero,
                    const uint8_t* rings) {
    memset(c6, 0, 0x100);
    c6[0x00] = (uint8_t)(game_id & 0xFF);
    c6[0x01] = (uint8_t)((game_id >> 8) & 0x7F);
    memcpy(&c6[0x02], "LINK", 4);
    memcpy(&c6[0x09], "KID", 3);
    c6[0x0F] = 0x15;      /* child status */
    c6[0x10] = 0x0B;      /* animal companion */
    c6[0x11] = 1;         /* which game */
    c6[0x12] = (uint8_t)linked;
    c6[0x13] = (uint8_t)hero;
    c6[0x15] = 1;         /* ring box */
    for (int i = 0; i < 8; i++) c6[0x16 + i] = rings ? rings[i] : 0;
}

static const unsigned GAME_IDS[] = {
    0x0001, 0x1234, 0x2B67, 0x7FFF, 0x00FF, 0x4000, 0x0F0F, 0x7070,
};
static const uint8_t RINGS[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

static void dump(void) {
    uint8_t c6[0x100], cells[ES_MAX_SYMBOLS];
    for (size_t i = 0; i < sizeof(GAME_IDS) / sizeof(*GAME_IDS); i++) {
        for (int linked = 0; linked <= 1; linked++) {
            make_c6(c6, GAME_IDS[i], linked, 0, RINGS);
            struct { const char* name; EsType type; int idx; } cases[] = {
                { "game",  ES_TYPE_GAME,  -1 },
                { "ring",  ES_TYPE_RING,  -1 },
                { "short", ES_TYPE_SHORT, 0x00 },
                { "short", ES_TYPE_SHORT, 0x04 },
                { "short", ES_TYPE_SHORT, 0x20 },
                { "short", ES_TYPE_SHORT, 0x39 },
            };
            for (size_t k = 0; k < sizeof(cases) / sizeof(*cases); k++) {
                const int n = es_encode(c6, cases[k].type,
                                        (uint8_t)(cases[k].idx < 0 ? 0
                                                  : cases[k].idx), cells);
                printf("%04X %d %s %d", GAME_IDS[i], linked, cases[k].name,
                       cases[k].idx);
                for (int c = 0; c < n; c++) printf(" %02d", cells[c]);
                printf("\n");
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        dump();
        return 0;
    }

    uint8_t c6[0x100], cells[ES_MAX_SYMBOLS];

    /* Lengths. */
    make_c6(c6, 0x2B67, 0, 0, RINGS);
    CHECK(es_encode(c6, ES_TYPE_GAME, 0, cells) == 20, "game secret is 20");
    CHECK(es_encode(c6, ES_TYPE_RING, 0, cells) == 15, "ring secret is 15");
    CHECK(es_encode(c6, ES_TYPE_SHORT, 4, cells) == 5, "short secret is 5");

    /* Round trip through the decoder, every cipher slice. */
    for (size_t i = 0; i < sizeof(GAME_IDS) / sizeof(*GAME_IDS); i++) {
        make_c6(c6, GAME_IDS[i], 0, 0, RINGS);
        int type = -1, id = -1;
        const int n = es_encode(c6, ES_TYPE_GAME, 0, cells);
        char name[64];
        snprintf(name, sizeof(name), "game secret round-trips (%04X)",
                 GAME_IDS[i]);
        CHECK(es_decode(cells, n, &type, &id) && type == ES_TYPE_GAME &&
              id == (int)GAME_IDS[i], name);
    }

    /* A flipped symbol must fail the checksum. */
    make_c6(c6, 0x2B67, 0, 0, RINGS);
    int n = es_encode(c6, ES_TYPE_SHORT, 4, cells);
    cells[2] ^= 1;
    CHECK(!es_decode(cells, n, NULL, NULL), "tampered secret is rejected");

    /* Every symbol value must map to a character. */
    {
        int missing = 0;
        for (int v = 0; v < 64; v++)
            if (es_symbol_char((uint8_t)v) == '\0') missing++;
        CHECK(missing == 0, "all 64 symbol values have a character");
        /* '?' is symbol 45, so the out-of-range marker has to be NUL. */
        CHECK(es_symbol_char(45) == '?', "symbol 45 really is a question mark");
        CHECK(es_symbol_char(64) == '\0', "out-of-range value is refused");
    }

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
