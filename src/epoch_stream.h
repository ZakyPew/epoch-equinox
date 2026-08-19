/* Live game state for the stream overlays.
 *
 * The player already reads the save block every frame for achievements.
 * This writes the interesting parts of it to stream/live.js next to the
 * binary, so an OBS browser source can show essences, hearts, rings,
 * deaths and the achievement tally without anyone typing them in.
 *
 * It emits JavaScript, not JSON, and that is deliberate: OBS loads a
 * local overlay over file://, where Chromium blocks fetch() and XHR but
 * still allows a <script> from the same folder. The existing now.js
 * status line already works that way; this follows it.
 */
#ifndef EPOCH_STREAM_H
#define EPOCH_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one frame of the game looks like to a stream overlay. Kept plain
 * so tools/stream_test.c can build one by hand. */
typedef struct {
    char     cart[16];        /* "tlozooa" / "tlozoos"     */
    char     title[32];       /* "Oracle of Ages"          */
    int      group, room;     /* -1 when not in a room     */
    int      essences;        /* 0-8                       */
    int      hearts;          /* quarter-hearts, as stored */
    int      max_hearts;
    int      rings;           /* 0-64                      */
    int      deaths;
    int      kills;
    int      rupees;          /* purse now                 */
    int      rupees_total;    /* collected ever            */
    int      play_seconds;
    bool     linked;
    int      unlocked, total; /* achievements              */
    char     last_id[48];     /* most recent unlock, "" if none */
    char     last_title[64];
    char     last_desc[96];
    uint32_t unlock_serial;   /* bumps per unlock, so the overlay can
                               * tell a NEW one from a redraw */
    uint32_t tick;            /* bumps per write: a heartbeat, so the
                               * overlay can tell a running player from a
                               * file left behind by one that exited */

    /* -- speedrun timer and item tracker --------------------------- *
     * play_frames is the game's own counter, untouched: it is what a
     * file's clock reads, it pauses when the game does, and it survives
     * the overlay being reloaded mid-run. Seconds are derived from it
     * for anything that does not want the precision. */
    uint32_t play_frames;
    /* One bit per treasure id, LSB first -- wObtainedTreasureFlags,
     * straight out of the save block. 128 ids, and the tracker decides
     * which of them are worth a square. */
    uint8_t  treasures[16];
    int      sword, shield, satchel, bracelet;  /* tiers, 0 = not held */
    int      bombs, max_bombs;
    int      seeds;           /* how many of the five seed types held */

    /* Buttons held this frame, for an input display. One bit each:
     * right, left, up, down, A, B, select, start. Active HIGH here --
     * the hardware's active-low is inverted on the way in, because a
     * feed that reads "1 means pressed" is one less thing to get
     * backwards in the overlay. */
    int      pad;

    /* The run, when the cart has a split file. Names come along every
     * write: it is a few hundred bytes once a second and it means the
     * overlay needs no second channel to learn the route. */
    int      split_count, split_next;
    uint32_t split_frame[16];
    const char* split_name[16];
} EpochStreamState;

/** Fill `out` from a WRAM snapshot (0x2000 bytes, 0xC000-based) for the
 *  named cart. Returns false when the game is not in play, in which case
 *  `out` is left untouched -- the overlay keeps the last good values
 *  rather than blanking during menus and transitions. */
bool epoch_stream_read(EpochStreamState* out, const uint8_t* wram,
                       const char* game_id);

/** Render `s` as the one-line JS call the overlays consume. Returns the
 *  length written (excluding the terminator), or -1 if it would not fit. */
int epoch_stream_format(const EpochStreamState* s, char* buf, size_t cap);

/** Call once per emulated frame; writes stream/live.js about once a
 *  second, and immediately when an achievement unlocks. */
void epoch_stream_tick(GBContext* ctx, const char* game_id);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_STREAM_H */
