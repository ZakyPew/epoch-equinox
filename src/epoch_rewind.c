/* Rewind, room checkpoints and their HUD. See epoch_rewind.h.
 *
 * Snapshots go through gb_context_save_state_file into a fixed ring of
 * paths under states/rewind/. Files rather than memory because that is
 * the API the runtime exposes, and because the OS page cache makes a
 * 60 KB write every sixth frame free in practice -- measured at well
 * under a millisecond, against a 16 ms frame budget.
 *
 * Two independent things live here because they share the machinery:
 *
 *   REWIND (hold R)      steps back through the ring, ~6 steps a second,
 *                        giving about twelve seconds of history.
 *   ROOM CHECKPOINT (F9) jumps to the moment you walked into the room
 *                        you are standing in. This is LynnaLab's
 *                        "quickstart" idea from the player's side: the
 *                        editor boots you into a room, and this puts you
 *                        back at the start of one.
 */
#include "epoch_rewind.h"

#include "platform_sdl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define ep_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define ep_mkdir(p) mkdir(p, 0755)
#endif

#ifdef GB_HAS_SDL2
#include <SDL.h>
#endif

#define RING_SLOTS      72     /* ~12 s of history at the capture rate */
#define CAPTURE_EVERY   10     /* frames between snapshots (6 per second) */
#define REWIND_EVERY    3      /* frames between steps while held */
#define TOAST_FRAMES    110    /* ~1.8 s on screen */

typedef struct {
    long  frame;      /* guest frame this slot holds, -1 when empty */
    int   room;       /* active room at capture, -1 if unknown */
    int   group;
} RingSlot;

static RingSlot g_ring[RING_SLOTS];
static int  g_head = 0;          /* next slot to write */
static int  g_filled = 0;
static long g_frame = 0;

static int  g_cursor = -1;       /* slot being shown while rewinding */
static bool g_rewinding = false;

/* The room we are standing in, and the ring slot where we entered it. */
static int  g_room = -1, g_group = -1;
static int  g_room_entry_slot = -1;

static char g_toast[96];
static int  g_toast_left = 0;

static void toast(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
    va_end(ap);
    g_toast_left = TOAST_FRAMES;
}

static void slot_path(char* buf, size_t n, int slot) {
    snprintf(buf, n, "states/rewind/%02d.state", slot);
}

/* ------------------------------------------------------------------ */
/* which room are we in                                                */
/* ------------------------------------------------------------------ */

/* wActiveGroup / wActiveRoom, per cart. Same addresses the voxel renderer
 * reads; duplicated rather than shared so rewind works with the voxel
 * module compiled out. */
static bool read_room(GBContext* ctx, int* group, int* room) {
    if (!ctx || !ctx->rom || !ctx->wram) return false;
    const uint8_t* title = ctx->rom + 0x134;
    uint16_t g_addr, r_addr;
    if (memcmp(title, "ZELDA NAYRU", 11) == 0) {
        g_addr = 0xCC2D; r_addr = 0xCC30;
    } else if (memcmp(title, "ZELDA DIN", 9) == 0) {
        g_addr = 0xCC49; r_addr = 0xCC4C;
    } else {
        return false;
    }
    *group = ctx->wram[g_addr - 0xC000];
    *room  = ctx->wram[r_addr - 0xC000];
    return true;
}

/* ------------------------------------------------------------------ */
/* capture / restore                                                   */
/* ------------------------------------------------------------------ */

static void capture(GBContext* ctx) {
    char path[256];
    slot_path(path, sizeof(path), g_head);
    if (!gb_context_save_state_file(ctx, path)) return;
    g_ring[g_head].frame = g_frame;
    g_ring[g_head].room  = g_room;
    g_ring[g_head].group = g_group;
    if (g_room_entry_slot < 0) g_room_entry_slot = g_head;
    g_head = (g_head + 1) % RING_SLOTS;
    if (g_filled < RING_SLOTS) g_filled++;
}

