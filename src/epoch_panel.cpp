/* See epoch_panel.h. */
extern "C" {
#include "epoch_panel.h"
#include "platform_sdl.h"
}

#include "imgui.h"

#include <stdlib.h>

#ifdef GB_HAS_SDL2
#include <SDL.h>
#endif

extern "C" void epoch_achievements_panel_draw(void);
extern "C" void epoch_secrets_panel_draw(void);

static bool g_open = false;

bool epoch_panel_open(void) { return g_open; }

void epoch_panel_tick(GBContext* ctx) {
    (void)ctx;
    /* EPOCH_PANEL_TEST=1 opens it at boot, for screenshots. */
    static bool checked = false;
    if (!checked) { checked = true; if (getenv("EPOCH_PANEL_TEST")) g_open = true; }
#ifdef GB_HAS_SDL2
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    static bool was_down = false;
    const bool down = keys && keys[SDL_SCANCODE_F2];
    if (down && !was_down) g_open = !g_open;
    was_down = down;

    if (g_open && keys && keys[SDL_SCANCODE_ESCAPE]) g_open = false;
#endif
    if (g_open) {
        /* Hold the joypad the way the Esc menu does: the game keeps
         * running, but keys aimed at this panel do not reach Link.
         * Written after poll_events, which rebuilds these each frame. */
        g_joypad_dpad = 0xFF;
        g_joypad_buttons = 0xFF;
    }
}

void epoch_panel_draw(void) {
    if (!g_open) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size(io.DisplaySize.x * 0.62f, io.DisplaySize.y * 0.72f);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 21, 34, 244));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(222, 178, 76, 190));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(38, 34, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_TabActive, IM_COL32(60, 50, 26, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);

    bool open = true;
    if (ImGui::Begin("Epoch & Equinox###epoch_panel", &open,
                     ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##epoch_tabs")) {
            if (ImGui::BeginTabItem("Achievements")) {
                epoch_achievements_panel_draw();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Secrets")) {
                epoch_secrets_panel_draw();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("F2 closes this. Display and emulator settings stay on Esc.");
    }
    ImGui::End();
    if (!open) g_open = false;

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}
