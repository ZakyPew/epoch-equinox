/* Renderer smoke test that needs no cartridge.
 *
 * Every other probe in tools/ boots a ROM, which no CI machine has and
 * no repository may carry. This one builds a VoxTileGrid by hand -- a
 * meadow with a lake under a spring sky, a blue-stone crypt, a red-brick
 * forge -- and pushes it through vox_render exactly as the live frame
 * hook would, in the chase camera and the 45-degree diorama. It writes
 * the frames as PPMs for eyes, and checks the atmosphere with numbers:
 *
 *   - ground mist changes the far water and clears with the slider off;
 *   - indoors, distance fog takes the ROOM's colour, so the crypt hazes
 *     blue and the forge hazes red -- a fixed grey would fail both.
 *
 * Build next to the other probes, from build/:
 *   cc -O2 -o vox_synth ../tools/vox_synth.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 *   ./vox_synth <out_prefix>
 *
 * Exit status is the check count that failed; CI runs it. */
#include "gbrt.h"
#include "voxel/voxel.h"
#include "voxel/voxel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S 2
#define OW (GB_SCREEN_WIDTH * S)
#define OH (GB_SCREEN_HEIGHT * S)

static int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

static void write_ppm(const char* path, const uint32_t* fb, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t c = fb[i];
        uint8_t px[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "wrote %s\n", path);
}

/* Paint one 8x8 tile of the ground texture in a base colour with a
 * faint checker, the way real tile art has texture without being noise. */
static void paint_tile(VoxTileGrid* g, int tx, int ty, uint32_t base) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int k = ((x >> 1) + (y >> 1)) & 1;
            uint32_t c = base;
            if (k) {
                uint32_t r = ((c >> 16) & 0xFF) * 15 / 16;
                uint32_t gg = ((c >> 8) & 0xFF) * 15 / 16;
                uint32_t b = (c & 0xFF) * 15 / 16;
                c = 0xFF000000u | (r << 16) | (gg << 8) | b;
            }
            g->tex[(ty * 8 + y) * VOX_TEX_W + tx * 8 + x] = c;
        }
    }
}

typedef struct {
    const char* name;
    int sky;
    uint32_t floor, low, wall, water;
} Scene;

/* One layout for every scene, so only the palette and sky change: a
 * ridge across the far rows, a lake in the middle distance, tufts on
 * the near ground, and Link in the foreground looking north over it. */
static void build(VoxTileGrid* g, const Scene* sc) {
    memset(g, 0, sizeof(*g));
    g->sky = sc->sky;
    g->hud_rows = 16;
    g->link_known = true;
    g->link_sx = 80;
    g->link_feet_sy = 124;
    g->link_dir = 0;
    for (int ty = 0; ty < VOX_TILES_H; ty++) {
        for (int tx = 0; tx < VOX_TILES_W; tx++) {
            int cls = VOX_H_FLOOR;
            uint32_t col = sc->floor;
            if (ty >= 2 && ty <= 4) { cls = VOX_H_HIGH; col = sc->wall; }
            else if (ty >= 6 && ty <= 9 && tx >= 3 && tx <= 14) {
                cls = VOX_H_WATER; col = sc->water;
            } else if (ty >= 11 && ((tx * 7 + ty * 3) % 5) == 0) {
                cls = VOX_H_LOW; col = sc->low;
                g->leafy[ty][tx] = 1;
            }
            if (tx == 0 || tx == VOX_TILES_W - 1) { cls = VOX_H_HIGH; col = sc->wall; }
            g->height[ty][tx] = (uint8_t)cls;
            paint_tile(g, tx, ty, col);
        }
    }
}

static void render(const VoxTileGrid* g, int mode, uint32_t* out) {
    static VoxSpriteList sprites;
    static uint32_t fb[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];
    for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++)
        fb[i] = 0xFF202020u;
    sprites.count = 0;
    /* The chase camera eases into place over many frames and keeps its
     * pose between calls: run it well past settling so two renders of
     * the same scene differ only in what the tuning changed. */
    for (int i = 0; i < 60; i++) vox_render(NULL, g, &sprites, fb, mode, S, out);
}

/* Mean colour over a screen rectangle (fractions of the frame). */
static void mean_rgb(const uint32_t* out, float x0, float x1, float y0,
                     float y1, float rgb[3]) {
    double acc[3] = {0, 0, 0};
    int n = 0;
    for (int y = (int)(y0 * OH); y < (int)(y1 * OH); y++) {
        for (int x = (int)(x0 * OW); x < (int)(x1 * OW); x++) {
            uint32_t c = out[y * OW + x];
            acc[0] += (c >> 16) & 0xFF;
            acc[1] += (c >> 8) & 0xFF;
            acc[2] += c & 0xFF;
            n++;
        }
    }
    for (int i = 0; i < 3; i++) rgb[i] = n ? (float)(acc[i] / n) : 0.0f;
}

static int differing(const uint32_t* a, const uint32_t* b) {
    int n = 0;
    for (int i = 0; i < OW * OH; i++) if (a[i] != b[i]) n++;
    return n;
}

