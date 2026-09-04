/* Continue the Legend: the linked-game handoff.
 *
 * Finishing either Oracle earns a twenty-symbol transfer secret; typed
 * into the other cart it starts the linked quest. The launcher writes
 * that secret to states/handoff.txt and starts the other game; this
 * machine reads it at boot and does the typing itself -- splashes,
 * file select, SECRETS, the symbol grid (the same typist the Esc menu
 * uses), accept, and on to a linked game standing in a room. The
 * emulator runs at max speed for the whole drive.
 *
 * The file is consumed the moment it is read, so a handoff is one
 * shot: whatever stops it, the next launch is an ordinary launch.
 * Any real button press while the machine drives hands the game to
 * the player and stops it.
 */
#ifndef EPOCH_HANDOFF_H
#define EPOCH_HANDOFF_H

#include "gbrt.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Read (and consume) states/handoff.txt if it is addressed to
 *  `game_id`. Call once, after the platform knows the game and before
 *  the first frame. */
void epoch_handoff_arm(const char* game_id);

/** True while the machine is driving. */
bool epoch_handoff_active(void);

/** 0 while driving (or idle), 1 once a linked game is live, -1 when the
 *  machine stopped short. Meaningful while epoch_handoff_message() is
 *  non-NULL. */
int epoch_handoff_outcome(void);

/** What to show the player: progress while driving, then the verdict
 *  for a few seconds after the machine stops. NULL when there is
 *  nothing to say. */
const char* epoch_handoff_message(void);

/** Once per poll, after the platform rebuilt the joypad globals and
 *  before the typist's tick. Safe to call when nothing is armed. */
void epoch_handoff_tick(GBContext* ctx, const char* game_id);

#ifdef __cplusplus
}
#endif

#endif
