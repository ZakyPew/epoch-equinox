/* Continue the Legend: the seamless linked-game handoff.
 *
 * Finishing one Oracle used to mean copying a twenty-symbol secret to
 * paper, swapping carts, and typing it in. Both carts live in this one
 * player and the player already generates the secret AND types codes by
 * driving the game's own cursor -- so the launcher writes the secret to
 * states/handoff.txt and starts the other game, and this machine does
 * the rest: walk the file select to SECRETS on a free slot, type the
 * code with the same typist the Esc menu uses, accept, and keep going
 * until the linked game is standing in a room.
 *
 * It never writes game memory. Every step is the same button presses a
 * hand would make, steered by the game's own state -- and one real
 * press from the player cancels the whole thing, because hands beat
 * automation.
 */
#ifndef EPOCH_HANDOFF_H
#define EPOCH_HANDOFF_H

#include <stdbool.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read states/handoff.txt and arm the machine if its `to=` names this
 *  game. Call once at startup, after the ROM is loaded. Safe to call
 *  when the file is absent or for someone else. */
void epoch_handoff_arm(const char* game_id);

/** True while the machine is driving. */
bool epoch_handoff_active(void);

/** One line of status for HUDs and logs. */
const char* epoch_handoff_message(void);

/** Service one frame; call after gb_platform_poll_events, before the
 *  secrets tick (it queues work for the typist). */
void epoch_handoff_tick(GBContext* ctx, const char* game_id);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_HANDOFF_H */
