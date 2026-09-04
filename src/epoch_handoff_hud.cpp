/* The Continue the Legend strip: what the machine is doing while it
 * drives the other cart, and the verdict for a moment after it stops.
 * Top-right, so it never covers the typist's own strip on the left
 * while the two work together. */
extern "C" {
#include "epoch_handoff.h"
}

#include "imgui.h"
#include <stdio.h>

extern "C" void epoch_handoff_hud_draw(void) {
    const char* message = epoch_handoff_message();
    if (!message) return;
    const bool driving = epoch_handoff_active();
    const int outcome = epoch_handoff_outcome();

    ImGuiIO& io = ImGui::GetIO();
    const float scale = io.FontGlobalScale > 0.0f ? io.FontGlobalScale : 1.0f;
    const float w = 300.0f * scale, h = (driving ? 60.0f : 46.0f) * scale;
    const float pad = 14.0f * scale;
    const ImVec2 p0(io.DisplaySize.x - pad - w, pad), p1(p0.x + w, p0.y + h);

    /* Gold while driving and on success, ember when it stopped short. */
    const ImU32 frame = outcome < 0 ? IM_COL32(222, 110, 76, 255)
                                    : IM_COL32(222, 178, 76, 255);
    const ImU32 title = outcome < 0 ? IM_COL32(235, 150, 110, 255)
                                    : IM_COL32(222, 185, 100, 255);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(21, 24, 44, 235), 6.0f * scale);
    dl->AddRect(p0, p1, frame, 6.0f * scale, 0, 1.4f * scale);
    dl->AddText(ImVec2(p0.x + 10.0f * scale, p0.y + 7.0f * scale), title,
                driving ? "CONTINUING THE LEGEND"
                        : (outcome > 0 ? "THE LEGEND CONTINUES"
                                       : "HANDOFF STOPPED"));
    dl->AddText(ImVec2(p0.x + 10.0f * scale, p0.y + 25.0f * scale),
                IM_COL32(235, 235, 235, 255), message);
    if (driving) {
        dl->AddText(ImVec2(p0.x + 10.0f * scale, p0.y + 42.0f * scale),
                    IM_COL32(160, 165, 185, 255),
                    "Running at max speed. Press any button to take over.");
    }
}
