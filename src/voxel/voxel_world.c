/* Persistent world: remember every room you visit, so the chase camera
 * can draw past the edge of the one the game keeps in memory.
 *
 * The carts hold exactly one room at a time; the 3D view used to end at
 * its border with a clamped smear fading into fog. But the overworld is a
 * 16x16 grid of single-screen rooms whose index encodes its own geography
 * -- crossing one room north is -0x10, east is +1 (verified live, 7A->6A
 * walking north in Ages) -- so a room seen once can be pinned to a world
 * position and redrawn from memory whenever it is next door to wherever
 * you stand. The world fills in as you explore.
 *
 * What is remembered per room is exactly what the renderer needs and
 * nothing it can compute: the 8px height/foliage/tree-cell grids, the
 * sprite-free ground texture, and the volumetric vegetation list. All of it in
 * room space (160x128, HUD stripped), captured only when the room is at
 * rest with the camera and screen offsets parked at zero -- which outdoors
 * is every trustworthy frame, since overworld rooms are one screen.
 *
 * Cached rooms are a memory, not a simulation: their water does not
 * ripple-animate and their NPCs are absent (sprites are live things). At
 * neighbor distance, under fog, neither reads as wrong.
 *
 * Seasons wrinkle: the season restyles the whole overworld, and a summer
 * memory next to a winter room would be a lie. Rooms remember the season
 * they were captured in and refuse to appear under a different one.
 */
#include "voxel.h"
#include "voxel_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ROOM_W  VOX_ROOM_W     /* room pixels */
#define ROOM_H  VOX_ROOM_H
#define ROOM_TW (ROOM_W / 8)   /* 8px tiles */
#define ROOM_TH (ROOM_H / 8)
#define ROOM_GRID 16           /* rooms per overworld axis */

typedef struct {
    uint8_t season;            /* wRoomStateModifier at capture (Seasons) */
    uint8_t height[ROOM_TH][ROOM_TW];
    uint8_t elevation[ROOM_TH][ROOM_TW];
    uint8_t leafy[ROOM_TH][ROOM_TW];
    uint8_t treecell[ROOM_TH][ROOM_TW];
    uint32_t tex[ROOM_H * ROOM_W];
    VoxTree trees[VOX_MAX_TREES];
    int tree_count;
} VoxRoom;

/* Groups 0-1 are the overworlds (Ages present/past, Seasons over/Subrosia).
 * Interiors are not geographic in the same way and never anchor here.
 * Rooms allocate lazily: ~150K each, and only the ones actually visited. */
static VoxRoom* g_rooms[2][ROOM_GRID * ROOM_GRID];

/* Where the current room sits, when the last trusted frame said so. While
 * false every sampler below answers "unknown" and the renderer falls back
 * to its edge fade -- exactly the old behaviour. */
static bool g_anchor = false;
static int g_group, g_room, g_hud;
static bool g_seasons;
static uint8_t g_season;

void vox_world_lose(void) { g_anchor = false; }

