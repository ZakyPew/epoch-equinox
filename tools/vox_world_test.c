/* Exercises the persistent world (voxel_world.c) without a cart: builds
 * two synthetic rooms that sit north/south of each other on the overworld
 * grid, remembers both, then checks that the samplers and the chase
 * renderer see the northern room across the border of the southern one.
 *
 * Build like vox_shot (it links the same libraries):
 *
 *   cc -O2 -o vox_world_test ../tools/vox_world_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 *
 * Writes world-cached.ppm / world-lost.ppm beside the binary for
 * eyeballing; exits nonzero if any check fails.
 */
#include "voxel/voxel.h"
#include "voxel/voxel_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HUD 16
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

/* Solid-colour room with a given ground texel; heights all floor. */
static void make_room(VoxTileGrid* g, uint32_t ground) {
    memset(g, 0, sizeof(*g));
    g->hud_rows = HUD;
    g->sky = VOX_SKY_NONE;
    for (int i = 0; i < VOX_TEX_H * VOX_TEX_W; i++) g->tex[i] = ground;
    for (int ty = 0; ty < VOX_TILES_H; ty++)
        for (int tx = 0; tx < VOX_TILES_W; tx++)
            g->height[ty][tx] = VOX_H_FLOOR;
}

static void make_state(VoxOracleState* st, int room) {
    memset(st, 0, sizeof(*st));
    st->valid = true;
    st->profile_matched = true;
    st->scroll_mode = 1;
    st->active_group = 0;
    st->active_room = room;
    st->is_seasons = false;
}

static void write_ppm(const char* path, const uint32_t* fb, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t c = fb[i];
        uint8_t px[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(void) {
    int failures = 0;
    static VoxTileGrid room_north, room_south;
    VoxOracleState st;

    const uint32_t RED = 0xFFC04030u, BLUE = 0xFF3040C0u;

    /* Room 0x6A: red ground, a wall band across room-tile rows 6-9
     * (room px 48-79), and one billboard tree at room cell (5, 2). */
    make_room(&room_north, RED);
    for (int rty = 6; rty <= 9; rty++) {
        int gty = rty + HUD / 8;          /* fine offsets are zero */
        for (int tx = 0; tx < VOX_TILES_W; tx++)
            room_north.height[gty][tx] = VOX_H_HIGH;
    }
    {
        int cell_col = 5, cell_row = 2;   /* 16px room cells */
        int sx = cell_col * 16, sy = cell_row * 16 + HUD;
        VoxTree* t = &room_north.trees[0];
        t->sx = sx;
        t->sy = sy;
        t->hcls = VOX_H_HIGH;
        t->lit = t->mid = t->dark = t->bark = RED;
        for (int i = 0; i < 16 * 16; i++) t->tex[i] = RED;
        room_north.tree_count = 1;
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
                room_north.treecell[(sy + dy * 8) / 8][(sx + dx * 8) / 8] = 1;
    }

    /* Room 0x7A: flat blue, Link standing near its northern border. */
    make_room(&room_south, BLUE);
    room_south.link_known = true;
    room_south.link_sx = 80;
    room_south.link_feet_sy = 30;

    /* Visit north, then walk south: the anchor ends on 0x7A. */
    make_state(&st, 0x6A);
    vox_world_remember(&st, &room_north);
    make_state(&st, 0x7A);
    vox_world_remember(&st, &room_south);

    float h;
    uint32_t c;
    bool leafy = true;

    CHECK(vox_world_height(80.0f, 60.0f, &h) && h == 0.0f,
          "current room samples flat");
    /* Screen y -48 is room-north px row 80... band is rows 48-79, so aim
     * at -60: ry = -76, northern ly = 52. */
    CHECK(vox_world_height(80.0f, -60.0f, &h) && h > 4.0f,
          "north neighbour's wall band has height");
    CHECK(vox_world_tex(80.0f, -60.0f, &c) && c == RED,
          "north neighbour wears its own texture");
    CHECK(vox_world_face(80.0f, -60.0f, 3, &c, &leafy) && c == RED && !leafy,
          "face art comes from the neighbour, not leafy");
    CHECK(!vox_world_height(200.0f, 60.0f, &h),
          "unvisited east neighbour stays unknown");
    /* The tree cell reads as open ground in chase semantics. */
    CHECK(vox_world_height(85.0f, 40.0f + HUD - 128.0f, &h) && h == 0.0f,
          "north neighbour's tree cell is flattened");

    VoxWorldTree wt[8];
    int n = vox_world_neighbor_trees(wt, 8);
    CHECK(n == 1 && wt[0].sx == 80 && wt[0].sy == 32 + HUD - 128,
          "neighbour trees arrive with screen offsets");

    /* Render the chase view from the south room, facing north: the far
     * half of the picture should be the remembered red room, wall band
     * and all. Then lose the anchor and render again -- the old edge
     * fade -- and require the two frames to genuinely differ. */
    const int S = 2;
    static uint32_t fb[GB_FRAMEBUFFER_SIZE];
    static uint32_t out_cached[GB_FRAMEBUFFER_SIZE * 4];
    static uint32_t out_lost[GB_FRAMEBUFFER_SIZE * 4];
    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) fb[i] = 0xFF202020u;
    VoxSpriteList sprites;
    memset(&sprites, 0, sizeof(sprites));

    vox_render(NULL, &room_south, &sprites, fb, VOXEL_MODE_CHASE, S,
               out_cached);
    vox_world_lose();
    vox_render(NULL, &room_south, &sprites, fb, VOXEL_MODE_CHASE, S,
               out_lost);

    int diff = 0, red_hits = 0;
    const int OW = GB_SCREEN_WIDTH * S, OH = GB_SCREEN_HEIGHT * S;
    for (int i = 0; i < OW * OH; i++) {
        if (out_cached[i] != out_lost[i]) diff++;
        /* Red-dominant pixels only occur where the northern room shows. */
        uint32_t px = out_cached[i];
        int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
        if (r > g + 30 && r > b + 30) red_hits++;
    }
    printf("     frames differ in %d px; %d red px in cached view\n",
           diff, red_hits);
    CHECK(diff > 2000, "cached neighbour changes the rendered frame");
    CHECK(red_hits > 500, "northern room's texture reaches the screen");

    write_ppm("world-cached.ppm", out_cached, OW, OH);
    write_ppm("world-lost.ppm", out_lost, OW, OH);

    /* Anchor-less samplers answer unknown across the board. */
    CHECK(!vox_world_height(80.0f, -60.0f, &h) &&
          !vox_world_tex(80.0f, -60.0f, &c) &&
          vox_world_neighbor_trees(wt, 8) == 0,
          "everything is unknown after lose()");

    printf(failures ? "%d FAILURES\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
