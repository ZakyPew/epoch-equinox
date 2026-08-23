/* The sculpt mode HUD: keys in, help out.
 *
 * Everything stateful lives in voxel_edit.c; this file is the thin SDL
 * and ImGui skin over it, drawn through the host-overlay hook so it
 * shows whether or not the Esc menu is open. F4 toggles sculpting while
 * a voxel mode is on; the number keys paint the gold-lit cell in front
 * of Link. Digits, because the letters are all taken: movement, items,
 * rewind, recentre and the camera all live on letters already.
 */
extern "C" {
#include "voxel_internal.h"
#include "voxel.h"
}

#include "imgui.h"

#include <SDL.h>
#include <stdio.h>

/* Edge detection: act on the press, not the hold. A held key repaints
 * the same value harmlessly, but sixty file writes a second is silly. */
static bool edge(const Uint8* keys, int scancode, bool* prev) {
    bool down = keys[scancode] != 0;
    bool fired = down && !*prev;
    *prev = down;
    return fired;
}

extern "C" void voxel_edit_hud_draw(void) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    if (!keys) return;

    static bool p_f4, p_1, p_2, p_3, p_4, p_5, p_0, p_bksp;

    /* Sculpting is a voxel-mode tool; in flat mode F4 does nothing
     * rather than arming an invisible editor. */
    if (edge(keys, SDL_SCANCODE_F4, &p_f4) && voxel_get_mode() > 0) {
        vox_edit_set_enabled(!vox_edit_enabled());
    }
    if (!vox_edit_enabled() || voxel_get_mode() == 0) return;

    /* What just happened, held on screen for a moment. */
    static char toast[64];
    static int toast_frames = 0;

    struct { int scancode; bool* prev; char code; const char* name; } paints[] = {
        { SDL_SCANCODE_1, &p_1, '_', "flat"  },
        { SDL_SCANCODE_2, &p_2, 'w', "water" },
        { SDL_SCANCODE_3, &p_3, 'l', "low"   },
        { SDL_SCANCODE_4, &p_4, 'm', "mid"   },
        { SDL_SCANCODE_5, &p_5, 'h', "high"  },
        { SDL_SCANCODE_0, &p_0, '.', "keep (erase override)" },
    };
    for (auto& p : paints) {
        if (edge(keys, p.scancode, p.prev)) {
            if (vox_edit_paint(p.code)) {
                snprintf(toast, sizeof(toast), "painted %s", p.name);
            } else {
                snprintf(toast, sizeof(toast), "no cell to paint");
            }
            toast_frames = 90;
        }
    }
    if (edge(keys, SDL_SCANCODE_BACKSPACE, &p_bksp)) {
        if (vox_edit_undo()) {
            snprintf(toast, sizeof(toast), "undid last paint");
        } else {
            snprintf(toast, sizeof(toast), "nothing to undo in this room");
        }
        toast_frames = 90;
    }

    bool is_seasons; int group, room;
    bool have_room = vox_edit_room(&is_seasons, &group, &room);
    int col, row;
    bool have_cell = vox_edit_cursor(&col, &row);

    ImGuiIO& io = ImGui::GetIO();
    const float pad = 10.0f * io.FontGlobalScale;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    if (ImGui::Begin("##vox_edit_hud", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs)) {
        ImGui::TextColored(ImVec4(0.87f, 0.70f, 0.30f, 1.0f),
                           "SCULPT  (F4 to leave)");
        if (have_room && have_cell) {
            static const char* NAMES = "._wlmh";
            static const char* LONG[] = {
                "keep", "flat", "water", "low", "mid", "high"
            };
            char cur = vox_edit_cell();
            const char* what = "keep";
            for (int i = 0; NAMES[i]; i++) {
                if (NAMES[i] == cur) what = LONG[i];
            }
            ImGui::Text("%s-%d-%02x  cell %d,%d: %s",
                        is_seasons ? "seasons" : "ages", group, room,
                        col, row, what);
            ImGui::TextDisabled("the gold cell is one step ahead of Link");
        } else {
            ImGui::TextDisabled("walk into a room to start");
        }
        int undoable = vox_edit_undo_count();
        if (undoable > 0) {
            ImGui::TextDisabled(
                "1 flat  2 water  3 low  4 mid  5 high  0 keep  "
                "Bksp undo (%d)", undoable);
        } else {
            ImGui::TextDisabled(
                "1 flat  2 water  3 low  4 mid  5 high  0 keep");
        }
        if (toast_frames > 0) {
            toast_frames--;
            ImGui::TextColored(ImVec4(0.56f, 0.84f, 0.66f, 1.0f),
                               "%s", toast);
        }
    }
    ImGui::End();
}