void vox_world_remember(const VoxOracleState* st, const VoxTileGrid* grid) {
    g_anchor = false;
    if (!st->valid || st->active_group > 1) return;
    /* At rest, an overworld room fills the screen exactly and the camera
     * has nowhere to be but zero. Anything else (screen shake, a larger
     * interior leaking through) would smear the capture: skip the frame. */
    if (st->cam_x != 0 || st->cam_y != 0) return;
    if (st->off_x != 0 || st->off_y != 0) return;
    if (GB_SCREEN_HEIGHT - grid->hud_rows != ROOM_H) return;

    int room = st->active_room & 0xFF;
    VoxRoom** slot = &g_rooms[st->active_group][room];
    if (!*slot) {
        *slot = (VoxRoom*)malloc(sizeof(VoxRoom));
        if (!*slot) return;
    }
    VoxRoom* r = *slot;
    r->season = (uint8_t)st->room_state;

    const int hud = grid->hud_rows;
    /* Tile grids, sampled at each room tile's centre so a sub-tile scroll
     * remainder (fine_x/y, normally zero at rest) lands on the right cell. */
    for (int ty = 0; ty < ROOM_TH; ty++) {
        int gy = (ty * 8 + 4 + hud + grid->fine_y) >> 3;
        if (gy >= VOX_TILES_H) gy = VOX_TILES_H - 1;
        for (int tx = 0; tx < ROOM_TW; tx++) {
            int gx = (tx * 8 + 4 + grid->fine_x) >> 3;
            if (gx >= VOX_TILES_W) gx = VOX_TILES_W - 1;
            r->height[ty][tx]   = grid->height[gy][gx];
            r->elevation[ty][tx] = grid->elevation[gy][gx];
            r->leafy[ty][tx]    = grid->leafy[gy][gx];
            r->treecell[ty][tx] = grid->treecell[gy][gx];
        }
    }
    for (int y = 0; y < ROOM_H; y++) {
        memcpy(&r->tex[y * ROOM_W],
               &grid->tex[(y + hud + grid->fine_y) * VOX_TEX_W + grid->fine_x],
               ROOM_W * sizeof(uint32_t));
    }
    /* Tree cells become volumetric objects in chase view. Replace their
     * top-down room art in the remembered ground texture with the nearest
     * real floor/grass tile; otherwise a neighbouring remembered room grows
     * the same long projected branch streaks as the live room used to. */
    for (int ty = 0; ty < ROOM_TH; ty++) {
        for (int tx = 0; tx < ROOM_TW; tx++) {
            if (!r->treecell[ty][tx]) continue;
            int bx = tx, by = ty, best = 100000;
            for (int sy = 0; sy < ROOM_TH; sy++) {
                for (int sx = 0; sx < ROOM_TW; sx++) {
                    if (r->treecell[sy][sx] || r->leafy[sy][sx] ||
                        r->height[sy][sx] < VOX_H_FLOOR ||
                        r->height[sy][sx] > VOX_H_LOW ||
                        r->elevation[sy][sx] != r->elevation[ty][tx])
                        continue;
                    int dx = sx - tx, dy = sy - ty;
                    int score = dx * dx + dy * dy;
                    if (score < best) { best = score; bx = sx; by = sy; }
                }
            }
            if (best == 100000) continue;
            for (int py = 0; py < 8; py++) {
                memcpy(&r->tex[(ty * 8 + py) * ROOM_W + tx * 8],
                       &r->tex[(by * 8 + py) * ROOM_W + bx * 8],
                       8 * sizeof(uint32_t));
            }
        }
    }
    r->tree_count = grid->tree_count;
    for (int i = 0; i < grid->tree_count; i++) {
        r->trees[i] = grid->trees[i];
        r->trees[i].sy -= hud;       /* screen -> room space */
    }

    g_group = st->active_group;
    g_room = room;
    g_seasons = st->is_seasons;
    g_season = (uint8_t)st->room_state;
    g_hud = hud;
    g_anchor = true;
}

/* The room under a screen position (which may run off any edge of the
 * screen), or NULL if unknown there. Local room-space px out through lx/ly. */
static VoxRoom* resolve(float sx, float sy, float* lx, float* ly) {
    if (!g_anchor) return NULL;
    float rx = sx;
    float ry = sy - (float)g_hud;
    int dx = (int)floorf(rx / (float)ROOM_W);
    int dy = (int)floorf(ry / (float)ROOM_H);
    int col = (g_room & 0x0F) + dx;
    int row = (g_room >> 4) + dy;
    if (col < 0 || col >= ROOM_GRID || row < 0 || row >= ROOM_GRID) return NULL;
    VoxRoom* r = g_rooms[g_group][row * ROOM_GRID + col];
    if (!r) return NULL;
    /* A memory from another season is a lie next to a live room. Only the
     * Seasons overworld restyles by season; Ages and Subrosia don't. */
    if (g_seasons && g_group == 0 && r->season != g_season) return NULL;
    if (lx) *lx = rx - (float)(dx * ROOM_W);
    if (ly) *ly = ry - (float)(dy * ROOM_H);
    return r;
}

/* One 8px cell by world tile coords (current room's tile 0,0 as origin,
 * negatives fine). Chase-camera semantics: volumetric vegetation cells read as
 * open ground, the way g_flatten_trees empties them from the live grid. */
typedef struct {
    float h;                   /* complete surface height */
    float base;                /* walkable plateau underneath the object */
    float object;              /* water/decor/solid extrusion only */
    bool leafy;
    bool ok;
} WCell;

static WCell wcell(int tx, int ty) {
    WCell c = {0.0f, 0.0f, 0.0f, false, false};
    int dx = tx >= 0 ? tx / ROOM_TW : -((-tx + ROOM_TW - 1) / ROOM_TW);
    int dy = ty >= 0 ? ty / ROOM_TH : -((-ty + ROOM_TH - 1) / ROOM_TH);
    VoxRoom* r = resolve((float)(dx * ROOM_W) + 1.0f,
                         (float)(g_hud + dy * ROOM_H) + 1.0f, NULL, NULL);
    if (!r) return c;
    int ltx = tx - dx * ROOM_TW;
    int lty = ty - dy * ROOM_TH;
    const float* units = voxel_tuning()->units;
    c.ok = true;
    uint8_t base_class = r->elevation[lty][ltx];
    c.base = base_class >= VOX_H_FLOOR && base_class <= VOX_H_HIGH
        ? units[base_class] : 0.0f;
    if (r->treecell[lty][ltx]) {
        c.h = c.base;
        return c;
    }
    c.object = units[r->height[lty][ltx]];
    c.h = c.base + c.object;
    c.leafy = r->leafy[lty][ltx] != 0;
    return c;
}

