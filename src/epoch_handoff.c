/* See epoch_handoff.h.
 *
 * The machine is a chain of "press this until the game's state says
 * we're past it" steps, in the same spirit as the typist one file
 * over: read WRAM, push buttons, and if the game ever disagrees, the
 * game wins. State values were learned with HANDOFF_PROBE=1 in
 * tools/handoff_test.c against the real carts.
 */
#include "epoch_handoff.h"
#include "epoch_secrets.h"
#include "platform_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDOFF_PATH "states/handoff.txt"

#define LOG(...) do { fprintf(stderr, "[HANDOFF] " __VA_ARGS__); \
                      fputc('\n', stderr); } while (0)

/* wFileSelect block, shared by both carts (same union the typist reads). */
#define A_fs_mode   0xCBB3
#define A_fs_mode2  0xCBB4
#define A_fs_tim    0xCBB7   /* textInputMode; top bit = secret grid  */
#define A_cursor    0xCBBC
#define A_scroll    0xCD00   /* wScrollMode; 1 = standing in a room   */

/* Active-low, like the runtime's joypad globals. */
#define BTN_A    0x01
#define BTN_ST   0x08
#define DPAD_D   0x08

typedef enum {
    HO_OFF = 0,
    HO_BOOT,        /* splashes and title: Start until the file select */
    HO_PICK,        /* move down to the free slot, enter it            */
    HO_SUB,         /* NEW GAME / SECRETS / GAME LINK: down once, A    */
    HO_GRID,        /* wait for the secret grid, hand off to the typist */
    HO_TYPING,      /* the typist is driving                           */
    HO_ACCEPT,      /* Start to accept, then A through what follows    */
    HO_DONE,
} HoState;

static struct {
    bool    armed;
    HoState state;
    uint8_t cells[ES_MAX_SYMBOLS];
    int     count;
    int     slot;
    char    name[8];
    char    message[96];
    int     frame;          /* frames in the current state */
    int     presses;        /* taps issued in the current state */
    int     hold, gap;
    uint8_t hold_dpad, hold_buttons;
} g;

bool epoch_handoff_active(void) { return g.armed; }
const char* epoch_handoff_message(void) { return g.message; }

static void ho_fail(const char* why) {
    LOG("stopped: %s (state %d)", why, (int)g.state);
    snprintf(g.message, sizeof(g.message), "Handoff stopped: %s", why);
    g.armed = false;
    g.state = HO_OFF;
}

static void ho_enter(HoState s) {
    g.state = s;
    g.frame = 0;
    g.presses = 0;
}

void epoch_handoff_arm(const char* game_id) {
    memset(&g, 0, sizeof(g));
    FILE* f = fopen(HANDOFF_PATH, "r");
    if (!f) return;
    char line[128], to[32] = "";
    while (fgets(line, sizeof(line), f)) {
        char sym[64];
        if (sscanf(line, "to=%31s", to) == 1) continue;
        if (sscanf(line, "slot=%d", &g.slot) == 1) continue;
        if (sscanf(line, "name=%7s", g.name) == 1) continue;
        if (sscanf(line, "symbols=%63s", sym) == 1) {
            for (int i = 0; i + 1 < (int)strlen(sym) &&
                            g.count < ES_MAX_SYMBOLS; i += 2) {
                unsigned v;
                if (sscanf(sym + i, "%2x", &v) != 1) break;
                g.cells[g.count++] = (uint8_t)v;
            }
        }
    }
    fclose(f);
    if (strcmp(to, game_id) != 0) return;    /* someone else's handoff */
    if (g.count != 20 || g.slot < 0 || g.slot > 2) {
        LOG("malformed %s ignored", HANDOFF_PATH);
        return;
    }
    /* The encoder's own decoder vets the code before a single press. */
    int type, gid;
    if (!es_decode(g.cells, g.count, &type, &gid) || type != 0) {
        LOG("secret does not decode as a game secret; ignored");
        return;
    }
    g.armed = true;
    ho_enter(HO_BOOT);
    snprintf(g.message, sizeof(g.message),
             "Continuing the legend: linking %s's game...", g.name);
    LOG("armed: slot %d, 20 symbols, hero %s", g.slot, g.name);
}

/* One paced tap: press for 2 frames, release for 8 so the menu's own
 * fade-ins and cursor repeat cannot eat consecutive presses. */
static void tap(uint8_t dpad, uint8_t buttons) {
    g.hold_dpad = dpad;
    g.hold_buttons = buttons;
    g.hold = 2;
    g.gap = 8;
}

static uint8_t rd(GBContext* ctx, uint16_t addr) {
    return ctx->wram[addr - 0xC000];
}

