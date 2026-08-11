/* The achievement toast: a card that slides in from the right edge of the
 * window, sits, and slides away -- the Steam/Xbox notification, drawn by
 * the host overlay over the presented frame. The game underneath never
 * sees it.
 *
 * Everything is ImDrawList primitives; no textures, no fonts beyond the
 * default. The medal is drawn, not loaded.
 */
extern "C" {
#include "epoch_achievements.h"
}

#include "imgui.h"

#include <math.h>

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
    const float round = 8.0f * scale;

    /* Shadow, card, hairline. */
    dl->AddRectFilled(ImVec2(p0.x + 3, p0.y + 4), ImVec2(p1.x + 3, p1.y + 4),
                      IM_COL32(0, 0, 0, (int)(90 * slide)), round);
    dl->AddRectFilled(p0, p1, IM_COL32(24, 28, 34, (int)(242 * slide)), round);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, (int)(28 * slide)), round);

    /* The medal: a gold disc with a star cut of triangles, ribbon below.
     * Steam has its icon; we have geometry. */
    const float cx = p0.x + card_h * 0.5f;
    const float cy = p0.y + card_h * 0.44f;
    const float r  = card_h * 0.26f;
    const int   alpha = (int)(255 * slide);
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
                  1.5f * scale);
    /* Five-point star inside the disc. */
    {
        const float sr = r * 0.55f, ir = r * 0.22f;
        ImVec2 pts[10];
        for (int i = 0; i < 10; i++) {
            const float a = (float)(i * M_PI / 5.0 - M_PI / 2.0);
            const float rr = (i & 1) ? ir : sr;
            pts[i] = ImVec2(cx + cosf(a) * rr, cy + sinf(a) * rr);
        }
        dl->AddConvexPolyFilled(pts, 10, IM_COL32(140, 100, 20, alpha));
    }

    /* Text: the header line, then the achievement. */
    const float tx = p0.x + card_h * 1.0f;
    const float ty = p0.y + card_h * 0.16f;
    dl->AddText(ImVec2(tx, ty),
                IM_COL32(150, 190, 235, alpha), "ACHIEVEMENT UNLOCKED");
    dl->AddText(ImVec2(tx, ty + 18.0f * scale),
                IM_COL32(235, 235, 235, alpha), t->title);
    if (t->desc[0]) {
        dl->AddText(ImVec2(tx, ty + 34.0f * scale),
                    IM_COL32(160, 168, 178, alpha), t->desc);
    }
}
