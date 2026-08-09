/* Time-travel quality of life: rewind, room checkpoints, and the HUD that
 * tells you they happened.
 *
 * The Oracle games predate every modern convenience: one death and a
 * puzzle room starts over, and a mistimed jump costs the walk back. This
 * module gives the player the thing that makes an old game pleasant again
 * -- the ability to take a moment back -- without touching the game.
 *
 * It is built entirely on the runtime's existing snapshot API, so the
 * emulated machine has no idea it is happening.
 */
#ifndef EPOCH_REWIND_H
#define EPOCH_REWIND_H

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install the HUD hook. Call after gb_platform_init(). */
void epoch_rewind_install(void);

/** Call once per emulated frame, after the frame completes. Captures
 *  snapshots, services a held rewind key, and drives the HUD. */
void epoch_rewind_tick(GBContext* ctx, const char* game_id);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_REWIND_H */
