/* The voxel section of the Esc settings menu.
 *
 * Drawn through the runtime's host-menu hook (patches/gbrt-host-menu.patch),
 * inside the live ImGui frame, above the Advanced header -- so the diorama
 * is a visible option rather than an F3 secret. C++ because ImGui is; the
 * voxel core stays C and is driven through its public getters/setters.
 *
 * The Shape and Camera sliders write straight into the renderer's live
 * tuning block, so the world reshapes under the menu as you drag. That is
 * deliberate: "the trees are too tall" used to be a rebuild, and now it is
 * a slider you can settle while playing and then Save.
 */
extern "C" {
#include "voxel/voxel.h"
}

#include "imgui.h"
#include "platform_sdl.h"

/* Height classes, matching voxel_internal.h without dragging it in. */
enum { VX_WATER = 0, VX_FLOOR = 1, VX_LOW = 2, VX_MID = 3, VX_HIGH = 4 };

extern "C" void voxel_menu_draw(void) {
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
    ImGui::TextDisabled("F3 cycles this in game. In Chase Cam the right stick\n"
                        "(or Q/E) turns the camera.");
    ImGui::TextDisabled("Terrain and sky follow the game's own state\n"
                        "on the Oracle carts; menus stay flat.");

    VoxelTuning* t = voxel_tuning();

    ImGui::Spacing();
    ImGui::TextDisabled("Shape");
    ImGui::SliderFloat("Grass / decor", &t->units[VX_LOW], 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Bushes / rocks", &t->units[VX_MID], 0.0f, 8.0f, "%.2f");
    ImGui::SliderFloat("Trees / walls", &t->units[VX_HIGH], 0.0f, 14.0f, "%.2f");
    ImGui::SliderFloat("Water depth", &t->units[VX_WATER], -6.0f, 0.0f, "%.2f");
    ImGui::SliderFloat("Foliage footprint", &t->footprint, 0.0f, 4.0f, "%.2f");
    ImGui::TextDisabled("Footprint is how far foliage pulls back from its\n"
                        "cell edge: 0 makes hard blocks, high makes tufts.");
    ImGui::SliderFloat("Tilt height", &t->tilt_scale, 0.2f, 2.5f, "%.2f");

    ImGui::Spacing();
    ImGui::TextDisabled("Chase camera");
    ImGui::SliderFloat("Distance", &t->chase_back, 20.0f, 130.0f, "%.0f");
    ImGui::SliderFloat("Height", &t->chase_height, 6.0f, 90.0f, "%.0f");
    ImGui::SliderFloat("Field of view", &t->chase_fov, 0.35f, 1.20f, "%.2f");
    ImGui::SliderFloat("Vertical scale", &t->chase_hpx, 0.8f, 9.0f, "%.2f");

    /* The camera holds still by default and is recentred on request. The
     * checkbox is the old walk-follow, off unless someone wants it. */
    bool follow = t->chase_follow > 0.0f;
    if (ImGui::Checkbox("Auto-swing behind Link while walking", &follow)) {
        t->chase_follow = follow ? 0.06f : 0.0f;
    }
    if (follow) {
        ImGui::SliderFloat("Auto-swing speed", &t->chase_follow,
                           0.01f, 0.40f, "%.3f");
    }
    ImGui::SliderFloat("Recentre speed", &t->chase_recenter,
                       0.02f, 0.60f, "%.2f");
    ImGui::TextDisabled("Right stick (or Q/E) turns the camera and it stays\n"
                        "put. Click the right stick (or C) to swing behind\n"
                        "Link -- hold it to keep following him.");
    ImGui::SliderFloat("Fog begins", &t->fog_start, 0.0f, 200.0f, "%.0f");
    ImGui::SliderFloat("Fog strength", &t->fog_max, 0.0f, 256.0f, "%.0f");

    ImGui::Spacing();
    if (ImGui::Button("Save tuning")) {
        voxel_tuning_save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        voxel_tuning_reset();
    }
    ImGui::TextDisabled("Saved to voxel/tuning.ini next to the binary --\n"
                        "a plain text file you can share or commit.");
}