/* Height at a screen position, with the same foliage footprint taper the
 * live grid gets (height_at in voxel_render.c): raised leafy cells roll
 * off at edges that face something shorter. Neighbours in rooms nobody
 * has visited count as equal height -- no taper toward the unknown. */
static bool world_height_raw(float sx, float sy, float* out) {
    float px = sx;
    float py = sy - (float)g_hud;
    int tx = (int)floorf(px / 8.0f);
    int ty = (int)floorf(py / 8.0f);
    WCell c = wcell(tx, ty);
    if (!c.ok) return false;
    float h = c.h;
    if (c.object <= 0.0f || !c.leafy) { *out = h; return true; }

    const float EDGE = voxel_tuning()->footprint;
    if (EDGE <= 0.01f) { *out = h; return true; }
    float u = px - (float)tx * 8.0f;
    float v = py - (float)ty * 8.0f;
    float k = 1.0f;
    WCell n;
    n = wcell(tx - 1, ty);
    if (n.ok && n.h < h && u < EDGE)        k = fminf(k, u / EDGE);
    n = wcell(tx + 1, ty);
    if (n.ok && n.h < h && u > 8.0f - EDGE) k = fminf(k, (8.0f - u) / EDGE);
    n = wcell(tx, ty - 1);
    if (n.ok && n.h < h && v < EDGE)        k = fminf(k, v / EDGE);
    n = wcell(tx, ty + 1);
    if (n.ok && n.h < h && v > 8.0f - EDGE) k = fminf(k, (8.0f - v) / EDGE);
    if (k < 0.0f) k = 0.0f;
    k = k * k * (3.0f - 2.0f * k);
    *out = c.base + c.object * k;
    return true;
}

bool vox_world_height(float sx, float sy, float* out) {
    /* Same tent filter as the live grid's chase_height_at: 3x3 cross at
     * 3px spacing, weights 4/1/1/1/1. Edge taps that land in unknown
     * rooms borrow the centre, so the filter never invents a cliff at
     * the boundary of what's been explored. */
    const float o = 3.0f;
    float c;
    if (!world_height_raw(sx, sy, &c)) return false;
    float e, sum = c * 4.0f;
    sum += world_height_raw(sx - o, sy, &e) ? e : c;
    sum += world_height_raw(sx + o, sy, &e) ? e : c;
    sum += world_height_raw(sx, sy - o, &e) ? e : c;
    sum += world_height_raw(sx, sy + o, &e) ? e : c;
    *out = sum * (1.0f / 8.0f);
    return true;
}

bool vox_world_tex(float sx, float sy, uint32_t* out) {
    float lx, ly;
    VoxRoom* r = resolve(sx, sy, &lx, &ly);
    if (!r) return false;
    int x = (int)lx, y = (int)ly;
    if (x < 0) x = 0;
    if (x >= ROOM_W) x = ROOM_W - 1;
    if (y < 0) y = 0;
    if (y >= ROOM_H) y = ROOM_H - 1;
    *out = r->tex[y * ROOM_W + x];
    return true;
}

bool vox_world_face(float sx, float sy, int art_row, uint32_t* out,
                    bool* leafy) {
    float lx, ly;
    VoxRoom* r = resolve(sx, sy, &lx, &ly);
    if (!r) return false;
    int x = (int)lx, y = (int)ly;
    if (x < 0) x = 0;
    if (x >= ROOM_W) x = ROOM_W - 1;
    if (y < 0) y = 0;
    if (y >= ROOM_H) y = ROOM_H - 1;
    int tile_top = y & ~7;
    if (art_row < 0) art_row = 0;
    if (art_row > 7) art_row = 7;
    *out = r->tex[(tile_top + art_row) * ROOM_W + x];
    if (leafy) *leafy = r->leafy[tile_top >> 3][x >> 3] != 0;
    return true;
}

int vox_world_neighbor_trees(VoxWorldTree* out, int max) {
    if (!g_anchor) return 0;
    int n = 0;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            if (dx == 0 && dy == 0) continue;   /* current room is live */
            VoxRoom* r = resolve((float)(dx * ROOM_W) + 1.0f,
                                 (float)(g_hud + dy * ROOM_H) + 1.0f,
                                 NULL, NULL);
            if (!r) continue;
            for (int i = 0; i < r->tree_count && n < max; i++) {
                out[n].t = &r->trees[i];
                out[n].sx = r->trees[i].sx + dx * ROOM_W;
                out[n].sy = r->trees[i].sy + g_hud + dy * ROOM_H;
                n++;
            }
        }
    }
    return n;
}
