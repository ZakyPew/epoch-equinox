/* Exercises the secret typist without a cart.
 *
 * The typist steers the game's cursor by reading wFileSelect.cursorPos
 * and pressing one direction a frame. This harness stands in for the
 * game: it holds a fake WRAM, applies the *game's own* cursor rules
 * (ported from bank2.s runTextInput) to whatever buttons the typist
 * presses, and records which symbol each A press would have entered.
 *
 * If the typist's idea of the grid disagrees with the game's, the
 * recorded symbols come out wrong -- which is exactly the failure that
 * would be invisible until someone typed a 20-symbol code by hand.
 *
 * Build like the other tools:
 *
 *   cc -O2 -o typist_test ../tools/typist_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 */
#include "epoch_secrets.h"
#include "platform_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

#define A_mode2           0xCBB4
#define A_textInputMode   0xCBB7
#define A_cursorPos       0xCBBC

/* The grid as the game draws it: 13 wide, 5 rows, with the blank in
 * row 2. Independently written here so a typo in the module is not
 * silently mirrored by the test. */
static int grid_symbol(int row, int col) {
    static const int ROW_START[5] = { 0, 13, 26, 38, 51 };
    if (row < 0 || row > 4 || col < 0 || col > 12) return -1;
    if (row == 2) {
        if (col == 6) return -1;               /* the blank cell */
        return ROW_START[2] + (col < 6 ? col : col - 1);
    }
    return ROW_START[row] + col;
}

/* --- the stand-in game ------------------------------------------- */

static GBContext g_ctx;
static uint8_t g_wram[0x8000];

static uint8_t rd(uint16_t addr) { return g_wram[addr - 0xC000]; }
static void wr(uint16_t addr, uint8_t v) { g_wram[addr - 0xC000] = v; }

static int g_entered[64];
static int g_entered_count = 0;

/* bank2.s: left/right wrap within 13 columns; up/down step rows and
 * stop before the options row unless you walk into it.
 *
 * The game reads *edges* (getInputWithAutofire), so a button held for
 * several frames moves the cursor once. Modelling levels instead would
 * make a two-frame press look like two presses -- which is precisely
 * the bug this harness exists to catch, so it has to be right here. */
static void apply_buttons(uint8_t dpad_now, uint8_t buttons_now) {
    static uint8_t prev_dpad = 0xFF, prev_buttons = 0xFF;
    /* Newly pressed = was high (released), now low (pressed). */
    const uint8_t dpad = (uint8_t)(dpad_now | ~prev_dpad);
    const uint8_t buttons = (uint8_t)(buttons_now | ~prev_buttons);
    prev_dpad = dpad_now;
    prev_buttons = buttons_now;

    uint8_t pos = rd(A_cursorPos);
    int row = (pos >> 4) & 0x0F, col = pos & 0x0F;

    if (!(dpad & 0x01)) col = (col + 1) % 13;            /* right */
    if (!(dpad & 0x02)) col = (col + 12) % 13;           /* left  */
    if (!(dpad & 0x08)) row = (row + 1) % 6;             /* down  */
    if (!(dpad & 0x04)) row = (row + 5) % 6;             /* up    */
    wr(A_cursorPos, (uint8_t)((row << 4) | col));

    if (!(buttons & 0x01)) {                              /* A */
        const int sym = grid_symbol(row, col);
        if (g_entered_count < 64) g_entered[g_entered_count++] = sym;
    }
}

/* Run the typist until it finishes or the frame budget runs out. */
static int run_typist(const uint8_t* cells, int count, int max_frames) {
    g_entered_count = 0;
    memset(g_wram, 0, sizeof(g_wram));
    g_ctx.wram = g_wram;
    wr(A_mode2, 1);
    wr(A_textInputMode, 0x80);      /* secret grid, taking input */
    wr(A_cursorPos, 0x00);

    if (!epoch_secrets_type(cells, count)) return -1;
    apply_buttons(0xFF, 0xFF);       /* reset the edge detector */

    int frames = 0;
    while (epoch_secrets_busy() && frames < max_frames) {
        g_joypad_dpad = 0xFF;       /* the runtime rebuilds these each poll */
        g_joypad_buttons = 0xFF;
        epoch_secrets_tick(&g_ctx, "tlozooa");
        apply_buttons(g_joypad_dpad, g_joypad_buttons);
        frames++;
    }
    return frames;
}

