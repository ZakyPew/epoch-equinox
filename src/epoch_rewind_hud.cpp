/* The rewind HUD: a corner readout drawn through the runtime's
 * host-overlay hook, so it shows whether or not the Esc menu is open.
 *
 * Deliberately quiet. It says three things and only when they matter:
 * that you are scrubbing and how far back, which room you are standing
 * in (the number you need to name an override file), and whatever just
 * happened.
 */
extern "C" {
#include "gbrt.h"
}

#include "imgui.h"
#include "platform_sdl.h"

/* Mirror of the state struct in epoch_rewind.c. */
struct EpochHudState {
    bool rewinding;
    int  steps_back;
    int  seconds_back;
    int  group, room;
    const char* toast;
};

extern "C" const EpochHudState* epoch_rewind_hud_state(void);

static void draw_hud(void) {
    const EpochHudState* s = epoch_rewind_hud_state();
    if (!s) return;
    const bool show_room = (s->room >= 0);
    if (!s->rewinding && !s->toast && !show_room) return;

    ImGuiIO& io = ImGui::GetIO();
    const float pad = 10.0f * io.FontGlobalScale;
    ImGui::SetNextWindowPos(ImVec2(pad, io.DisplaySize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(s->rewinding ? 0.7f : 0.4f);
    if (ImGui::Begin("##epoch_hud", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs)) {
        if (s->rewinding) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.45f, 1.0f),
                               "<< REWIND  -%d.%ds",
                               s->seconds_back,
                               (s->steps_back * 10 / 6) % 10);
        }
        if (show_room) {
            ImGui::TextDisabled("Room %d-%02X   (F9 restarts it, hold R to rewind)",
                                s->group, s->room);
        }
        if (s->toast) {
            ImGui::TextUnformatted(s->toast);
        }
    }
    ImGui::End();
}

extern "C" void epoch_rewind_install(void) {
    gb_platform_set_host_overlay(draw_hud);
}
