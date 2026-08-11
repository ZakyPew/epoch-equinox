/* See epoch_overlay.h. */
extern "C" {
#include "epoch_overlay.h"
#include "platform_sdl.h"
#include "epoch_panel.h"
#if EPOCH_HAVE_VOXEL
#include "voxel/voxel.h"
#endif
}

#include "imgui.h"

extern "C" void epoch_rewind_hud_draw(void);
extern "C" void epoch_achievements_hud_draw(void);
extern "C" void epoch_secrets_hud_draw(void);

static void draw_overlays(void) {
    epoch_rewind_hud_draw();
    epoch_achievements_hud_draw();
    epoch_secrets_hud_draw();
    epoch_panel_draw();
}

/* The Esc-menu hook is single-slot too; dispatch it the same way. */
static void draw_menus(void) {
#if EPOCH_HAVE_VOXEL
    voxel_menu_draw();
#endif
    ImGui::TextDisabled("Achievements and Secrets have their own window: F2.");
}

extern "C" void epoch_overlay_install(void) {
    gb_platform_set_host_overlay(draw_overlays);
    gb_platform_set_host_menu(draw_menus);
}
