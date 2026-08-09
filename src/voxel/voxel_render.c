/* Diorama renderer + frame hook for the voxel mode.
 *
 * The scheme is the classic "voxel space" column march, adapted to a
 * screen-sized diorama: for each screen column, walk the visible map from
 * far (top) to near (bottom), project each cell's top surface under the
 * tilted camera, and fill the gap a height drop opens with that cell's
 * front wall. Painter's order per column — near rows overdraw far rows —
 * so no depth buffer is needed.
 *
 * The whole scene is rendered at an integer SCALE above the GB screen
 * (default 3x, VOXEL_SCALE=1..4 to override) and handed to the runtime
 * through the scaled frame hook. Texels stay chunky -- that is the pixel
 * art -- but silhouettes, domes and the camera tilt resolve at sub-GB
 * precision instead of a 160x144 staircase.
 *
 * The terrain texture is decoded straight from the BG tilemap (see
 * VoxTileGrid::tex), which keeps every palette, season tint and animation
 * exactly as the cart drew it -- without the sprites the composed frame
 * bakes in. Sprites are re-decoded from OAM/VRAM and stood upright as
 * billboards, interleaved with the terrain rows in painter's order so a
 * wall in front of a character actually hides them.
 */
#include "voxel.h"
#include "voxel_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_sdl.h"

#ifdef GB_HAS_SDL2
#include <SDL.h>
#endif

#define VOX_MAX_SCALE 4

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static int g_mode = VOXEL_MODE_OFF;
static int g_scale = 3;
static uint32_t g_out[GB_FRAMEBUFFER_SIZE * VOX_MAX_SCALE * VOX_MAX_SCALE];
static VoxTileGrid g_grid;
static VoxSpriteList g_sprites;

/* Per-mode camera: row squash (cos pitch) and per-height-unit lift in
 * pixels (sin pitch, scaled to taste). */
typedef struct { float squash; float lift; } VoxCam;
static const VoxCam VOX_CAMS[VOXEL_MODE_COUNT] = {
    {1.00f, 0.0f},   /* OFF (unused) */
    {0.966f, 1.1f},  /* 15 deg */
    {0.866f, 2.2f},  /* 30 deg */
    {0.707f, 3.2f},  /* 45 deg */
};

/* Extrusion units per height class. Water is negative: it sinks. */
static const float VOX_UNITS[5] = {
    [VOX_H_WATER] = -1.5f,
    [VOX_H_FLOOR] = 0.0f,
    [VOX_H_LOW]   = 1.0f,
    [VOX_H_MID]   = 3.0f,
    [VOX_H_HIGH]  = 6.0f,
};

int voxel_get_mode(void) { return g_mode; }

