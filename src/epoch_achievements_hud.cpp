/* The achievement toast and the Esc-menu browser page.
 *
 * The toast is a card that slides in from the right edge of the window,
 * sits, and slides away -- the Steam/Xbox notification, drawn by the host
 * overlay over the presented frame. The game underneath never sees it.
 *
 * Icons come from achievements/icons/<cart>/<id>.ppm when present and are
 * drawn as run-merged pixel rectangles -- no textures, so it works the
 * same on every ImGui backend. Missing icons get the built-in medal,
 * drawn from primitives.
 */
extern "C" {
#include "epoch_achievements.h"
}

#include "imgui.h"

#include <math.h>
#include <stdio.h>

/* Timeline, in seconds of toast age. Total must stay in sync with
 * TOAST_LIFETIME in epoch_achievements.c. */
static const float SLIDE_IN  = 0.45f;
static const float SLIDE_OUT = 0.6f;
static const float LIFETIME  = 5.2f;

/* Ease-out cubic: fast arrival, gentle stop. */
static float ease_out(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/* 0xAARRGGBB (EaIcon) -> ImGui's ABGR, with an overall fade applied. */
static ImU32 icon_col(uint32_t px, float fade) {
    const unsigned a = (unsigned)(((px >> 24) & 0xFF) * fade);
    return IM_COL32((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF, a);
}

/* Draw a pixel-art icon scaled into a size x size box. Horizontal runs of
 * identical pixels collapse into one rect, which on real pixel art cuts
 * the primitive count by an order of magnitude. */
static void draw_icon(ImDrawList* dl, const EaIcon* ic, ImVec2 pos,
                      float size, float fade) {
    const int   dim = ic->w > ic->h ? ic->w : ic->h;
    const float s = size / (float)dim;
    const float ox = pos.x + (size - ic->w * s) * 0.5f;
    const float oy = pos.y + (size - ic->h * s) * 0.5f;
    for (int y = 0; y < ic->h; y++) {
        int x = 0;
        while (x < ic->w) {
            const uint32_t px = ic->px[y * ic->w + x];
            int run = 1;
            while (x + run < ic->w && ic->px[y * ic->w + x + run] == px) run++;
            if ((px >> 24) != 0) {
                dl->AddRectFilled(
                    ImVec2(ox + x * s, oy + y * s),
                    ImVec2(ox + (x + run) * s, oy + (y + 1) * s),
                    icon_col(px, fade));
            }
            x += run;
        }
    }
}

/* The built-in medal: gold disc, star, ribbon. The fallback when an
 * achievement ships no icon of its own. */
static void draw_medal(ImDrawList* dl, float cx, float cy, float r,
                       float fade) {
    const int alpha = (int)(255 * fade);
    dl->AddTriangleFilled(ImVec2(cx - r * 0.5f, cy + r * 0.4f),
                          ImVec2(cx - r * 0.9f, cy + r * 1.7f),
                          ImVec2(cx - r * 0.05f, cy + r * 1.25f),
                          IM_COL32(178, 44, 48, alpha));
    dl->AddTriangleFilled(ImVec2(cx + r * 0.5f, cy + r * 0.4f),
                          ImVec2(cx + r * 0.9f, cy + r * 1.7f),
                          ImVec2(cx + r * 0.05f, cy + r * 1.25f),
                          IM_COL32(178, 44, 48, alpha));
    dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(212, 175, 55, alpha), 24);
    dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(255, 235, 160, alpha), 24,
                  r * 0.09f);
    const float sr = r * 0.55f, ir = r * 0.22f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; i++) {
        const float a = (float)(i * M_PI / 5.0 - M_PI / 2.0);
        const float rr = (i & 1) ? ir : sr;
        pts[i] = ImVec2(cx + cosf(a) * rr, cy + sinf(a) * rr);
    }
    dl->AddConvexPolyFilled(pts, 10, IM_COL32(140, 100, 20, alpha));
}

/* ------------------------------------------------------------------ */
/* the toast                                                           */
/* ------------------------------------------------------------------ */

