/* See epoch_overlay.h. */
extern "C" {
#include "epoch_overlay.h"
#include "platform_sdl.h"
}

extern "C" void epoch_rewind_hud_draw(void);
extern "C" void epoch_achievements_hud_draw(void);

static void draw_overlays(void) {
    epoch_rewind_hud_draw();
    epoch_achievements_hud_draw();
}

extern "C" void epoch_overlay_install(void) {
    gb_platform_set_host_overlay(draw_overlays);
}