void voxel_set_mode(int mode) {
    if (mode < 0 || mode >= VOXEL_MODE_COUNT) mode = VOXEL_MODE_OFF;
    if (mode != g_mode) {
        g_mode = mode;
        static const char* names[VOXEL_MODE_COUNT] = {"OFF", "15", "30", "45",
                                                      "CHASE"};
        fprintf(stderr, "[VOXEL] mode: %s\n", names[mode]);
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint32_t shade(uint32_t c, int mul /* 0..~272 */) {
    /* Clamp per channel: brightening (mul > 256) overflowed bright
     * channels into their neighbours' bytes, which is why top-lit yellow
     * flowers came out the wrong colour entirely. */
    uint32_t r = ((c & 0x0000FF) * (uint32_t)mul) >> 8;
    uint32_t g = (((c >> 8) & 0xFF) * (uint32_t)mul) >> 8;
    uint32_t b = (((c >> 16) & 0xFF) * (uint32_t)mul) >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* Height in extrusion units at a world (screen-space) position. Float in,
 * so at render scale the dome profiles resolve smoothly. */
static inline float height_at(const VoxTileGrid* grid, float x, float y) {
    float px = x + (float)grid->fine_x;
    float py = y + (float)grid->fine_y;
    int tx = (int)px >> 3;
    int ty = (int)py >> 3;
    if (tx < 0) tx = 0;
    if (tx >= VOX_TILES_W) tx = VOX_TILES_W - 1;
    if (ty < 0) ty = 0;
    if (ty >= VOX_TILES_H) ty = VOX_TILES_H - 1;
    float h = VOX_UNITS[grid->height[ty][tx]];

    /* Raised foliage gets a domed top: height peaks at the centre of the
     * 16px room cell and rounds off toward its edges, so trees and bushes
     * read as canopies instead of flat-topped crates. Everything
     * non-leafy (walls, cliffs, fences) stays architectural and flat. */
    if (h >= VOX_UNITS[VOX_H_MID] && grid->leafy[ty][tx]) {
        float cx = px - 16.0f * floorf(px / 16.0f);
        float cy = py - 16.0f * floorf(py / 16.0f);
        float dx = (cx - 7.5f) / 8.0f;
        float dy = (cy - 7.5f) / 8.0f;
        float k = 1.0f - 0.22f * (dx * dx + dy * dy);
        if (k < 0.62f) k = 0.62f;
        h *= k;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* sky                                                                 */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t zenith, horizon, cloud; } VoxSkyPalette;

/* 0xFFRRGGBB, matching the framebuffer. */
static const VoxSkyPalette VOX_SKIES[] = {
    [VOX_SKY_AGES_PRESENT] = {0xFF2C68B4u, 0xFF9CC8E0u, 0xFFDCE8F0u},
    [VOX_SKY_AGES_PAST]    = {0xFF9C6A4Au, 0xFFF0D8A8u, 0xFFF8ECC8u},
    [VOX_SKY_SPRING]       = {0xFF4880C0u, 0xFFC0D0E8u, 0xFFE4ECF4u},
    [VOX_SKY_SUMMER]       = {0xFF2878D0u, 0xFF8CC8E8u, 0xFFE0F0F8u},
    [VOX_SKY_AUTUMN]       = {0xFF90507Cu, 0xFFF0B888u, 0xFFF8D8B0u},
    [VOX_SKY_WINTER]       = {0xFF7890B0u, 0xFFC0C8D8u, 0xFFE8ECF0u},
    [VOX_SKY_SUBROSIA]     = {0xFF3C0820u, 0xFFA01830u, 0xFFD04050u},
};

static inline uint32_t lerp_color(uint32_t a, uint32_t b, int t /*0..256*/) {
    uint32_t rb = ((a & 0x00FF00FFu) * (256 - t) + (b & 0x00FF00FFu) * t) >> 8;
    uint32_t g  = ((a & 0x0000FF00u) * (256 - t) + (b & 0x0000FF00u) * t) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

/* Cheap smooth value noise for the clouds: two octaves of cosine bumps.
 * Coordinates in GB pixels (floats, so the sky stays smooth at scale). */
static inline int cloud_density(float x, float y, int t) {
    float fx = (x + (float)t) * 0.045f;
    float fy = y * 0.11f;
    float v = cosf(fx) * cosf(fy * 0.7f + 1.7f)
            + 0.5f * cosf(fx * 2.3f + 0.9f) * cosf(fy * 1.6f);
    int d = (int)((v - 0.55f) * 190.0f);
    if (d < 0) d = 0;
    if (d > 110) d = 110;
    return d;
}

static void vox_paint_sky(int kind, uint32_t* out, int S) {
    const VoxSkyPalette* p = &VOX_SKIES[kind];
    const int OW = GB_SCREEN_WIDTH * S;
    const int OH = GB_SCREEN_HEIGHT * S;

    /* Slow drift; wraps harmlessly. Subrosia's "clouds" are ember glow and
     * drift faster. */
    static int t = 0;
    t += (kind == VOX_SKY_SUBROSIA) ? 3 : 1;
    int drift = t >> 3;

    for (int Y = 0; Y < OH; Y++) {
        float gy = (float)Y / (float)S;
        /* Horizon low on the screen, where the diorama's far edge sits. */
        int g = (int)(gy * 256.0f / (float)(GB_SCREEN_HEIGHT + 24));
        uint32_t base = lerp_color(p->zenith, p->horizon, g);
        for (int X = 0; X < OW; X++) {
            int d = cloud_density((float)X / (float)S, gy, drift);
            out[Y * OW + X] = d ? lerp_color(base, p->cloud, d * 2) : base;
        }
    }
}

/* ------------------------------------------------------------------ */
/* chase camera                                                        */
/* ------------------------------------------------------------------ */

static inline int top_rim(int S) { return 2 * S; }

/* Raw cell height, no dome shaping. The dome is a top-down flourish: seen
 * edge-on it puts a peak in the middle of every cell, and a row of them
 * aliases into the striped shark-fins that made tree lines look crazy. */
static inline float height_raw(const VoxTileGrid* grid, float x, float y) {
    int tx = (int)(x + (float)grid->fine_x) >> 3;
    int ty = (int)(y + (float)grid->fine_y) >> 3;
    if (tx < 0) tx = 0;
    if (tx >= VOX_TILES_W) tx = VOX_TILES_W - 1;
    if (ty < 0) ty = 0;
    if (ty >= VOX_TILES_H) ty = VOX_TILES_H - 1;
    return VOX_UNITS[grid->height[ty][tx]];
}

/* Tent-filtered height for the perspective camera: 3x3 taps at 5px
 * spacing (weights 4/2/1) turn a clump of raised cells into one rounded
 * mass and a tree line into a smooth ridge, while a lone tree still
 * stands as its own bump. Cheap enough for the ~100k samples a frame of
 * raycasting takes, because the taps skip the dome maths entirely. */
static inline float chase_height_at(const VoxTileGrid* grid, float x, float y) {
    const float o = 3.0f;
    float c = height_raw(grid, x, y);
    float e = height_raw(grid, x - o, y) + height_raw(grid, x + o, y) +
              height_raw(grid, x, y - o) + height_raw(grid, x, y + o);
    return (c * 4.0f + e) * (1.0f / 8.0f);
}

static inline uint32_t chase_tex_at(const VoxTileGrid* grid, float x, float y) {
    int px = (int)(x + (float)grid->fine_x);
    int py = (int)(y + (float)grid->fine_y);
    if (px < 0) px = 0;
    if (px >= VOX_TEX_W) px = VOX_TEX_W - 1;
    if (py < 0) py = 0;
    if (py >= VOX_TEX_H) py = VOX_TEX_H - 1;
    return grid->tex[py * VOX_TEX_W + px];
}

/* Third person: a camera floating behind the player, looking the way they
 * face, raycasting the same heightfield the diorama extrudes -- the
 * classic "voxel space" scheme finally used for what it was invented
 * for. Planar (unnormalized) rays keep walls straight; a per-column
 * y-buffer gives front-to-back occlusion with no depth buffer. */
static void render_chase(GBContext* ctx, const VoxTileGrid* grid,
                         const VoxSpriteList* sprites, int S, uint32_t* out) {
    const int OW = GB_SCREEN_WIDTH * S;
    const int OH = GB_SCREEN_HEIGHT * S;
    const int world_top = grid->hud_rows;
    const float HPX = 3.2f;             /* vertical px per height unit */
    const float FAR = 230.0f;

    /* Sky first; the ground overwrites what it owns. */
    if (grid->sky != VOX_SKY_NONE) {
        vox_paint_sky(grid->sky, out, S);
    } else {
        for (int Y = 0; Y < OH; Y++) {
            int l = 26 + (Y * 20) / OH;
            uint32_t c = 0xFF000000u
                         | ((uint32_t)(l - 8 > 0 ? l - 8 : 0) << 16)
                         | ((uint32_t)l << 8) | (uint32_t)(l + 6);
            for (int X = 0; X < OW; X++) out[Y * OW + X] = c;
        }
    }

    /* Hold the last known anchor through the frames where the game state
     * is unreadable (room transitions): re-targeting a default centre made
     * the camera swim away and back on every screen change. */
    static float lx = 80.0f, ly = 104.0f;
    if (grid->link_known) {
        lx = (float)grid->link_sx;
        ly = (float)grid->link_feet_sy;
    }

    /* The camera is on the stick, not on the d-pad: auto-following Link's
     * 4-way facing swung the world around on every tap, which played as
     * chaos. Yaw is yours -- right stick, or Q/E on the keyboard -- and
     * holds steady otherwise. Default: behind Link, looking north. */
    static float yaw = -1.5708f;
#ifdef GB_HAS_SDL2
    {
        float turn = 0.0f;
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        if (keys) {
            if (keys[SDL_SCANCODE_Q]) turn -= 1.0f;
            if (keys[SDL_SCANCODE_E]) turn += 1.0f;
        }
        SDL_GameController* pad = SDL_GameControllerFromPlayerIndex(0);
        if (pad) {
            Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
            if (ax > 6000 || ax < -6000) turn += (float)ax / 32767.0f;
        }
        yaw += turn * 0.055f;
    }
#endif

    /* Screen space has y growing DOWNWARD, so the right-handed pairing
     * (-sin, cos) points screen-left and the view came out mirrored:
     * pushing the stick right swung the world left, and forward walked
     * toward the camera. (+sin, -cos) is right in screen space. */
    const float fxv = cosf(yaw), fyv = sinf(yaw);
    const float rxv = fyv, ryv = -fxv;      /* screen-space right vector */

    const float BACK = 62.0f;               /* camera behind the player */
    const float CAMH = 32.0f;               /* and above their ground */
    /* Ease the camera's position so Link's whole-pixel steps (and room
     * transitions) glide instead of ticking. */
    static float scx_f = 0.0f, scy_f = 0.0f;
    static bool cam_live = false;
    float tx = lx - fxv * BACK;
    float ty = ly - fyv * BACK;
    if (!cam_live) { scx_f = tx; scy_f = ty; cam_live = true; }
    scx_f += (tx - scx_f) * 0.35f;
    scy_f += (ty - scy_f) * 0.35f;
    float cx = scx_f, cy = scy_f;
    float cg = chase_height_at(grid, cx, cy);
    if (cg < 0.0f) cg = 0.0f;
    const float cz = cg * HPX + CAMH;

    const float focal = (float)OW * 0.62f;  /* ~78 degree field of view */
    const int horizon = (int)((float)OH * 0.36f);
    const uint32_t fog = (grid->sky != VOX_SKY_NONE)
        ? VOX_SKIES[grid->sky].horizon : 0xFF2C2620u;

    for (int X = 0; X < OW; X++) {
        float ndc = ((float)X - (float)OW * 0.5f) / focal;
        float rx = fxv + rxv * ndc;
        float ry = fyv + ryv * ndc;
        int ybuf = OH;
        float ph = 0.0f;
        for (float d = 8.0f; d < FAR; d += (d < 70.0f ? 0.6f : 1.4f)) {
            float wx2 = cx + rx * d;
            float wy2 = cy + ry * d;
            /* Past the room's edge the heightfield has nothing to say, but
             * a sky-coloured hole in the floor is worse than a continued
             * ground plane: clamp the sample to the room border so the
             * world runs out into fog instead of falling away. */
            bool inside = wx2 >= 0.0f && wx2 < (float)GB_SCREEN_WIDTH &&
                          wy2 >= (float)world_top &&
                          wy2 < (float)GB_SCREEN_HEIGHT;
            float sx2 = wx2 < 0.0f ? 0.0f
                      : (wx2 > (float)(GB_SCREEN_WIDTH - 1)
                         ? (float)(GB_SCREEN_WIDTH - 1) : wx2);
            float sy2 = wy2 < (float)world_top ? (float)world_top
                      : (wy2 > (float)(GB_SCREEN_HEIGHT - 1)
                         ? (float)(GB_SCREEN_HEIGHT - 1) : wy2);
            float h = chase_height_at(grid, sx2, sy2);
            if (!inside) {
                /* Outside the room the border cell repeats, so a treeline
                 * at the edge keeps going instead of ending at a cliff of
                 * flat ground -- then settles to the fog plane over ~70px
                 * so it reads as "the world continues" rather than "an
                 * infinite wall". */
                float outd = 0.0f;
                if (wx2 < 0.0f) outd = -wx2;
                else if (wx2 > (float)(GB_SCREEN_WIDTH - 1))
                    outd = wx2 - (float)(GB_SCREEN_WIDTH - 1);
                if (wy2 < (float)world_top) {
                    float t2 = (float)world_top - wy2;
                    if (t2 > outd) outd = t2;
                } else if (wy2 > (float)(GB_SCREEN_HEIGHT - 1)) {
                    float t2 = wy2 - (float)(GB_SCREEN_HEIGHT - 1);
                    if (t2 > outd) outd = t2;
                }
                float k = 1.0f - outd * (1.0f / 70.0f);
                if (k < 0.0f) k = 0.0f;
                h *= k;
            }
            int sy = horizon + (int)((cz - h * HPX) * focal / d);
            if (sy < ybuf) {
                uint32_t c = chase_tex_at(grid, sx2, sy2);
                if (h < 0.0f) {
                    c = shade(c, 190);
                    c = (c & 0xFFFFFF00u) | 0x00000050u;
                }
                if (!inside) c = lerp_color(c, fog, 96);
                int t = (int)((d - 70.0f) * 256.0f / (FAR - 70.0f));
                if (t < 0) t = 0;
                if (t > 150) t = 150;
                c = lerp_color(c, fog, t);
                /* A step up in height means this span is mostly the riser's
                 * front face: darken it below a thin lit rim, so cliffs and
                 * tree lines read as forms instead of vertical streaks. */
                bool face = h > ph + 0.25f;
                uint32_t cw = face ? shade(c, 178) : c;
                int rim = top_rim(S);
                int top = sy < 0 ? 0 : sy;
                for (int yy = top; yy < ybuf; yy++) {
                    out[yy * OW + X] = (face && yy >= top + rim) ? cw : c;
                }
                ybuf = top;
                if (ybuf <= 0) break;
            }
            ph = h;
        }
    }

    /* Sprites: adjacent 8px OAM entries at the same y are one character,
     * and drawing the halves as independent billboards let them drift into
     * twins with depth. Group each horizontal run, then draw runs farthest
     * first, scaled by depth, standing on their ground. */
    typedef struct {
        int members[6];      /* sprite indices, left to right */
        int n;
        int x, y, sh;
        float dep, lat, g;
    } ChaseRun;
    ChaseRun runs[VOX_MAX_SPRITES];
    bool used[VOX_MAX_SPRITES] = {false};
    int nr = 0;
    for (int i = 0; i < sprites->count; i++) {
        if (used[i]) continue;
        const VoxSprite* s = &sprites->entries[i];
        /* Find the leftmost member of this run. */
        int lx0 = s->x;
        for (bool moved = true; moved;) {
            moved = false;
            for (int j = 0; j < sprites->count; j++) {
                if (!used[j] && sprites->entries[j].y == s->y &&
                    sprites->entries[j].x == lx0 - 8) {
                    lx0 -= 8;
                    moved = true;
                }
            }
        }
        ChaseRun* r = &runs[nr];
        r->n = 0;
        r->x = lx0;
        r->y = s->y;
        r->sh = s->tall ? 16 : 8;
        for (int step = 0; step < 6; step++) {
            int want = lx0 + step * 8;
            int found = -1;
            for (int j = 0; j < sprites->count; j++) {
                if (!used[j] && sprites->entries[j].y == s->y &&
                    sprites->entries[j].x == want) { found = j; break; }
            }
            if (found < 0) break;
            used[found] = true;
            r->members[r->n++] = found;
        }
        if (r->n == 0) continue;

        float pxc = (float)r->x + (float)(r->n * 8) * 0.5f;
        float pyc = (float)(r->y + r->sh);
        r->dep = (pxc - cx) * fxv + (pyc - cy) * fyv;
        r->lat = (pxc - cx) * rxv + (pyc - cy) * ryv;
        if (r->dep < 24.0f || r->dep > FAR) continue;
        r->g = chase_height_at(grid, pxc, pyc);
        if (r->g < 0.0f) r->g = 0.0f;
        nr++;
    }
    for (int a = 1; a < nr; a++) {
        ChaseRun tmp = runs[a];
        int b = a - 1;
        while (b >= 0 && runs[b].dep < tmp.dep) {
            runs[b + 1] = runs[b];
            b--;
        }
        runs[b + 1] = tmp;
    }
    for (int k = 0; k < nr; k++) {
        const ChaseRun* r = &runs[k];
        float scale = focal / r->dep;
        int wgb = r->n * 8;
        int cxp = (int)((float)OW * 0.5f + r->lat / r->dep * focal);
        int byp = horizon + (int)((cz - r->g * HPX) * focal / r->dep);
        int w2 = (int)((float)wgb * scale / (float)S + 0.5f) * S;
        int h2 = (int)((float)r->sh * scale / (float)S + 0.5f) * S;
        if (w2 < 2 || h2 < 2) continue;
        int left = cxp - w2 / 2;
        for (int m = 0; m < r->n; m++) {
            const VoxSprite* s = &sprites->entries[r->members[m]];
            for (int row = 0; row < r->sh; row++) {
                uint32_t px8[8];
                vox_decode_sprite_row(ctx, s, row, px8);
                int y0 = byp - h2 + row * h2 / r->sh;
                int y1 = byp - h2 + (row + 1) * h2 / r->sh;
                for (int yy = y0; yy < y1; yy++) {
                    if (yy < 0 || yy >= OH) continue;
                    for (int c2 = 0; c2 < 8; c2++) {
                        if (!px8[c2]) continue;
                        int gcol = m * 8 + c2;
                        int x0 = left + gcol * w2 / wgb;
                        int x1 = left + (gcol + 1) * w2 / wgb;
                        for (int xx = x0; xx < x1; xx++) {
                            if (xx < 0 || xx >= OW) continue;
                            out[yy * OW + xx] = px8[c2];
                        }
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* the diorama                                                         */
/* ------------------------------------------------------------------ */

void vox_render(GBContext* ctx, const VoxTileGrid* grid,
                const VoxSpriteList* sprites, const uint32_t* fb,
                int mode, int scale, uint32_t* out) {
    const VoxCam cam = VOX_CAMS[mode];
    if (scale < 1) scale = 1;
    if (scale > VOX_MAX_SCALE) scale = VOX_MAX_SCALE;
    const int S = scale;
    const float fS = (float)S;
    const int OW = GB_SCREEN_WIDTH * S;
    const int OH = GB_SCREEN_HEIGHT * S;

    /* A full-screen menu owns the display: pass the frame through (the
     * live hook returns NULL before ever calling here; this path serves
     * direct callers like vox_shot). */
    if (grid->flat) {
        for (int Y = 0; Y < OH; Y++) {
            const uint32_t* src = fb + (Y / S) * GB_SCREEN_WIDTH;
            for (int X = 0; X < OW; X++) out[Y * OW + X] = src[X / S];
        }
        return;
    }

    /* The status bar is a band at the top; the world is everything under it
     * and stays flat inside its own band at the end. */
    const int world_top = grid->hud_rows;
    const int world_h = GB_SCREEN_HEIGHT - world_top;
    if (world_h <= 0) {
        for (int Y = 0; Y < OH; Y++) {
            const uint32_t* src = fb + (Y / S) * GB_SCREEN_WIDTH;
            for (int X = 0; X < OW; X++) out[Y * OW + X] = src[X / S];
        }
        return;
    }

    if (mode == VOXEL_MODE_CHASE) {
        render_chase(ctx, grid, sprites, S, out);
        goto compose_overlays;
    }

    /* Squashing the world frees vertical room; spend it lifting the diorama
     * so tall geometry has somewhere to go without clipping off the top. */
    const float headroom = VOX_UNITS[VOX_H_HIGH] * cam.lift;
    float y_off = (float)world_top + ((float)world_h * (1.0f - cam.squash)) * 0.5f;
    y_off += headroom * 0.55f;

    /* Backdrop. Outdoors on the Oracles carts this is a sky that follows
     * the game's own state; anywhere else it stays the quiet dark fade. */
    if (grid->sky != VOX_SKY_NONE) {
        vox_paint_sky(grid->sky, out, S);
    } else {
        for (int Y = 0; Y < OH; Y++) {
            int l = 26 + (Y / S) * 20 / GB_SCREEN_HEIGHT;
            uint32_t c = 0xFF000000u
                         | ((uint32_t)(l - 8 > 0 ? l - 8 : 0) << 16)
                         | ((uint32_t)l << 8) | (uint32_t)(l + 6);
            for (int X = 0; X < OW; X++) out[Y * OW + X] = c;
        }
    }

    /* Water animates: a slow ripple in the shading. */
    static int water_t = 0;
    water_t++;

    /* When real heights return after a transition's flat slide, grow the
     * extrusion in over a few frames -- snapping from flat to full relief
     * in one frame reads as a glitch, growing in reads as intent. */
    bool extruded = false;
    for (int ty = 0; ty < VOX_TILES_H && !extruded; ty++) {
        for (int tx = 0; tx < VOX_TILES_W; tx++) {
            if (VOX_UNITS[grid->height[ty][tx]] != 0.0f) { extruded = true; break; }
        }
    }
    static float grow = 1.0f;
    static bool was_extruded = false;
    if (extruded && !was_extruded) grow = 0.0f;
    was_extruded = extruded;
    grow += (1.0f - grow) * 0.18f;
    if (grow > 0.999f) grow = 1.0f;
    const float lift = cam.lift * grow;

    /* Link's ground height, eased over a few frames: point-sampling the
     * cell under his feet made his billboard teleport a full step the
     * instant he crossed onto raised or lowered ground. */
    static float link_ground_ease = 0.0f;
    static bool link_ease_live = false;
    if (grid->link_known) {
        int fy = grid->link_feet_sy;
        if (fy < world_top) fy = world_top;
        if (fy >= GB_SCREEN_HEIGHT) fy = GB_SCREEN_HEIGHT - 1;
        float now = height_at(grid, (float)grid->link_sx, (float)fy);
        if (now < 0.0f) now = 0.0f;
        if (!link_ease_live) {
            link_ground_ease = now;
            link_ease_live = true;
        }
        link_ground_ease += (now - link_ground_ease) * 0.25f;
    } else {
        link_ease_live = false;
    }

    /* One pass over output rows, far to near, terrain and sprites together
     * in painter's order. Per-output-column memory of the previous row's
     * projection drives the front-wall fills. */
    static int prev_sy[GB_SCREEN_WIDTH * VOX_MAX_SCALE];
    for (int X = 0; X < OW; X++) prev_sy[X] = -1;

    for (int J = world_top * S; J < (GB_SCREEN_HEIGHT + 16) * S; J++) {
        const float wy = (float)J / fS;
        if (J < OH) {
            const int tex_row = (int)(wy + (float)grid->fine_y);
            for (int X = 0; X < OW; X++) {
                const float wx = (float)X / fS;
                float h = height_at(grid, wx, wy);
                int sy = (int)((y_off + (wy - (float)world_top) * cam.squash
                                - h * lift) * fS + 0.5f);
                const int tex_col = (int)(wx + (float)grid->fine_x);
                uint32_t tex = grid->tex[tex_row * VOX_TEX_W + tex_col];

                if (h < 0.0f) {
                    /* Water: tint toward deep blue so sunk cells read as
                     * liquid, with a slow moving shimmer on top. */
                    int rip = (int)(12.0f * sinf(wx * 0.42f + wy * 0.27f
                                                 + (float)water_t * 0.09f));
                    tex = shade(tex, 190 + rip);
                    tex = (tex & 0xFFFFFF00u) | 0x00000050u;
                }

                if (prev_sy[X] >= 0 && sy > prev_sy[X] + 1) {
                    /* Height dropped toward the viewer: the far cell's front
                     * wall is exposed. Texture it by tiling that cell's own
                     * 8px artwork down the face, darkening with a little
                     * falloff so faces read as faces. */
                    int span = sy - prev_sy[X] - 1;
                    int src_py = (int)(wy - 1.0f + (float)grid->fine_y);
                    /* Never source wall texture from above the world's
                     * first row -- the tile rows under the HUD band hold
                     * whatever the map wraps to there (HUD tiles). */
                    if (src_py < world_top) src_py = world_top;
                    int tile_top = src_py & ~7;
                    /* Foliage faces stretch their art once down the wall;
                     * tiling it made tall trees a busy stack of repeats.
                     * Masonry and rock keep the tiling -- stacked courses
                     * are exactly what a cliff face should look like. */
                    bool leafy_wall =
                        grid->leafy[tile_top >> 3][tex_col >> 3] != 0;
                    for (int fy = prev_sy[X] + 1; fy < sy; fy++) {
                        if (fy < world_top * S || fy >= OH) continue;
                        int d = fy - prev_sy[X];
                        int art_row;
                        if (leafy_wall) {
                            art_row = (span > 0) ? ((d - 1) * 8 / (span + 1)) : 0;
                            if (art_row > 7) art_row = 7;
                        } else {
                            art_row = (d / S) & 7;
                        }
                        uint32_t wall = grid->tex[(tile_top + art_row)
                                                      * VOX_TEX_W + tex_col];
                        int t = d * 52 / (span + 1);
                        out[fy * OW + X] = shade(wall, 186 - t);
                    }
                }

                if (sy >= world_top * S && sy < OH) {
                    /* Slight top-light on raised ground helps height pop. */
                    out[sy * OW + X] = (h > 0.0f) ? shade(tex, 272) : tex;
                }
                prev_sy[X] = sy;
            }
        }

        /* Sprites standing on this world row (once per GB row). */
        if (J % S != 0) continue;
        const int wy_i = J / S;
        for (int i = 0; i < sprites->count; i++) {
            const VoxSprite* s = &sprites->entries[i];
            int sh = s->tall ? 16 : 8;
            int feet = s->y + sh;          /* sprites stand on their bottom edge */

            /* Link mid-jump: the game draws his sprite higher, but his
             * shadow -- and therefore his billboard's anchor -- stays on
             * the ground he jumped from. Re-anchor his OAM sprites to the
             * ground row and lift the body instead. */
            int air = 0;
            if (grid->link_known && grid->link_jump > 0 &&
                s->x + 4 >= grid->link_sx - 10 && s->x + 4 <= grid->link_sx + 10 &&
                feet >= grid->link_feet_sy - grid->link_jump - 4 &&
                feet <= grid->link_feet_sy - grid->link_jump + 4) {
                air = grid->link_jump;
                feet = grid->link_feet_sy;
            }
            if (feet != wy_i) continue;
            if (feet < world_top) continue;   /* lives in the HUD band */

            int fy = feet >= GB_SCREEN_HEIGHT ? GB_SCREEN_HEIGHT - 1 : feet;
            /* Characters are drawn as runs of adjacent 8px OAM sprites.
             * Anchoring each half to the ground under its own feet split
             * them apart whenever a character straddled a height boundary,
             * so find the horizontal run this sprite belongs to and anchor
             * the whole run at its shared centre. */
            int run_l = s->x, run_r = s->x;
            for (bool moved = true; moved;) {
                moved = false;
                for (int j = 0; j < sprites->count; j++) {
                    const VoxSprite* o = &sprites->entries[j];
                    if (o->y != s->y) continue;
                    if (o->x == run_l - 8) { run_l = o->x; moved = true; }
                    if (o->x == run_r + 8) { run_r = o->x; moved = true; }
                }
            }
            float anchor_x = (float)(run_l + run_r + 8) * 0.5f;
            float ground = height_at(grid, anchor_x, (float)fy);
            if (ground < 0.0f) ground = 0.0f;   /* stand on the water surface */
            /* Link rides the eased ground instead of the raw cell sample,
             * so stepping across a height boundary ramps instead of pops. */
            if (link_ease_live &&
                s->x + 4 >= grid->link_sx - 10 && s->x + 4 <= grid->link_sx + 10 &&
                feet >= grid->link_feet_sy - 6 && feet <= grid->link_feet_sy + 6) {
                ground = link_ground_ease;
            }
            int base = (int)((y_off + (float)(fy - world_top) * cam.squash
                              - ground * lift) * fS + 0.5f);

            /* Airborne pixels rise like terrain does: 16 world px of jump
             * equals one HIGH block of extrusion. */
            int body_lift = (int)(((float)air * (VOX_UNITS[VOX_H_HIGH] / 16.0f)
                                   * lift + (float)air) * fS + 0.5f);

            for (int row = 0; row < sh; row++) {
                uint32_t px[8];
                vox_decode_sprite_row(ctx, s, row, px);
                int sy0 = base - body_lift - (sh - row) * S;
                for (int sub = 0; sub < S; sub++) {
                    int SY = sy0 + sub;
                    if (SY < world_top * S || SY >= OH) continue;
                    for (int c = 0; c < 8; c++) {
                        if (!px[c]) continue;
                        int sx = s->x + c;
                        if (sx < 0 || sx >= GB_SCREEN_WIDTH) continue;
                        uint32_t* dst = &out[SY * OW + sx * S];
                        for (int k = 0; k < S; k++) dst[k] = px[c];
                    }
                }
            }

            /* A soft contact shadow sells the billboard standing on the
             * ground instead of floating over it. */
            for (int sub = 0; sub < S; sub++) {
                int SY = base + sub;
                if (SY < world_top * S || SY >= OH) continue;
                for (int c = 1; c < 7; c++) {
                    int sx = s->x + c;
                    if (sx < 0 || sx >= GB_SCREEN_WIDTH) continue;
                    uint32_t* p = &out[SY * OW + sx * S];
                    for (int k = 0; k < S; k++) p[k] = shade(p[k], 170);
                }
            }
        }
    }

compose_overlays:
    /* A dialog box floats flat over the frozen diorama, exactly as the
     * game drew it -- the world stays voxel, the words stay words. */
    if (grid->text_overlay && grid->box_w > 0) {
        /* In chase cam the box's 2D screen position means nothing (it was
         * landing in the sky over a 3D view), so pin it to the bottom of
         * the screen where a dialog belongs. The diorama modes keep it
         * exactly where the game drew it. */
        int dy = 0;
        if (mode == VOXEL_MODE_CHASE) {
            dy = (GB_SCREEN_HEIGHT - 4) - (grid->box_y + grid->box_h);
        }
        for (int y = grid->box_y; y < grid->box_y + grid->box_h; y++) {
            const uint32_t* src = fb + y * GB_SCREEN_WIDTH;
            int oy = y + dy;
            if (oy < 0 || oy >= GB_SCREEN_HEIGHT) continue;
            for (int sub = 0; sub < S; sub++) {
                uint32_t* dst = &out[(oy * S + sub) * OW];
                for (int x = grid->box_x; x < grid->box_x + grid->box_w; x++) {
                    for (int k = 0; k < S; k++) dst[x * S + k] = src[x];
                }
            }
        }
    }

    /* The status bar comes back exactly as the game drew it. */
    for (int Y = 0; Y < world_top * S; Y++) {
        const uint32_t* src = fb + (Y / S) * GB_SCREEN_WIDTH;
        for (int X = 0; X < OW; X++) out[Y * OW + X] = src[X / S];
    }
}

/* ------------------------------------------------------------------ */
/* frame hook                                                          */
/* ------------------------------------------------------------------ */

static void poll_toggle_key(void) {
#ifdef GB_HAS_SDL2
    /* The platform pumps SDL events every frame, so keyboard state is
     * already current by the time the frame hook runs. Edge-detect F3. */
    static bool was_down = false;
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    bool down = keys && keys[SDL_SCANCODE_F3];
    if (down && !was_down) {
        voxel_set_mode((g_mode + 1) % VOXEL_MODE_COUNT);
    }
    was_down = down;
#endif
}

/* Returning NULL leaves the guest frame completely untouched, which is what
 * happens whenever voxel mode is off -- the default -- and whenever the
 * cart is showing something that should stay flat (menus, dialog,
 * cutscenes). The game plays exactly as it always did unless you ask for
 * the diorama. */
static const uint32_t* voxel_frame_hook(GBContext* ctx, const uint32_t* fb,
                                        int* out_w, int* out_h) {
    poll_toggle_key();
    if (g_mode == VOXEL_MODE_OFF || !ctx || !fb) return NULL;
    if (!vox_scrape(ctx, fb, &g_grid, &g_sprites)) return NULL;
    if (g_grid.flat) return NULL;
    vox_render(ctx, &g_grid, &g_sprites, fb, g_mode, g_scale, g_out);
    *out_w = GB_SCREEN_WIDTH * g_scale;
    *out_h = GB_SCREEN_HEIGHT * g_scale;
    return g_out;
}

void voxel_install(void) {
    const char* s = getenv("VOXEL_SCALE");
    if (s) {
        int v = atoi(s);
        if (v >= 1 && v <= VOX_MAX_SCALE) g_scale = v;
    }
    gb_platform_set_frame_hook_scaled(voxel_frame_hook);
    fprintf(stderr,
            "[VOXEL] available at %dx internal scale "
            "(off by default; F3 cycles OFF/15/30/45)\n", g_scale);
}