int main(void) {
    /* Every symbol, one at a time: the whole alphabet must be reachable
     * and must land on the right character. */
    int wrong = 0;
    for (int v = 0; v < 64; v++) {
        const uint8_t one = (uint8_t)v;
        const int frames = run_typist(&one, 1, 2000);
        if (frames < 0) { wrong++; continue; }
        if (g_entered_count != 1 || g_entered[0] != v) wrong++;
    }
    CHECK(wrong == 0, "all 64 symbols are reached and entered correctly");

    /* A full 20-symbol secret, in order. */
    {
        uint8_t cells[20];
        for (int i = 0; i < 20; i++) cells[i] = (uint8_t)((i * 7 + 3) % 64);
        const int frames = run_typist(cells, 20, 20000);
        CHECK(frames > 0, "a 20-symbol secret finishes");
        CHECK(g_entered_count == 20, "20 symbols were entered");
        int mismatch = 0;
        for (int i = 0; i < g_entered_count && i < 20; i++)
            if (g_entered[i] != cells[i]) mismatch++;
        CHECK(mismatch == 0, "every symbol matches the secret, in order");
        /* Sanity on cost: worst case is a few rows plus half a row of
         * columns per symbol, times press+gap frames. */
        CHECK(frames < 20 * 14 * 4, "it does not dawdle");
    }

    /* Starting from the options row, it climbs back into the grid. */
    {
        const uint8_t one = 40;
        g_entered_count = 0;
        memset(g_wram, 0, sizeof(g_wram));
        g_ctx.wram = g_wram;
        wr(A_mode2, 1);
        wr(A_textInputMode, 0x80);
        wr(A_cursorPos, 0x5A);          /* down among the OK/back options */
        epoch_secrets_type(&one, 1);
        int frames = 0;
        while (epoch_secrets_busy() && frames < 2000) {
            g_joypad_dpad = 0xFF;
            g_joypad_buttons = 0xFF;
            epoch_secrets_tick(&g_ctx, "tlozooa");
            apply_buttons(g_joypad_dpad, g_joypad_buttons);
            frames++;
        }
        CHECK(g_entered_count == 1 && g_entered[0] == 40,
              "recovers from the options row");
    }

    /* No entry screen: it must give up rather than press buttons at the
     * overworld. */
    {
        const uint8_t one = 5;
        memset(g_wram, 0, sizeof(g_wram));
        g_ctx.wram = g_wram;
        wr(A_mode2, 0);                 /* not the text input state */
        g_entered_count = 0;
        epoch_secrets_type(&one, 1);
        int pressed = 0;
        for (int i = 0; i < 60; i++) {
            g_joypad_dpad = 0xFF;
            g_joypad_buttons = 0xFF;
            epoch_secrets_tick(&g_ctx, "tlozooa");
            if (g_joypad_dpad != 0xFF || g_joypad_buttons != 0xFF) pressed++;
        }
        CHECK(pressed == 0, "presses nothing when no secret screen is open");
        epoch_secrets_cancel();
    }

    /* The typist refuses to start twice. */
    {
        const uint8_t one = 1;
        memset(g_wram, 0, sizeof(g_wram));
        g_ctx.wram = g_wram;
        wr(A_mode2, 1);
        wr(A_textInputMode, 0x80);
        CHECK(epoch_secrets_type(&one, 1), "first request is accepted");
        CHECK(!epoch_secrets_type(&one, 1), "second request is refused");
        epoch_secrets_cancel();
        CHECK(!epoch_secrets_busy(), "cancel stops it");
    }

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
