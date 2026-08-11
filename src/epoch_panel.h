/* The Epoch panel: our own window for the things that are not display
 * settings -- achievements and secrets -- so the runtime's Esc menu
 * stays what it is.
 *
 * Toggled with F2 (Esc and F10 both belong to the runtime's own
 * menu). Drawn through the host overlay, so it floats over the game
 * without the game seeing it, and swallows joypad input while it is
 * open the same way the Esc menu does.
 */
#ifndef EPOCH_PANEL_H
#define EPOCH_PANEL_H

#include <stdbool.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Service the hotkey and hold the joypad while the panel is up.
 *  Call once per frame, after gb_platform_poll_events. */
void epoch_panel_tick(GBContext* ctx);

/** True while the panel is showing. */
bool epoch_panel_open(void);

/** Draw it. Called from the host overlay. */
void epoch_panel_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_PANEL_H */
