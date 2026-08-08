/* Diorama renderer + frame hook for the voxel mode.
 *
 * The scheme is the classic "voxel space" column march, adapted to a
 * screen-sized diorama: for each screen column, walk the visible map from
 * far (top) to near (bottom), project each cell's top surface under the
 * tilted camera, and fill the gap a height drop opens with that cell's
 * front wall. Painter's order per column — near rows overdraw far rows —
 * so no depth buffer is needed.
 *
 * The terrain texture is the game's own composed background frame, which
 * keeps every palette, season tint and animation exactly as the cart drew
 * it. Sprites are re-decoded from OAM/VRAM and stood upright as billboards
 * so characters rise out of the ground instead of lying flat on it.
 */
#include "voxel.h"
#include "voxel_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "platform_sdl.h"

#ifdef GB_HAS_SDL2
#include <SDL.h>
#endif

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static int g_mode = VOXEL_MODE_OFF;
static uint32_t g_out[GB_FRAMEBUFFER_SIZE];
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
        static const char* names[VOXEL_MODE_COUNT] = {"OFF", "15", "30", "45"};
        fprintf(stderr, "[VOXEL] mode: %s\n", names[mode]);
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint32_t shade(uint32_t c, int mul /* 0..256 */) {
    uint32_t r = ((c & 0x0000FF) * (uint32_t)mul) >> 8;
    uint32_t g = (((c >> 8) & 0xFF) * (uint32_t)mul) >> 8;
    uint32_t b = (((c >> 16) & 0xFF) * (uint32_t)mul) >> 8;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* Height in extrusion units at a world (screen-space) pixel. */
static inline float height_at(const VoxTileGrid* grid, int x, int y) {
    int tx = (x + grid->fine_x) >> 3;
    int ty = (y + grid->fine_y) >> 3;
    if (tx < 0) tx = 0;
    if (tx >= VOX_TILES_W) tx = VOX_TILES_W - 1;
    if (ty < 0) ty = 0;
    if (ty >= VOX_TILES_H) ty = VOX_TILES_H - 1;
    return VOX_UNITS[grid->height[ty][tx]];
}

/* ------------------------------------------------------------------ */
/* the diorama                                                         */
/* ------------------------------------------------------------------ */

void vox_render(GBContext* ctx, const VoxTileGrid* grid,
                const VoxSpriteList* sprites, const uint32_t* fb,
                int mode, uint32_t* out) {
    const VoxCam cam = VOX_CAMS[mode];

    /* A full-screen menu owns the display: pass the frame through flat
     * rather than extruding an inventory screen. The mode stays armed, so
     * closing the menu drops straight back into the diorama. */
    if (grid->flat) {
        memcpy(out, fb, GB_FRAMEBUFFER_SIZE * sizeof(uint32_t));
        return;
    }

    /* The status bar is a band at the top; the world is everything under it
     * and stays flat inside its own band at the end. */
    const int world_top = grid->hud_rows;
    const int world_h = GB_SCREEN_HEIGHT - world_top;
    if (world_h <= 0) {
        memcpy(out, fb, GB_FRAMEBUFFER_SIZE * sizeof(uint32_t));
        return;
    }

    /* Squashing the world frees vertical room; spend it lifting the diorama
     * so tall geometry has somewhere to go without clipping off the top. */
    const float headroom = VOX_UNITS[VOX_H_HIGH] * cam.lift;
    float y_off = (float)world_top + ((float)world_h * (1.0f - cam.squash)) * 0.5f;
    y_off += headroom * 0.55f;

    /* Backdrop: a quiet vertical fade so the diorama reads as a model on a
     * table rather than a screen with holes in it. */
    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        int l = 26 + y * 20 / GB_SCREEN_HEIGHT;
        uint32_t c = 0xFF000000u | ((uint32_t)(l + 6) << 16) | ((uint32_t)l << 8)
                     | (uint32_t)(l - 8 > 0 ? l - 8 : 0);
        for (int x = 0; x < GB_SCREEN_WIDTH; x++) out[y * GB_SCREEN_WIDTH + x] = c;
    }

    for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
        int prev_sy = -1;
        uint32_t prev_tex = 0;
        for (int wy = world_top; wy < GB_SCREEN_HEIGHT; wy++) {
            float h = height_at(grid, x, wy);
            int sy = (int)(y_off + (float)(wy - world_top) * cam.squash
                           - h * cam.lift + 0.5f);
            uint32_t tex = fb[wy * GB_SCREEN_WIDTH + x];

            if (h < 0.0f) {
                /* Water: tint toward deep blue so sunk cells read as
                 * liquid rather than as a hole in the table. */
                tex = shade(tex, 190);
                tex = (tex & 0xFF00FFFFu) | 0x00500000u;
            }

            if (prev_sy >= 0 && sy > prev_sy + 1) {
                /* Height dropped toward the viewer: the far cell's front
                 * wall is exposed. Shade it darker with a little vertical
                 * falloff so tall faces read as faces. */
                int span = sy - prev_sy - 1;
                for (int fy = prev_sy + 1; fy < sy; fy++) {
                    if (fy < world_top || fy >= GB_SCREEN_HEIGHT) continue;
                    int t = (fy - prev_sy) * 52 / (span + 1);
                    out[fy * GB_SCREEN_WIDTH + x] = shade(prev_tex, 198 - t);
                }
            }

            if (sy >= world_top && sy < GB_SCREEN_HEIGHT) {
                /* Slight top-light on raised ground helps height pop. */
                out[sy * GB_SCREEN_WIDTH + x] = (h > 0.0f) ? shade(tex, 272) : tex;
            }
            prev_sy = sy;
            prev_tex = tex;
        }
    }

    /* Sprites as upright billboards, far-to-near so overlaps sort. */
    for (int pass_y = world_top; pass_y < GB_SCREEN_HEIGHT + 16; pass_y++) {
        for (int i = 0; i < sprites->count; i++) {
            const VoxSprite* s = &sprites->entries[i];
            int sh = s->tall ? 16 : 8;
            int feet = s->y + sh;          /* sprites stand on their bottom edge */

            /* Link mid-jump: the game draws his sprite higher, but his
             * shadow -- and therefore his billboard's anchor -- stays on
             * the ground he jumped from. w1Link gives both. Re-anchor any
             * of his OAM sprites to the ground row and lift the body
             * instead, so a jump reads as height above the terrain rather
             * than a slide toward the horizon. */
            int air = 0;
            if (grid->link_known && grid->link_jump > 0 &&
                s->x + 4 >= grid->link_sx - 10 && s->x + 4 <= grid->link_sx + 10 &&
                feet >= grid->link_feet_sy - grid->link_jump - 4 &&
                feet <= grid->link_feet_sy - grid->link_jump + 4) {
                air = grid->link_jump;
                feet = grid->link_feet_sy;
            }
            if (feet != pass_y) continue;
            if (feet < world_top) continue;   /* lives in the HUD band */

            int fy = feet >= GB_SCREEN_HEIGHT ? GB_SCREEN_HEIGHT - 1 : feet;
            float ground = height_at(grid, s->x + 4, fy);
            if (ground < 0.0f) ground = 0.0f;   /* stand on the water surface */
            int base = (int)(y_off + (float)(fy - world_top) * cam.squash
                             - ground * cam.lift + 0.5f);

            /* Airborne pixels rise like terrain does: 16 world px of jump
             * equals one HIGH block of extrusion. */
            int body_lift = (int)((float)air * (VOX_UNITS[VOX_H_HIGH] / 16.0f)
                                  * cam.lift + 0.5f) + air;

            for (int row = 0; row < sh; row++) {
                uint32_t px[8];
                vox_decode_sprite_row(ctx, s, row, px);
                int sy = base - body_lift - (sh - row);
                if (sy < world_top || sy >= GB_SCREEN_HEIGHT) continue;
                for (int c = 0; c < 8; c++) {
                    int sx = s->x + c;
                    if (!px[c] || sx < 0 || sx >= GB_SCREEN_WIDTH) continue;
                    out[sy * GB_SCREEN_WIDTH + sx] = px[c];
                }
            }

            /* A soft contact shadow sells the billboard standing on the
             * ground instead of floating over it. */
            if (base >= world_top && base < GB_SCREEN_HEIGHT) {
                for (int c = 1; c < 7; c++) {
                    int sx = s->x + c;
                    if (sx < 0 || sx >= GB_SCREEN_WIDTH) continue;
                    uint32_t* p = &out[base * GB_SCREEN_WIDTH + sx];
                    *p = shade(*p, 170);
                }
            }
        }
    }

    /* The status bar comes back exactly as the game drew it. */
    if (world_top > 0) {
        memcpy(out, fb, (size_t)world_top * GB_SCREEN_WIDTH * sizeof(uint32_t));
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
 * happens whenever voxel mode is off -- the default. The game plays exactly
 * as it always did unless you ask for the diorama. */
static const uint32_t* voxel_frame_hook(GBContext* ctx, const uint32_t* fb) {
    poll_toggle_key();
    if (g_mode == VOXEL_MODE_OFF || !ctx || !fb) return NULL;
    if (!vox_scrape(ctx, &g_grid, &g_sprites)) return NULL;
    vox_render(ctx, &g_grid, &g_sprites, fb, g_mode, g_out);
    return g_out;
}

void voxel_install(void) {
    gb_platform_set_frame_hook(voxel_frame_hook);
    fprintf(stderr, "[VOXEL] available (off by default; F3 cycles OFF/15/30/45)\n");
}