extern "C" void epoch_achievements_hud_draw(void) {
    ImGuiIO& io = ImGui::GetIO();
    ea_toast_advance(io.DeltaTime);

    const EaToast* t = ea_toast_current();
    if (!t) return;

    const float scale = io.FontGlobalScale > 0.0f ? io.FontGlobalScale : 1.0f;
    const float card_w = 340.0f * scale;
    const float card_h = 64.0f * scale;
    const float pad    = 14.0f * scale;

    /* Position on the timeline: off right edge -> resting -> off again. */
    float slide;                       /* 0 = hidden, 1 = resting */
    if (t->age < SLIDE_IN) {
        slide = ease_out(t->age / SLIDE_IN);
    } else if (t->age > LIFETIME - SLIDE_OUT) {
        slide = 1.0f - ease_out((t->age - (LIFETIME - SLIDE_OUT)) / SLIDE_OUT);
    } else {
        slide = 1.0f;
    }
    const float hidden_x  = io.DisplaySize.x + 8.0f;
    const float resting_x = io.DisplaySize.x - card_w - pad;
    const float x = hidden_x + (resting_x - hidden_x) * slide;
    const float y = pad;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 p0(x, y), p1(x + card_w, y + card_h);
    const float round = 6.0f * scale;

    /* A royal card: deep night-blue field in a gilded double frame, with
     * a diamond at each corner -- treasure-chest fanfare, not desktop
     * notification. Every stroke is a primitive; no assets. */
    const int a = (int)(255 * slide);
    dl->AddRectFilled(ImVec2(p0.x + 3, p0.y + 4), ImVec2(p1.x + 3, p1.y + 4),
                      IM_COL32(0, 0, 0, (int)(110 * slide)), round);
    dl->AddRectFilled(p0, p1, IM_COL32(21, 24, 44, (int)(246 * slide)), round);
    /* Candlelight falling from above. */
    dl->AddRectFilled(ImVec2(p0.x + 4, p0.y + 2),
                      ImVec2(p1.x - 4, p0.y + card_h * 0.24f),
                      IM_COL32(222, 178, 76, (int)(16 * slide)), round);
    /* Antique gold outside, bright gold within. */
    dl->AddRect(p0, p1, IM_COL32(110, 84, 28, a), round, 0, 2.6f * scale);
    dl->AddRect(ImVec2(p0.x + 3.0f * scale, p0.y + 3.0f * scale),
                ImVec2(p1.x - 3.0f * scale, p1.y - 3.0f * scale),
                IM_COL32(222, 178, 76, a), round * 0.6f, 0, 1.2f * scale);
    /* Corner diamonds, tipped with a highlight. */
    const float dr = 4.2f * scale;
    const ImVec2 corners[4] = { p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y) };
    for (int i = 0; i < 4; i++) {
        const ImVec2 c = corners[i];
        dl->AddQuadFilled(ImVec2(c.x, c.y - dr), ImVec2(c.x + dr, c.y),
                          ImVec2(c.x, c.y + dr), ImVec2(c.x - dr, c.y),
                          IM_COL32(222, 178, 76, a));
        dl->AddQuad(ImVec2(c.x, c.y - dr), ImVec2(c.x + dr, c.y),
                    ImVec2(c.x, c.y + dr), ImVec2(c.x - dr, c.y),
                    IM_COL32(120, 90, 30, a), 1.0f);
        dl->AddCircleFilled(ImVec2(c.x - dr * 0.25f, c.y - dr * 0.25f),
                            dr * 0.18f, IM_COL32(255, 240, 190, a));
    }
    /* A small spark at the top edge's center, because whimsy. */
    {
        const float mx = (p0.x + p1.x) * 0.5f;
        const float sr = 2.6f * scale;
        dl->AddQuadFilled(ImVec2(mx, p0.y - sr), ImVec2(mx + sr, p0.y),
                          ImVec2(mx, p0.y + sr), ImVec2(mx - sr, p0.y),
                          IM_COL32(255, 240, 190, a));
    }

    /* Custom icon when the pack ships one; the medal otherwise. */
    const EaIcon* ic = ea_icon_get(t->id);
    if (ic) {
        const float box = card_h * 0.72f;
        draw_icon(dl, ic, ImVec2(p0.x + card_h * 0.14f, p0.y + card_h * 0.14f),
                  box, slide);
    } else {
        draw_medal(dl, p0.x + card_h * 0.5f, p0.y + card_h * 0.44f,
                   card_h * 0.26f, slide);
    }

    /* Text: a gilded header line, then the achievement. */
    const float tx = p0.x + card_h * 1.0f;
    const float ty = p0.y + card_h * 0.16f;
    dl->AddText(ImVec2(tx, ty),
                IM_COL32(222, 185, 100, a), "ACHIEVEMENT UNLOCKED");
    dl->AddText(ImVec2(tx, ty + 18.0f * scale),
                IM_COL32(245, 240, 225, a), t->title);
    if (t->desc[0]) {
        dl->AddText(ImVec2(tx, ty + 34.0f * scale),
                    IM_COL32(178, 170, 140, a), t->desc);
    }
}

/* ------------------------------------------------------------------ */
/* the Esc-menu page                                                   */
/* ------------------------------------------------------------------ */

extern "C" void epoch_achievements_menu_draw(void) {
    const EaSet* set = epoch_achievements_set();
    if (!set || set->count == 0) return;

    int unlocked = 0, total = 0;
    epoch_achievements_progress(&unlocked, &total);

    char header[64];
    snprintf(header, sizeof(header), "Achievements  %d / %d###achievements",
             unlocked, total);
    if (!ImGui::CollapsingHeader(header)) return;

    ImGui::TextDisabled("Earned over every playthrough of this cart. The\n"
                        "toast pops over the window, never in the game.");
    ImGui::Spacing();

    const float row = ImGui::GetTextLineHeight() * 2.6f;
    const float icon_px = row * 0.78f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < set->count; i++) {
        const EaAchievement* a = &set->list[i];
        if (a->n_conds == 0 && !a->unlocked) continue;   /* broken entry */
        const float fade = a->unlocked ? 1.0f : 0.35f;

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const EaIcon* ic = ea_icon_get(a->id);
        if (ic) {
            draw_icon(dl, ic, pos, icon_px, fade);
        } else {
            draw_medal(dl, pos.x + icon_px * 0.5f, pos.y + icon_px * 0.4f,
                       icon_px * 0.3f, fade);
        }
        ImGui::Dummy(ImVec2(icon_px, row - ImGui::GetStyle().ItemSpacing.y));
        ImGui::SameLine(icon_px + ImGui::GetStyle().ItemSpacing.x * 2.0f);

        ImGui::BeginGroup();
        if (a->unlocked) {
            ImGui::TextUnformatted(a->title[0] ? a->title : a->id);
        } else {
            ImGui::TextDisabled("%s", a->title[0] ? a->title : a->id);
        }
        ImGui::TextDisabled("%s", a->desc[0] ? a->desc : " ");
        ImGui::EndGroup();
    }
}
