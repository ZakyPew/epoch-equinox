/* Auto-splits: fire a split when the game's own memory says you earned it.
 *
 * A split file is an achievement pack -- same six condition kinds, same
 * parser, same documented syntax -- because "the third essence is in the
 * bag" is the same question whether it lights a toast or stops a
 * segment. Put one at splits/<cart>.txt and every entry becomes a split,
 * in file order.
 *
 * Nothing here keeps the time. The run's clock is the game's own frame
 * counter, and the authority on a run is LiveSplit, which most people
 * already have open with their PB and their history in it. This decides
 * WHEN, and hands that to whoever is counting: the overlay draws the
 * segment list, and epoch_livesplit pushes the same event down a socket.
 *
 * The core works on a plain WRAM buffer so it can be exercised without a
 * cart (tools/splits_test.c); the GBContext glue is one thin call.
 */
#ifndef EPOCH_SPLITS_H
#define EPOCH_SPLITS_H

#include <stdbool.h>
#include <stdint.h>

#include "epoch_achievements.h"
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { ES_MAX_SPLITS = 64 };

typedef struct {
    /* The frame counter when this one fired. Frames, not seconds: a
     * split list is read to hundredths or it is not worth reading. */
    uint32_t frame;
    bool     done;
} EsSplit;

typedef struct {
    EaSet    set;                 /* the conditions, parsed as a pack   */
    EsSplit  hit[ES_MAX_SPLITS];
    int      count;
    int      next;                /* index of the segment being run     */
    uint32_t started;             /* frame the run was first seen at    */
    uint32_t last_frame;          /* to notice the clock going backwards */
    bool     running;
} EsRun;

/** Load splits/<cart>.txt into `run`. Returns how many splits were read,
 *  or -1 when there is no file -- which is not an error, it just means
 *  this cart has no splits and the feature stays quiet. */
int es_load(EsRun* run, const char* path);

/** Evaluate against a WRAM snapshot (0x2000 bytes, 0xC000-based) at the
 *  given play-time frame count. Indices of splits that fired this call
 *  are written to `fired` (up to `cap`); returns how many.
 *
 *  Splits fire in file order: a condition that is already true when the
 *  run starts does not retroactively fire the ones before it. */
int es_evaluate(EsRun* run, const uint8_t* wram, uint32_t frames,
                int* fired, int cap);

/** True when the clock went backwards or a different file was loaded --
 *  a new run. Clears the split times. Called by es_evaluate; exposed so
 *  a test can drive it directly. */
bool es_maybe_reset(EsRun* run, uint32_t frames);

/* ---- runner glue ---------------------------------------------------- */

/** Once per emulated frame. Loads the cart's splits on first call,
 *  evaluates, and forwards anything that fired to LiveSplit. */
void epoch_splits_tick(GBContext* ctx, const char* game_id);

/** The run as it stands, for the overlay feed. NULL before the first
 *  tick or when the cart has no split file. */
const EsRun* epoch_splits_run(void);

/** The name of split `i`, which is the title from the pack file. */
const char* epoch_splits_name(const EsRun* run, int i);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_SPLITS_H */
