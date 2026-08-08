/* The voxel section of the Esc settings menu.
 *
 * Drawn through the runtime's host-menu hook (patches/gbrt-host-menu.patch),
 * inside the live ImGui frame, above the Advanced header -- so the diorama
 * is a visible option rather than an F3 secret. C++ because ImGui is; the
 * voxel core stays C and is driven through its public getters/setters.
 */
extern "C" {
#include "voxel/voxel.h"
}

#include "imgui.h"
#include "platform_sdl.h"

static void draw_voxel_menu(void) {
    if (!ImGui::CollapsingHeader("Voxel Diorama")) {
        return;
    }

    static const char* modes[VOXEL_MODE_COUNT] = {
        "Off", "Tilt 15\xC2\xB0", "Tilt 30\xC2\xB0", "Tilt 45\xC2\xB0",
        "Chase Cam",
    };
    int mode = voxel_get_mode();
    if (ImGui::Combo("Diorama", &mode, modes, VOXEL_MODE_COUNT)) {
        voxel_set_mode(mode);
    }
    ImGui::TextDisabled("F3 cycles this in game.");
    ImGui::TextDisabled("Terrain and sky follow the game's own state\n"
                        "on the Oracle carts; menus stay flat.");
}

extern "C" void voxel_menu_install(void) {
    gb_platform_set_host_menu(draw_voxel_menu);
}