int main(int argc, char** argv) {
    const char* prefix = argc > 1 ? argv[1] : "synth";
    static VoxTileGrid grid;
    static uint32_t a[OW * OH], b[OW * OH];
    char path[256];
    VoxelTuning* t = voxel_tuning();

    const Scene meadow = { "meadow", VOX_SKY_SPRING,
                           0xFF6FA84Cu, 0xFF8CC85Au, 0xFF7A6A52u, 0xFF3C6CC0u };
    const Scene crypt  = { "crypt", VOX_SKY_NONE,
                           0xFF3C4A6Cu, 0xFF48587Cu, 0xFF2A3450u, 0xFF203C70u };
    const Scene forge  = { "forge", VOX_SKY_NONE,
                           0xFF7C3A2Cu, 0xFF8C4A34u, 0xFF5A2418u, 0xFFA04020u };

    /* ---- ground mist, chase camera ------------------------------------ */
    build(&grid, &meadow);
    t->mist = 0.0f;
    render(&grid, VOXEL_MODE_CHASE, a);
    t->mist = 1.0f;
    render(&grid, VOXEL_MODE_CHASE, b);
    snprintf(path, sizeof(path), "%s-meadow-chase-nomist.ppm", prefix);
    write_ppm(path, a, OW, OH);
    snprintf(path, sizeof(path), "%s-meadow-chase.ppm", prefix);
    write_ppm(path, b, OW, OH);
    {
        int d = differing(a, b);
        CHECK(d > OW * OH / 50, "mist changes the chase frame");
        /* The lake sits in the middle distance, just past Link. Mist is
         * a pale sky tone, so it must lift the water's luminance. */
        float m0[3], m1[3];
        mean_rgb(a, 0.35f, 0.65f, 0.50f, 0.58f, m0);
        mean_rgb(b, 0.35f, 0.65f, 0.50f, 0.58f, m1);
        float l0 = m0[0] * 0.3f + m0[1] * 0.59f + m0[2] * 0.11f;
        float l1 = m1[0] * 0.3f + m1[1] * 0.59f + m1[2] * 0.11f;
        printf("     lake luminance: %.1f -> %.1f with mist\n", l0, l1);
        CHECK(l1 > l0 + 4.0f, "mist lightens the lake");
        /* And Link's own ground, in the foreground, stays as it was. */
        float n0[3], n1[3];
        mean_rgb(a, 0.30f, 0.70f, 0.78f, 0.95f, n0);
        mean_rgb(b, 0.30f, 0.70f, 0.78f, 0.95f, n1);
        float dn = 0.0f;
        for (int i = 0; i < 3; i++) {
            float x = n1[i] - n0[i];
            dn += x < 0 ? -x : x;
        }
        printf("     foreground drift: %.2f\n", dn);
        CHECK(dn < 1.0f, "the ground under Link stays crisp");
    }

    /* ---- ground mist, diorama ----------------------------------------- */
    t->mist = 0.0f;
    render(&grid, VOXEL_MODE_45, a);
    t->mist = 1.0f;
    render(&grid, VOXEL_MODE_45, b);
    snprintf(path, sizeof(path), "%s-meadow-45.ppm", prefix);
    write_ppm(path, b, OW, OH);
    CHECK(differing(a, b) > OW * OH / 100, "mist changes the diorama frame");
    {
        float m0[3], m1[3];
        mean_rgb(a, 0.20f, 0.75f, 0.51f, 0.60f, m0);
        mean_rgb(b, 0.20f, 0.75f, 0.51f, 0.60f, m1);
        float l0 = m0[0] * 0.3f + m0[1] * 0.59f + m0[2] * 0.11f;
        float l1 = m1[0] * 0.3f + m1[1] * 0.59f + m1[2] * 0.11f;
        printf("     diorama lake luminance: %.1f -> %.1f with mist\n", l0, l1);
        CHECK(l1 > l0 + 4.0f, "mist lightens the diorama's lake");
    }

    /* ---- environment-coloured fog indoors ------------------------------ */
    t->mist = 0.45f;
    build(&grid, &crypt);
    render(&grid, VOXEL_MODE_CHASE, a);
    snprintf(path, sizeof(path), "%s-crypt-chase.ppm", prefix);
    write_ppm(path, a, OW, OH);
    build(&grid, &forge);
    render(&grid, VOXEL_MODE_CHASE, b);
    snprintf(path, sizeof(path), "%s-forge-chase.ppm", prefix);
    write_ppm(path, b, OW, OH);
    {
        /* The far ridge, deep in the fog, and the backdrop above it. */
        float c[3], f[3], cb[3], fbk[3];
        mean_rgb(a, 0.30f, 0.70f, 0.30f, 0.36f, c);
        mean_rgb(b, 0.30f, 0.70f, 0.30f, 0.36f, f);
        mean_rgb(a, 0.30f, 0.70f, 0.05f, 0.15f, cb);
        mean_rgb(b, 0.30f, 0.70f, 0.05f, 0.15f, fbk);
        printf("     crypt far: %.0f %.0f %.0f  forge far: %.0f %.0f %.0f\n",
               c[0], c[1], c[2], f[0], f[1], f[2]);
        printf("     crypt backdrop: %.0f %.0f %.0f  forge backdrop: %.0f %.0f %.0f\n",
               cb[0], cb[1], cb[2], fbk[0], fbk[1], fbk[2]);
        CHECK(c[2] > c[0] + 6.0f, "the crypt's far fog is bluer than red");
        CHECK(f[0] > f[2] + 6.0f, "the forge's far fog is redder than blue");
        CHECK(cb[2] > cb[0] && fbk[0] > fbk[2],
              "and the dark backdrop wears the same tint");
    }
    snprintf(path, sizeof(path), "%s-crypt-45.ppm", prefix);
    build(&grid, &crypt);
    render(&grid, VOXEL_MODE_45, a);
    write_ppm(path, a, OW, OH);

    if (failures) { printf("%d check(s) FAILED\n", failures); return failures; }
    printf("all checks passed\n");
    return 0;
}