static bool restore(GBContext* ctx, int slot) {
    if (slot < 0 || g_ring[slot].frame < 0) return false;
    char path[256];
    slot_path(path, sizeof(path), slot);
    return gb_context_load_state_file(ctx, path);
}

/* ------------------------------------------------------------------ */
/* HUD                                                                 */
/* ------------------------------------------------------------------ */

/* Drawn through the runtime's host-overlay hook; C++ because ImGui is,
 * so the drawing lives in epoch_rewind_hud.cpp. Declared here, defined
 * there, kept in sync by this tiny state struct. */
typedef struct {
    bool rewinding;
    int  steps_back;
    int  seconds_back;
    int  group, room;
    const char* toast;
} EpochHudState;

static EpochHudState g_hud;

const EpochHudState* epoch_rewind_hud_state(void) { return &g_hud; }

/* ------------------------------------------------------------------ */
/* frame service                                                       */
/* ------------------------------------------------------------------ */

void epoch_rewind_tick(GBContext* ctx, const char* game_id) {
    (void)game_id;
    if (!ctx) return;

    static bool inited = false;
    if (!inited) {
        ep_mkdir("states");
        ep_mkdir("states/rewind");
        gb_context_set_state_logging_quiet(true);
        for (int i = 0; i < RING_SLOTS; i++) g_ring[i].frame = -1;
        inited = true;
    }

    g_frame++;
    if (g_toast_left > 0) g_toast_left--;

    /* Room tracking: entering a new room starts a fresh checkpoint. */
    int group = -1, room = -1;
    if (read_room(ctx, &group, &room)) {
        if (room != g_room || group != g_group) {
            g_room = room;
            g_group = group;
            g_room_entry_slot = -1;   /* set by the next capture */
        }
    }

#ifdef GB_HAS_SDL2
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    bool want_rewind = keys && keys[SDL_SCANCODE_R];

    /* F9: back to the moment this room began. */
    static bool f9_was = false;
    bool f9 = keys && keys[SDL_SCANCODE_F9];
    if (f9 && !f9_was) {
        if (g_room_entry_slot >= 0 && restore(ctx, g_room_entry_slot)) {
            toast("Back to the start of room %02X", g_room);
        } else {
            toast("No checkpoint for this room yet");
        }
    }
    f9_was = f9;
#else
    bool want_rewind = false;
#endif

    if (want_rewind && g_filled > 1) {
        if (!g_rewinding) {
            g_rewinding = true;
            /* Start from the newest complete snapshot. */
            g_cursor = (g_head - 1 + RING_SLOTS) % RING_SLOTS;
        } else if ((g_frame % REWIND_EVERY) == 0) {
            int next = (g_cursor - 1 + RING_SLOTS) % RING_SLOTS;
            if (g_ring[next].frame >= 0 && next != g_head) g_cursor = next;
        }
        restore(ctx, g_cursor);

        int steps = (g_head - 1 - g_cursor + RING_SLOTS) % RING_SLOTS;
        g_hud.rewinding = true;
        g_hud.steps_back = steps;
        g_hud.seconds_back = (steps * CAPTURE_EVERY) / 60;
        g_hud.group = g_group;
        g_hud.room = g_room;
        g_hud.toast = NULL;
        return;                     /* do not capture while scrubbing */
    }

    if (g_rewinding) {
        /* Released: the timeline continues from here, so everything after
         * the cursor is a future that no longer happened. */
        g_rewinding = false;
        g_head = (g_cursor + 1) % RING_SLOTS;
        g_filled = 1;
        for (int i = 0; i < RING_SLOTS; i++) {
            if (i != g_cursor) g_ring[i].frame = -1;
        }
        g_room_entry_slot = -1;
        toast("Resumed");
    }

    if ((g_frame % CAPTURE_EVERY) == 0) capture(ctx);

    g_hud.rewinding = false;
    g_hud.group = g_group;
    g_hud.room = g_room;
    g_hud.toast = (g_toast_left > 0) ? g_toast : NULL;
}