void epoch_handoff_tick(GBContext* ctx, const char* game_id) {
    (void)game_id;
    if (!g.armed || !ctx || !ctx->wram) return;

    /* Hands beat automation: a real press on the pad or keyboard while
     * the machine drives is the player taking over. The globals are
     * rebuilt from real input every poll, so at this point they carry
     * only the player's own buttons. */
    if (g_joypad_dpad != 0xFF || g_joypad_buttons != 0xFF) {
        ho_fail("player took over");
        return;
    }

    if (g.hold > 0) {
        g_joypad_dpad = g.hold_dpad;
        g_joypad_buttons = g.hold_buttons;
        g.hold--;
        return;
    }
    if (g.gap > 0) { g.gap--; return; }

    g.frame++;
    if (g.frame > 3600) {                     /* a minute stuck: stop */
        ho_fail("state never advanced");
        return;
    }

    const uint8_t mode2 = rd(ctx, A_fs_mode2);
    const uint8_t tim   = rd(ctx, A_fs_tim);

    switch (g.state) {
    case HO_BOOT:
        /* The pre-gameplay path is frame-deterministic from reset (no
         * RNG until a room is live), and the wFileSelect bytes are
         * reused as scratch counters by the intro, so there is nothing
         * trustworthy to key on this early. Mirror the proven route:
         * Start twice, then the file select is up. */
        if (g.frame == 650 || g.frame == 1050)
            tap(0xFF, (uint8_t)~BTN_ST);
        if (g.frame >= 1150) ho_enter(HO_PICK);
        break;
    case HO_PICK:
        /* Down to the free slot, A to enter it. Generous spacing so a
         * sliding screen cannot eat a tap. */
        if (g.frame < 60) break;
        if ((g.frame % 30) == 0) {
            if (g.presses < g.slot) {
                g.presses++;
                tap((uint8_t)~DPAD_D, 0xFF);
            } else if (g.presses == g.slot) {
                g.presses++;
                tap(0xFF, (uint8_t)~BTN_A);
            } else {
                ho_enter(HO_SUB);
            }
        }
        break;
    case HO_SUB:
        /* NEW GAME / SECRETS / GAME LINK, cursor on NEW GAME. */
        if (g.frame < 50) break;
        if ((g.frame % 30) == 0) {
            if (g.presses == 0) { g.presses++; tap((uint8_t)~DPAD_D, 0xFF); }
            else if (g.presses == 1) { g.presses++; tap(0xFF, (uint8_t)~BTN_A); }
            else ho_enter(HO_GRID);
        }
        break;
    case HO_GRID:
        /* The grid gate (mode2 == 1, textInputMode bit 7 -- the same
         * one the typist trusts) is only meaningful ON the grid; during
         * transitions those bytes cycle as scratch. Real screens hold
         * their state, scratch does not: require the gate for ten
         * consecutive frames before believing it. The typist then
         * verifies for real -- its cursor feedback stalls out and
         * cancels if this is not actually a symbol grid. */
        if (mode2 == 1 && (tim & 0x80)) {
            if (++g.presses >= 10) {
                if (epoch_secrets_type(g.cells, g.count)) {
                    LOG("typist engaged");
                    ho_enter(HO_TYPING);
                } else {
                    ho_fail("typist was busy");
                }
            }
        } else {
            g.presses = 0;
        }
        if (g.frame > 900) ho_fail("no secret grid appeared");
        break;
    case HO_TYPING:
        if (!epoch_secrets_busy()) {
            int done, total;
            epoch_secrets_status(&done, &total, NULL);
            if (done < total) {
                ho_fail("typing was interrupted");
            } else {
                ho_enter(HO_ACCEPT);
            }
        }
        break;
    case HO_ACCEPT:
        /* Start submits the code, then the game walks its own script:
         * the decoded-name confirmation, the message-speed prompt, the
         * fade into the linked opening. A through all of it until a
         * room is live -- wScrollMode is one of the few bytes here that
         * means what it says. */
        if (rd(ctx, A_scroll) == 0x01) {
            remove(HANDOFF_PATH);
            snprintf(g.message, sizeof(g.message),
                     "The legend continues -- linked game ready.");
            LOG("linked game is standing in a room");
            g.armed = false;
            ho_enter(HO_DONE);
            break;
        }
        if ((g.frame % 50) == 0) {
            if (g.presses++ == 0) tap(0xFF, (uint8_t)~BTN_ST);
            else if (g.presses < 60) tap(0xFF, (uint8_t)~BTN_A);
            else ho_fail("the cart did not accept the secret");
        }
        break;
    case HO_OFF:
    case HO_DONE:
        g.armed = false;
        break;
    }
}
