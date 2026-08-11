/* The Secrets section of the Esc menu, and the progress card the typist
 * shows while it works.
 *
 * The launcher's Secrets dialog tells you what to type; this types it.
 * Open the game's own secret screen (Farore, or an NPC's prompt), then
 * pick the secret here and watch the cursor walk the grid.
 */
extern "C" {
#include "epoch_secrets.h"
}

#include "imgui.h"

#include <stdio.h>

/* constants/common/secrets.s. The base secret is told to this NPC; the
 * return secret ($10 higher) is what they answer with, for Farore. */
struct NpcSecret { const char* name; unsigned char index; };

static const NpcSecret AGES_NPCS[] = {
    {"King Zora", 0x00}, {"Great Fairy", 0x01}, {"Troy", 0x02},
    {"Plen", 0x03}, {"Library", 0x04}, {"Tokay", 0x05},
    {"Mamamu Yan", 0x06}, {"Tingle", 0x07}, {"Elder", 0x08},
    {"Symmetry City", 0x09},
};
static const NpcSecret SEASONS_NPCS[] = {
    {"Clock Shop", 0x20}, {"Graveyard", 0x21}, {"Subrosian", 0x22},
    {"Diver", 0x23}, {"Smith", 0x24}, {"Pirate", 0x25},
    {"Temple", 0x26}, {"Deku Scrub", 0x27}, {"Biggoron", 0x28},
    {"Ruul Village", 0x29},
};

static void secret_row(const NpcSecret& npc, bool ready) {
    ImGui::PushID((int)npc.index);
    if (!ready) ImGui::BeginDisabled();
    if (ImGui::Button("Type")) {
        epoch_secrets_type_current(ES_TYPE_SHORT, npc.index);
    }
    ImGui::SameLine();
    if (ImGui::Button("Type return")) {
        epoch_secrets_type_current(ES_TYPE_SHORT,
                                   (unsigned char)(npc.index + 0x10));
    }
    if (!ready) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(npc.name);
    ImGui::PopID();
}

extern "C" void epoch_secrets_panel_draw(void) {
    const bool have_save = epoch_secrets_have_save();
    const bool busy = epoch_secrets_busy();
    const bool ready = have_save && !busy;

    ImGui::TextDisabled(
        "Open the game's own secret screen first (Farore, or an NPC who\n"
        "asks for one), then pick a secret here and it will be typed in.");

    if (!have_save) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.45f, 1.0f),
                           "No file loaded yet -- start a game first.");
        return;
    }

    int done = 0, total = 0;
    const char* message = "";
    epoch_secrets_status(&done, &total, &message);
    if (busy) {
        ImGui::TextColored(ImVec4(0.87f, 0.73f, 0.39f, 1.0f), "%s", message);
        if (total > 0) {
            ImGui::ProgressBar((float)done / (float)total,
                               ImVec2(-1.0f, 0.0f));
        }
        if (ImGui::Button("Stop")) epoch_secrets_cancel();
        return;
    }
    if (message[0]) ImGui::TextDisabled("%s", message);

    ImGui::Spacing();
    ImGui::TextDisabled("Whole-game secrets");
    if (!ready) ImGui::BeginDisabled();
    if (ImGui::Button("Type the game secret")) {
        epoch_secrets_type_current(ES_TYPE_GAME, 0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Type the ring secret")) {
        epoch_secrets_type_current(ES_TYPE_RING, 0);
    }
    if (!ready) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("Secrets to tell in Oracle of Ages");
    for (const NpcSecret& npc : AGES_NPCS) secret_row(npc, ready);

    ImGui::Spacing();
    ImGui::TextDisabled("Secrets to tell in Oracle of Seasons");
    for (const NpcSecret& npc : SEASONS_NPCS) secret_row(npc, ready);

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Secrets are made from this file's own Game ID, so they are\n"
        "yours -- codes from a website will not validate here.");
}

/* The card that shows while typing, drawn over the game so the progress
 * is visible with the menu closed. */
extern "C" void epoch_secrets_hud_draw(void) {
    if (!epoch_secrets_busy()) return;

    int done = 0, total = 0;
    const char* message = "";
    epoch_secrets_status(&done, &total, &message);

    ImGuiIO& io = ImGui::GetIO();
    const float scale = io.FontGlobalScale > 0.0f ? io.FontGlobalScale : 1.0f;
    const float w = 240.0f * scale, h = 46.0f * scale, pad = 14.0f * scale;
    const ImVec2 p0(pad, pad), p1(pad + w, pad + h);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(21, 24, 44, 235), 6.0f * scale);
    dl->AddRect(p0, p1, IM_COL32(222, 178, 76, 255), 6.0f * scale, 0,
                1.4f * scale);
    dl->AddText(ImVec2(p0.x + 10.0f * scale, p0.y + 7.0f * scale),
                IM_COL32(222, 185, 100, 255), "ENTERING SECRET");

    char line[64];
    snprintf(line, sizeof(line), "%d / %d symbols", done, total);
    dl->AddText(ImVec2(p0.x + 10.0f * scale, p0.y + 25.0f * scale),
                IM_COL32(235, 235, 235, 255), line);

    if (total > 0) {
        const float bar_y = p1.y - 6.0f * scale;
        const float full = w - 20.0f * scale;
        dl->AddRectFilled(ImVec2(p0.x + 10.0f * scale, bar_y),
                          ImVec2(p0.x + 10.0f * scale + full, bar_y + 3.0f),
                          IM_COL32(60, 66, 86, 255));
        dl->AddRectFilled(
            ImVec2(p0.x + 10.0f * scale, bar_y),
            ImVec2(p0.x + 10.0f * scale + full * (float)done / (float)total,
                   bar_y + 3.0f),
            IM_COL32(222, 178, 76, 255));
    }
}
