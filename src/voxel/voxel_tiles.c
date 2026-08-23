/* PPU scraping + terrain classification for the voxel diorama.
 *
 * Everything here reads the same VRAM/OAM/IO state the PPU renders from.
 * Nothing is game-specific by symbol — classification works off what the
 * tiles look like, so it holds up across both Oracles carts (and any other
 * top-down GBC cart, roughly).
 */
#include "voxel.h"
#include "voxel_internal.h"

#include "mod_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* mod-supplied alternate voxel art                                    */
/* ------------------------------------------------------------------ */

/* A 16x16 P6 PPM, packed to the framebuffer's 0xAARRGGBB. PPM because it
 * is the repo's native image format -- every probe tool reads and writes
 * it, and `magick tree.png tree.ppm` is the whole conversion story. */
static bool load_ppm_16(const char* path, uint32_t out[16 * 16]) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char magic[3] = {0};
    int w = 0, h = 0, maxv = 0;
    uint8_t px[16 * 16 * 3];
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) goto bad;
    {
        /* Header fields, with the #-comments editors like to leave. */
        int* fields[3] = {&w, &h, &maxv};
        for (int i = 0; i < 3; i++) {
            int ch;
            do {
                ch = fgetc(f);
                if (ch == '#') {
                    while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
                }
            } while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
            if (ch == EOF) goto bad;
            ungetc(ch, f);
            if (fscanf(f, "%d", fields[i]) != 1) goto bad;
        }
    }
    if (fgetc(f) == EOF) goto bad;   /* the single whitespace after maxval */
    if (w != 16 || h != 16 || maxv != 255) {
        fprintf(stderr,
                "[VOXEL] %s: want a 16x16 P6 with maxval 255, got %dx%d/%d\n",
                path, w, h, maxv);
        goto bad;
    }
    if (fread(px, 1, sizeof(px), f) != sizeof(px)) goto bad;
    fclose(f);
    for (int i = 0; i < 16 * 16; i++) {
        out[i] = 0xFF000000u | ((uint32_t)px[i * 3] << 16) |
                 ((uint32_t)px[i * 3 + 1] << 8) | (uint32_t)px[i * 3 + 2];
    }
    return true;
bad:
    fclose(f);
    return false;
}

/* Vegetation source art from an enabled mod: voxel/tree.ppm dresses every
 * tree, voxel/tuft.ppm every bush and grass tuft. Probed once and cached --
 * mods are fixed at launch, same as the ROM bytes they patch. The renderer
 * still turns the result into fixed geometry and derives the trunk tile from
 * the supplied source pixels. */
static const uint32_t* vox_mod_tree_art(bool is_tree) {
    static uint32_t tex[2][16 * 16];
    static int state[2] = {0, 0};   /* 0 unprobed, 1 loaded, -1 absent */
    int k = is_tree ? 0 : 1;
    if (state[k] == 0) {
        state[k] = -1;
        char path[GB_MOD_PATH_MAX];
        if (gb_mods_find_asset(is_tree ? "voxel/tree.ppm" : "voxel/tuft.ppm",
                               path, sizeof(path)) &&
            load_ppm_16(path, tex[k])) {
            state[k] = 1;
            fprintf(stderr, "[VOXEL] vegetation art from mod: %s\n", path);
        }
    }
    return state[k] == 1 ? tex[k] : NULL;
}

static int foliage_luma(uint32_t c) {
    int r = (int)((c >> 16) & 0xFFu);
    int g = (int)((c >> 8) & 0xFFu);
    int b = (int)(c & 0xFFu);
    return (r * 30 + g * 59 + b * 11) / 100;
}

/* Build the transparent-looking silhouette that lets an overhead room tile
 * become a solid pixel-art object. BG tiles have no alpha, so transparency
 * has to be recovered from context: find the nearest ordinary ground tile,
 * collect its light/mid colours, then flood only matching pixels inward from
 * the object's border. An outline that encloses the tree is therefore kept
 * even when the same dark ink also appears in the grass or snow texture.
 *
 * This is deliberately a presentation mask. The room layout has already
 * proved that the cell is vegetation; collision and gameplay never consult
 * this result. */
static bool tree_bg_match(uint32_t c, const uint32_t* colours, int count) {
    for (int i = 0; i < count; i++) {
        if (colours[i] == c) return true;
    }
    return false;
}

static void tree_relief_mask(const VoxTileGrid* grid, int tx0, int ty0,
                             VoxTree* tree) {
    uint32_t ground_colours[64];
    int ground_count = 0;
    int bx = -1, by = -1, best = 1000000;
    const int first_world_tile = grid->hud_rows >> 3;

    /* Adjacent high vegetation is a drawn forest/tree-line structure. Keep
     * each complete 16x16 source cell: the renderer lays every one onto a
     * canopy tile, and the original edge variants still join into the same
     * authored treeline. Lone trees and low tufts take the alpha-recovery
     * route below. */
    tree->joins = 0;
    if (!tree->custom_art && tree->hcls >= VOX_H_HIGH) {
        if (tx0 > 0 && grid->treecell[ty0][tx0 - 1])
            tree->joins |= VOX_TREE_JOIN_W;
        if (tx0 + 2 < VOX_TILES_W && grid->treecell[ty0][tx0 + 2])
            tree->joins |= VOX_TREE_JOIN_E;
        if (ty0 > first_world_tile && grid->treecell[ty0 - 1][tx0])
            tree->joins |= VOX_TREE_JOIN_N;
        if (ty0 + 2 < VOX_TILES_H && grid->treecell[ty0 + 2][tx0])
            tree->joins |= VOX_TREE_JOIN_S;
        if (tree->joins) {
            memset(tree->solid, 1, sizeof(tree->solid));
            return;
        }
    }

    if (!tree->custom_art) {
        for (int sy = first_world_tile; sy < VOX_TILES_H; sy++) {
            for (int sx = 0; sx < VOX_TILES_W; sx++) {
                if (grid->treecell[sy][sx] || grid->leafy[sy][sx] ||
                    grid->height[sy][sx] == VOX_H_WATER ||
                    grid->height[sy][sx] > VOX_H_LOW)
                    continue;
                int dx = sx - tx0, dy = sy - ty0;
                int score = dx * dx + dy * dy;
                if (score < best) { best = score; bx = sx; by = sy; }
            }
        }
    }

    if (bx >= 0) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint32_t c = grid->tex[(by * 8 + y) * VOX_TEX_W + bx * 8 + x];
                /* The darkest shared colour is usually outline ink. It may
                 * occur in the ground tile, but must not punch holes through
                 * the recovered object silhouette. */
                if (foliage_luma(c) < 52) continue;
                int k;
                for (k = 0; k < ground_count && ground_colours[k] != c; k++) {}
                if (k == ground_count && ground_count < 64)
                    ground_colours[ground_count++] = c;
            }
        }
    }

    /* A custom tile has no matching live ground. Treat its most common light
     * border colour as the transparent surround when one clearly exists. */
    if (ground_count == 0) {
        uint32_t border_colours[64];
        int border_counts[64];
        int n = 0;
        for (int p = 0; p < 16; p++) {
            int at[4] = {p, 15 * 16 + p, p * 16, p * 16 + 15};
            for (int q = 0; q < 4; q++) {
                uint32_t c = tree->tex[at[q]];
                if (foliage_luma(c) < 52) continue;
                int k;
                for (k = 0; k < n && border_colours[k] != c; k++) {}
                if (k == n && n < 64) {
                    border_colours[n] = c;
                    border_counts[n] = 0;
                    n++;
                }
                if (k < n) border_counts[k]++;
            }
        }
        int pick = -1;
        for (int i = 0; i < n; i++) {
            if (pick < 0 || border_counts[i] > border_counts[pick]) pick = i;
        }
        if (pick >= 0 && border_counts[pick] >= 6)
            ground_colours[ground_count++] = border_colours[pick];
    }

    uint8_t outside[16 * 16] = {0};
    int queue[16 * 16], qread = 0, qwrite = 0;
#define SEED_BG(AT)                                                        \
    do {                                                                   \
        int _at = (AT);                                                    \
        if (!outside[_at] &&                                               \
            tree_bg_match(tree->tex[_at], ground_colours, ground_count)) { \
            outside[_at] = 1;                                              \
            queue[qwrite++] = _at;                                         \
        }                                                                  \
    } while (0)
    for (int p = 0; p < 16; p++) {
        SEED_BG(p);
        SEED_BG(15 * 16 + p);
        SEED_BG(p * 16);
        SEED_BG(p * 16 + 15);
    }
#undef SEED_BG

    while (qread < qwrite) {
        int at = queue[qread++];
        int x = at & 15, y = at >> 4;
        const int nx[4] = {x - 1, x + 1, x, x};
        const int ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; k++) {
            if (nx[k] < 0 || nx[k] >= 16 || ny[k] < 0 || ny[k] >= 16)
                continue;
            int next = ny[k] * 16 + nx[k];
            if (outside[next] ||
                !tree_bg_match(tree->tex[next], ground_colours, ground_count))
                continue;
            outside[next] = 1;
            queue[qwrite++] = next;
        }
    }

    int solids = 0;
    for (int i = 0; i < 16 * 16; i++) {
        tree->solid[i] = outside[i] ? 0 : 1;
        solids += tree->solid[i] != 0;
    }
    /* A recognised room object must never vanish because its palette happens
     * to match the nearest floor. A tiny remainder means the inferred alpha
     * was not trustworthy; retain the whole original tile as a relief box. */
    if (solids < 20) memset(tree->solid, 1, sizeof(tree->solid));
}

/* Same contextual alpha recovery for a wide authored facade strip. The
 * source map has no transparency: the light ground visible around Impa's
 * roots/door is merely another palette colour. Flooding only matching
 * colours connected to the strip edge removes that surround while keeping
 * enclosed highlights on the canopy and door. */
static void structure_strip_mask(const VoxTileGrid* grid, int tx0, int ty0,
                                 const uint32_t* tex, uint8_t* solid) {
    uint32_t ground_colours[64];
    int ground_count = 0;
    int bx = -1, by = -1, best = 1000000;
    const int first_world_tile = grid->hud_rows >> 3;
    for (int sy = first_world_tile; sy < VOX_TILES_H; sy++) {
        for (int sx = 0; sx < VOX_TILES_W; sx++) {
            if (grid->structurecell[sy][sx] || grid->leafy[sy][sx] ||
                grid->height[sy][sx] == VOX_H_WATER ||
                grid->height[sy][sx] > VOX_H_LOW)
                continue;
            int dx = sx - tx0, dy = sy - ty0;
            int score = dx * dx + dy * dy;
            if (score < best) { best = score; bx = sx; by = sy; }
        }
    }
    if (bx >= 0) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint32_t c = grid->tex[(by * 8 + y) * VOX_TEX_W + bx * 8 + x];
                if (foliage_luma(c) < 52) continue;
                int k;
                for (k = 0; k < ground_count && ground_colours[k] != c; k++) {}
                if (k == ground_count && ground_count < 64)
                    ground_colours[ground_count++] = c;
            }
        }
    }

    uint8_t outside[VOX_STRUCTURE_W * VOX_STRUCTURE_H] = {0};
    int queue[VOX_STRUCTURE_W * VOX_STRUCTURE_H];
    int qread = 0, qwrite = 0;
#define SEED_STRUCTURE_BG(AT)                                             \
    do {                                                                  \
        int _at = (AT);                                                   \
        if (!outside[_at] &&                                              \
            tree_bg_match(tex[_at], ground_colours, ground_count)) {      \
            outside[_at] = 1;                                             \
            queue[qwrite++] = _at;                                        \
        }                                                                 \
    } while (0)
    for (int x = 0; x < VOX_STRUCTURE_W; x++) {
        SEED_STRUCTURE_BG(x);
        SEED_STRUCTURE_BG((VOX_STRUCTURE_H - 1) * VOX_STRUCTURE_W + x);
    }
    for (int y = 0; y < VOX_STRUCTURE_H; y++) {
        SEED_STRUCTURE_BG(y * VOX_STRUCTURE_W);
        SEED_STRUCTURE_BG(y * VOX_STRUCTURE_W + VOX_STRUCTURE_W - 1);
    }
#undef SEED_STRUCTURE_BG

    while (qread < qwrite) {
        int at = queue[qread++];
        int x = at % VOX_STRUCTURE_W, y = at / VOX_STRUCTURE_W;
        const int nx[4] = {x - 1, x + 1, x, x};
        const int ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; k++) {
            if (nx[k] < 0 || nx[k] >= VOX_STRUCTURE_W ||
                ny[k] < 0 || ny[k] >= VOX_STRUCTURE_H)
                continue;
            int next = ny[k] * VOX_STRUCTURE_W + nx[k];
            if (outside[next] ||
                !tree_bg_match(tex[next], ground_colours, ground_count))
                continue;
            outside[next] = 1;
            queue[qwrite++] = next;
        }
    }

    int solids = 0;
    for (int i = 0; i < VOX_STRUCTURE_W * VOX_STRUCTURE_H; i++) {
        solid[i] = outside[i] ? 0 : 1;
        solids += solid[i] != 0;
    }
    if (solids < 80)
        memset(solid, 1, VOX_STRUCTURE_W * VOX_STRUCTURE_H);
}

/* ------------------------------------------------------------------ */
/* raw PPU access                                                      */
/* ------------------------------------------------------------------ */

static inline uint8_t io_reg(GBContext* ctx, uint8_t reg) {
    return ctx->io[reg];
}

/* CGB BGR555 -> the runtime's framebuffer format, which is 0xAARRGGBB
 * (R in bits 16-23, B in the low byte). This module once packed it the
 * other way round -- harmless for classification, which only compared
 * its own values, but every colour it DREW (billboards, sky, tints) had
 * red and blue swapped. */
static uint32_t cgb_color(const uint8_t* pal_ram, int pal, int idx) {
    int off = pal * 8 + idx * 2;
    uint16_t raw = (uint16_t)(pal_ram[off] | (pal_ram[off + 1] << 8));
    uint32_t r = (raw & 0x1F) * 255 / 31;
    uint32_t g = ((raw >> 5) & 0x1F) * 255 / 31;
    uint32_t b = ((raw >> 10) & 0x1F) * 255 / 31;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Fetch the 16 bytes of one tile pattern from a given VRAM bank. */
static const uint8_t* tile_pattern(GBContext* ctx, uint8_t tile_index,
                                   bool signed_addressing, int bank) {
    unsigned base;
    if (signed_addressing) {
        /* LCDC.4=0: tiles at 0x9000 signed */
        base = 0x1000 + (unsigned)((int8_t)tile_index) * 16;
    } else {
        base = (unsigned)tile_index * 16;
    }
    return ctx->vram + bank * 0x2000 + base;
}

/* ------------------------------------------------------------------ */
/* classification                                                      */
/*                                                                     */
/* A tile is classified by its four CGB palette colors weighted by how  */
/* often each color index appears in the pattern. Height comes from hue */
/* family + how "busy" the pattern is (trees/walls are high-contrast    */
/* and dark-edged; paths are flat and bright).                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t rgba;
    int count;
} ColorWeight;

static void tile_histogram(const uint8_t* pat, int counts[4]) {
    counts[0] = counts[1] = counts[2] = counts[3] = 0;
    for (int row = 0; row < 8; row++) {
        uint8_t lo = pat[row * 2];
        uint8_t hi = pat[row * 2 + 1];
        for (int bit = 0; bit < 8; bit++) {
            int idx = ((lo >> (7 - bit)) & 1) | (((hi >> (7 - bit)) & 1) << 1);
            counts[idx]++;
        }
    }
}

static void rgb_of(uint32_t c, int* r, int* g, int* b) {
    *r = (int)((c >> 16) & 0xFF);
    *g = (int)((c >> 8) & 0xFF);
    *b = (int)(c & 0xFF);
}

/* Dominant-hue features over the weighted palette. */
typedef struct {
    int luma;         /* 0..255 weighted average */
    int contrast;     /* max-min luma across used colors */
    int blue_bias;    /* how strongly blue beats the others */
    int green_bias;
    int dark_frac;    /* % of pixels whose color is dark (<72 luma) */
} TileFeatures;

static TileFeatures tile_features(GBContext* ctx, const uint8_t* pat, int pal) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    int counts[4];
    tile_histogram(pat, counts);

    TileFeatures f = {0, 0, 0, 0, 0};
    int total = 64;
    long luma_sum = 0;
    int min_luma = 255, max_luma = 0;
    long blue_sum = 0, green_sum = 0, red_sum = 0;
    int dark = 0;

    for (int i = 0; i < 4; i++) {
        if (!counts[i]) continue;
        uint32_t c = cgb_color(ppu->bg_palette_ram, pal, i);
        int r, g, b;
        rgb_of(c, &r, &g, &b);
        int luma = (r * 30 + g * 59 + b * 11) / 100;
        luma_sum += (long)luma * counts[i];
        blue_sum += (long)b * counts[i];
        green_sum += (long)g * counts[i];
        red_sum += (long)r * counts[i];
        if (luma < min_luma) min_luma = luma;
        if (luma > max_luma) max_luma = luma;
        if (luma < 72) dark += counts[i];
    }

    f.luma = (int)(luma_sum / total);
    f.contrast = max_luma - min_luma;
    f.blue_bias = (int)((blue_sum - (red_sum + green_sum) / 2) / total);
    f.green_bias = (int)((green_sum - (red_sum + blue_sum) / 2) / total);
    f.dark_frac = dark * 100 / total;
    return f;
}

static uint8_t classify_tile(GBContext* ctx, const uint8_t* pat, int pal,
                             bool* leafy_out) {
    TileFeatures f = tile_features(ctx, pat, pal);
    /* "Organic" for the renderer's dome pass: green growth, and bright
     * flowering tiles (any green presence, mostly light, plenty of
     * pattern). Masonry and rock stay architectural -- they are either
     * grey (no green lean) or dominated by dark outline pixels. */
    if (leafy_out) {
        *leafy_out = f.green_bias > 12 ||
                     (f.green_bias > 0 && f.dark_frac < 45 &&
                      f.luma > 100 && f.contrast > 70);
    }

    /* Water: strongly blue, mid luma. Sinks. */
    if (f.blue_bias > 28 && f.luma < 190) return VOX_H_WATER;

    /* Dark, high-contrast tiles read as tall solids: trees, walls,
     * cliff faces, building sides. */
    if (f.dark_frac >= 45 && f.contrast > 90) return VOX_H_HIGH;
    if (f.dark_frac >= 30 && f.green_bias > 18) return VOX_H_HIGH;   /* leafy tree */

    /* Mid solids: bushes, rocks, fences — busy but not dominated by dark. */
    if (f.contrast > 120 && f.dark_frac >= 18) return VOX_H_MID;

    /* Grass and decorated ground: green-leaning and genuinely textured.
     * The bar is deliberately high -- plain grass covers most of the
     * overworld, and lifting all of it turns the diorama into a noisy
     * quilt of one-step terraces instead of readable ground. */
    if (f.green_bias > 22 && f.contrast > 76) return VOX_H_LOW;

    /* Everything else lies flat: paths, sand, floors, plain ground. */
    return VOX_H_FLOOR;
}

/* ------------------------------------------------------------------ */
/* scrape                                                              */
/* ------------------------------------------------------------------ */

/* Decode one screen scanline's worth of BG pixels for a candidate world
 * origin row and count how many match the game's own composed frame.
 * Sprites cover some pixels, so callers probe more than one line and ask
 * for a clear majority rather than perfection. */
static int score_origin(GBContext* ctx, const uint32_t* fb, unsigned map_base,
                        bool signed_tiles, uint8_t scx, int fine_x,
                        int hud_rows, int fine_y, int cand_row, int screen_y) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    int map_py = cand_row * 8 + (screen_y - hud_rows) + fine_y;
    unsigned map_y = ((unsigned)(map_py >> 3)) & 31;
    int row = map_py & 7;
    int score = 0;
    for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
        int px = x + fine_x;
        unsigned map_x = ((scx / 8) + (px >> 3)) & 31;
        unsigned off = map_base + map_y * 32 + map_x;
        uint8_t tile = ctx->vram[off];
        uint8_t attr = ctx->vram[0x2000 + off];
        const uint8_t* pat = tile_pattern(ctx, tile, signed_tiles, (attr >> 3) & 1);
        int r2 = (attr & 0x40) ? 7 - row : row;
        uint8_t lo = pat[r2 * 2];
        uint8_t hi = pat[r2 * 2 + 1];
        int bit = (attr & 0x20) ? (px & 7) : 7 - (px & 7);
        int idx = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        if (cgb_color(ppu->bg_palette_ram, attr & 7, idx) ==
            fb[screen_y * GB_SCREEN_WIDTH + x]) {
            score++;
        }
    }
    return score;
}

/* One cliff, one height.
 *
 * The game says exactly which 8x8 quadrants are solid; how TALL the resulting
 * object should look is not encoded in the top-down map. That last choice
 * comes from the tile art, and the vote initially happens per 8x8 quadrant.
 * A cliff is one object made of many tiles whose art varies along its length
 * -- lit tops, shaded faces, the odd decorated block -- so some quadrants
 * clear the "tall" threshold and their neighbours do not, and the cliff
 * comes out with a ragged top.
 *
 * So let a connected mass vote once: flood-fill each region of MID/HIGH
 * cells, count how many wanted HIGH, and give the whole region the
 * majority answer. The collision data still decides what is solid; the
 * colours just stop re-deciding the height every eight pixels. Same idea
 * as the leafy spread further down, one step earlier.
 */
void vox_unify_solid_masses(uint8_t height[VOX_TILES_H][VOX_TILES_W]) {
    bool seen[VOX_TILES_H][VOX_TILES_W];
    static int16_t stack[VOX_TILES_H * VOX_TILES_W][2];
    static int16_t cells[VOX_TILES_H * VOX_TILES_W][2];
    memset(seen, 0, sizeof(seen));

    for (int sy = 0; sy < VOX_TILES_H; sy++) {
        for (int sx = 0; sx < VOX_TILES_W; sx++) {
            if (seen[sy][sx]) continue;
            uint8_t h0 = height[sy][sx];
            if (h0 != VOX_H_MID && h0 != VOX_H_HIGH) continue;

            int top = 0, members = 0, high = 0;
            stack[top][0] = (int16_t)sx; stack[top][1] = (int16_t)sy; top++;
            seen[sy][sx] = true;
            while (top > 0) {
                top--;
                int x = stack[top][0], y = stack[top][1];
                cells[members][0] = (int16_t)x;
                cells[members][1] = (int16_t)y;
                members++;
                if (height[y][x] == VOX_H_HIGH) high++;

                static const int DX[4] = { 1, -1, 0, 0 };
                static const int DY[4] = { 0, 0, 1, -1 };
                for (int k = 0; k < 4; k++) {
                    int nx = x + DX[k], ny = y + DY[k];
                    if (nx < 0 || nx >= VOX_TILES_W ||
                        ny < 0 || ny >= VOX_TILES_H) continue;
                    if (seen[ny][nx]) continue;
                    uint8_t hn = height[ny][nx];
                    if (hn != VOX_H_MID && hn != VOX_H_HIGH) continue;
                    seen[ny][nx] = true;
                    stack[top][0] = (int16_t)nx;
                    stack[top][1] = (int16_t)ny;
                    top++;
                }
            }

            /* Majority, ties going tall: a cliff misread as a row of rocks
             * is the worse failure, and it is the one being fixed. */
            uint8_t win = (high * 2 >= members) ? VOX_H_HIGH : VOX_H_MID;
            for (int i = 0; i < members; i++)
                height[cells[i][1]][cells[i][0]] = win;
        }
    }

}

/* Find walkable shelves enclosed by real architecture. The Oracles collision
 * grid tells us where the cliff lip blocks Link, but not that the ordinary
 * floor behind the lip is raised to the lip's top. At 8px resolution those lips
 * divide the room into floor regions. The broadest region is the outdoor
 * datum; a smaller region with a substantial non-vegetation wall boundary is
 * a plateau. Its base height class comes from the bordering cliff tiles, so a
 * MID lip produces a MID shelf and a HIGH lip produces a HIGH shelf.
 *
 * Tree cells participate in the flood. A tree line can block Link without
 * changing the land height, so treating it as a region boundary creates fake
 * mesas in forest clearings. Water separates regions but never votes for a
 * plateau. */
void vox_infer_plateaus(VoxTileGrid* grid) {
    enum { MAX_CELLS = VOX_TILES_W * VOX_TILES_H };
    int16_t label[VOX_TILES_H][VOX_TILES_W];
    int16_t queue[MAX_CELLS][2];
    int ground[MAX_CELLS], mid_edges[MAX_CELLS], high_edges[MAX_CELLS];
    memset(label, 0xFF, sizeof(label));
    memset(ground, 0, sizeof(ground));
    memset(mid_edges, 0, sizeof(mid_edges));
    memset(high_edges, 0, sizeof(high_edges));
    memset(grid->elevation, 0, sizeof(grid->elevation));

    int first_y = (grid->hud_rows + grid->fine_y) >> 3;
    int room_cols = (VOX_ROOM_W + grid->fine_x + 7) >> 3;
    int room_rows = (VOX_ROOM_H + grid->fine_y + 7) >> 3;
    int last_x = room_cols < VOX_TILES_W ? room_cols : VOX_TILES_W;
    int last_y = first_y + room_rows;
    if (last_y > VOX_TILES_H) last_y = VOX_TILES_H;

#define PLATEAU_OPEN(Y, X)                                                \
    (grid->treecell[(Y)][(X)] ||                                         \
     grid->height[(Y)][(X)] == VOX_H_FLOOR ||                            \
     grid->height[(Y)][(X)] == VOX_H_LOW)

    int regions = 0;
    static const int DX[4] = {1, -1, 0, 0};
    static const int DY[4] = {0, 0, 1, -1};
    for (int sy = first_y; sy < last_y; sy++) {
        for (int sx = 0; sx < last_x; sx++) {
            if (label[sy][sx] >= 0 || !PLATEAU_OPEN(sy, sx)) continue;
            int read = 0, write = 0;
            queue[write][0] = (int16_t)sx;
            queue[write][1] = (int16_t)sy;
            write++;
            label[sy][sx] = (int16_t)regions;
            while (read < write) {
                int x = queue[read][0], y = queue[read][1];
                read++;
                if (!grid->treecell[y][x]) ground[regions]++;
                for (int k = 0; k < 4; k++) {
                    int nx = x + DX[k], ny = y + DY[k];
                    if (nx < 0 || nx >= last_x ||
                        ny < first_y || ny >= last_y)
                        continue;
                    if (PLATEAU_OPEN(ny, nx)) {
                        if (label[ny][nx] < 0) {
                            label[ny][nx] = (int16_t)regions;
                            queue[write][0] = (int16_t)nx;
                            queue[write][1] = (int16_t)ny;
                            write++;
                        }
                    } else if (!grid->treecell[ny][nx] &&
                               grid->height[ny][nx] >= VOX_H_MID) {
                        if (grid->height[ny][nx] >= VOX_H_HIGH)
                            high_edges[regions]++;
                        else
                            mid_edges[regions]++;
                    }
                }
            }
            regions++;
        }
    }

    int datum = -1;
    for (int r = 0; r < regions; r++) {
        if (datum < 0 || ground[r] > ground[datum]) datum = r;
    }
    for (int y = first_y; y < last_y; y++) {
        for (int x = 0; x < last_x; x++) {
            int r = label[y][x];
            if (r >= 0 && r != datum && ground[r] >= 4 &&
                mid_edges[r] + high_edges[r] >= 4) {
                grid->elevation[y][x] =
                    high_edges[r] >= mid_edges[r] ? VOX_H_HIGH : VOX_H_MID;
            }
        }
    }

    /* Put compact props on the shelf they occupy. Cliff rims themselves
     * have elevated ground on only one side and stay at base zero; their
     * existing surface then meets the plateau exactly instead of
     * becoming a second wall stacked on top. */
    for (int pass = 0; pass < 2; pass++) {
        uint8_t next[VOX_TILES_H][VOX_TILES_W];
        memcpy(next, grid->elevation, sizeof(next));
        for (int y = first_y; y < last_y; y++) {
            for (int x = 0; x < last_x; x++) {
                if (grid->elevation[y][x] || grid->treecell[y][x] ||
                    grid->height[y][x] < VOX_H_MID)
                    continue;
                int raised_mid = 0, raised_high = 0, low_open = 0;
                for (int k = 0; k < 4; k++) {
                    int nx = x + DX[k], ny = y + DY[k];
                    if (nx < 0 || nx >= last_x ||
                        ny < first_y || ny >= last_y)
                        continue;
                    if (grid->elevation[ny][nx] >= VOX_H_HIGH)
                        raised_high++;
                    else if (grid->elevation[ny][nx] >= VOX_H_MID)
                        raised_mid++;
                    else if (PLATEAU_OPEN(ny, nx)) low_open++;
                }
                if (raised_mid + raised_high >= 3 && low_open == 0) {
                    next[y][x] = raised_high >= raised_mid
                        ? VOX_H_HIGH : VOX_H_MID;
                }
            }
        }
        memcpy(grid->elevation, next, sizeof(next));
    }
#undef PLATEAU_OPEN
}

bool vox_scrape(GBContext* ctx, const uint32_t* fb, VoxTileGrid* grid,
                VoxSpriteList* sprites) {
    if (!ctx || !ctx->vram || !ctx->oam || !ctx->io || !ctx->ppu) return false;

    /* Dialog frames deliberately reuse the last complete world scrape. Keep
     * the screen-space metadata that belongs to that texture alongside it:
     * overwriting only hud_rows/fine scroll before the freeze made the old
     * HUD tiles become terrain, and dropping scripted_scene switched a
     * frozen stage diorama back through the chase projection. */
    static bool s_have_world = false;
    const int held_hud_rows = s_have_world ? grid->hud_rows : 0;
    const uint8_t held_scx = s_have_world ? grid->scx : 0;
    const uint8_t held_scy = s_have_world ? grid->scy : 0;
    const uint8_t held_fine_x = s_have_world ? grid->fine_x : 0;
    const uint8_t held_fine_y = s_have_world ? grid->fine_y : 0;
    const bool held_scripted_scene =
        s_have_world ? grid->scripted_scene : false;

    uint8_t lcdc = io_reg(ctx, 0x40);
    if (!(lcdc & 0x80)) return false;   /* LCD off */

    uint8_t scx = io_reg(ctx, 0x43);
    uint8_t scy = io_reg(ctx, 0x42);

    /* VOXEL_DEBUG=1 dumps the PPU registers the classifier depends on. The
     * HUD band in particular is a raster trick on these carts, not the
     * window layer, and this is how that was pinned down. */
    if (getenv("VOXEL_DEBUG")) {
        static int n = 0;
        if ((n++ % 60) == 0) {
            fprintf(stderr,
                    "[VOXEL] lcdc=%02X scx=%3u scy=%3u wy=%3u wx=%3u ly=%3u\n",
                    lcdc, scx, scy, io_reg(ctx, 0x4A), io_reg(ctx, 0x4B),
                    io_reg(ctx, 0x44));
        }
    }
    {
        const char* want = getenv("VOXEL_DUMP_MAP");
        if (want) {
            static long n = 0;
            if (n++ == atol(want)) vox_dump_bg_map(ctx, "bgmap.ppm");
        }
    }
    grid->scx = scx;
    grid->scy = scy;
    grid->fine_x = scx & 7;
    grid->fine_y = scy & 7;

    /* HUD band: tile rows at the top of the screen that are sourced from the
     * BG map's reserved bottom strip (map rows 30-31). See VoxTileGrid. */
    grid->hud_rows = 0;
    for (int ty = 0; ty < VOX_TILES_H; ty++) {
        unsigned map_y = ((scy / 8) + ty) & 31;
        if (map_y < VOX_HUD_MAP_ROW) break;
        grid->hud_rows = (ty + 1) * 8 - grid->fine_y;
    }
    if (grid->hud_rows < 0) grid->hud_rows = 0;
    if (grid->hud_rows > GB_SCREEN_HEIGHT) grid->hud_rows = GB_SCREEN_HEIGHT;

    bool signed_tiles = !(lcdc & 0x10);
    unsigned map_base = (lcdc & 0x08) ? 0x1C00 : 0x1800;

    /* Game-aware pass: if this is an Oracles cart in trustworthy room
     * state, terrain height comes from the cart's own collision grid and
     * the colours only break ties. Otherwise colours decide everything,
     * exactly as before. */
    VoxOracleState oracle;
    bool use_oracle = vox_oracle_read(ctx, &oracle) && oracle.valid;
    /* An Oracles cart whose frame can't be trusted right now (room scroll,
     * cinematic): hold the diorama flat and steady rather than handing the
     * screen to the colour classifier, whose guessed terrain flickers for
     * the half-second of every room walk. */
    bool oracle_cart = oracle.profile_matched;
    grid->scripted_scene = oracle_cart && oracle.scripted_scene;
    /* Hand the screen back untouched whenever the cart is showing
     * something that isn't terrain: a menu, or no room at all (title,
     * file select, cutscenes). */
    grid->flat = oracle_cart && (oracle.menu_open || oracle.no_room);
    grid->text_overlay = false;

    /* A dialog box over a live room: the box is drawn into the BG
     * tilemap, so extruding it garbles the words and re-decoding the
     * texture would paint the box into the terrain. The game is paused
     * under dialog, so freeze the diorama exactly as it was, find the
     * box by diffing the composed frame against the frozen texture, and
     * let the renderer float it flat on top. (Without a frozen world to
     * show -- dialog on the very first frames -- fall back to flat.) */
    /* True when this scrape jumps to the sprite scan with the world grid
     * FROZEN (dialog over a live room). The tree extraction below the
     * label must not run then: it would match the frozen previous room's
     * tiles against the CURRENT room's collision data, and a dialog that
     * opened right after a room change silently deleted every tree until
     * the next refresh. Frozen frames keep the previous tree list, which
     * is exactly what a frozen diorama should show. */
    bool frozen = false;
    if (oracle_cart && oracle.text_active && !grid->flat) {
        if (!s_have_world) {
            grid->flat = true;
        } else {
            /* `tex`, heights, trees and sky are still the previous complete
             * world. Restore their coordinate system and camera choice too.
             * The live framebuffer remains current and supplies the dialog
             * rectangle and HUD below. */
            grid->hud_rows = held_hud_rows;
            grid->scx = held_scx;
            grid->scy = held_scy;
            grid->fine_x = held_fine_x;
            grid->fine_y = held_fine_y;
            grid->scripted_scene =
                grid->scripted_scene || held_scripted_scene;
            grid->text_overlay = true;
            int y0 = GB_SCREEN_HEIGHT, y1 = -1, x0 = GB_SCREEN_WIDTH, x1 = -1;
            for (int y = grid->hud_rows; y < GB_SCREEN_HEIGHT; y++) {
                int miss = 0;
                for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
                    if (fb[y * GB_SCREEN_WIDTH + x] !=
                        grid->tex[(y + grid->fine_y) * VOX_TEX_W + x + grid->fine_x]) {
                        miss++;
                    }
                }
                if (miss > GB_SCREEN_WIDTH / 2) {
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
            if (y1 >= y0) {
                for (int y = y0; y <= y1; y++) {
                    for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
                        if (fb[y * GB_SCREEN_WIDTH + x] !=
                            grid->tex[(y + grid->fine_y) * VOX_TEX_W + x + grid->fine_x]) {
                            if (x < x0) x0 = x;
                            if (x > x1) x1 = x;
                        }
                    }
                }
                grid->box_x = x0;
                grid->box_y = y0;
                grid->box_w = x1 - x0 + 1;
                grid->box_h = y1 - y0 + 1;
            } else {
                /* Box not on screen yet (opening frame): nothing to blit. */
                grid->box_w = 0;
                grid->box_h = 0;
            }
            /* Frozen world: skip the state/texture refresh entirely, but
             * keep the sprite scan live so characters stay animated. */
            frozen = true;
            goto scan_sprites;
        }
    }

    /* The hud_rows walk above assumes the HUD strip wraps in at the TOP of
     * the window, which the Oracles carts routinely violate: rooms are
     * double-buffered across the BG map and presented with a mid-frame
     * raster split that pins the HUD to the top 16 scanlines -- invisible
     * to a single latched scy, whose meaning turned out to vary by room
     * (240 -> world at row 0, 112 -> row 14, and at least one more parity
     * that put the HUD's heart row INSIDE the terrain). So stop inferring:
     * score every candidate origin row against the game's own composed
     * frame on two world scanlines and take the winner. fb is ground
     * truth; sprites only cost a few pixels of score. */
    int world_row0 = -1;   /* <0: generic scy mapping */
    if (use_oracle) {
        grid->hud_rows = 16;
        const int probe[2] = {grid->hud_rows + 5, grid->hud_rows + 61};
        int best = -1;
        int best_score = -1;
        for (int r = 0; r < 32; r++) {
            int sc = 0;
            for (int p = 0; p < 2; p++) {
                sc += score_origin(ctx, fb, map_base, signed_tiles, scx,
                                   grid->fine_x, grid->hud_rows, grid->fine_y,
                                   r, probe[p]);
            }
            if (sc > best_score) { best_score = sc; best = r; }
        }
        if (best_score >= GB_SCREEN_WIDTH) {   /* >= 50% of probed pixels */
            world_row0 = best;
        } else {
            /* Frame too busy to calibrate (huge sprites over both probe
             * lines): fall back to the two known scy parities. */
            unsigned r = (scy >> 3) & 31;
            world_row0 = (int)((r >= VOX_HUD_MAP_ROW) ? (r + 2) & 31 : r);
        }
    }

    /* Backdrop sky from live game state. Outdoors only: groups 0-1 are
     * the overworlds (Ages present/past; Seasons overworld/Subrosia),
     * everything else is interior and keeps the neutral backdrop. */
    static int s_last_sky = VOX_SKY_NONE;
    grid->sky = VOX_SKY_NONE;
    if (use_oracle && oracle.active_group <= 1) {
        if (oracle.is_seasons) {
            if (oracle.active_group == 1) {
                grid->sky = VOX_SKY_SUBROSIA;
            } else {
                switch (oracle.room_state & 3) {
                    case 0: grid->sky = VOX_SKY_SPRING; break;
                    case 1: grid->sky = VOX_SKY_SUMMER; break;
                    case 2: grid->sky = VOX_SKY_AUTUMN; break;
                    default: grid->sky = VOX_SKY_WINTER; break;
                }
            }
        } else {
            grid->sky = (oracle.active_group == 1) ? VOX_SKY_AGES_PAST
                                                   : VOX_SKY_AGES_PRESENT;
        }
    }
    if (use_oracle) {
        s_last_sky = grid->sky;
    } else if (oracle_cart) {
        /* Mid-transition: keep whatever sky the last good frame had, so
         * walking between two outdoor rooms doesn't blink the sky off. */
        grid->sky = s_last_sky;
    }

    /* Link's screen position, from his room position and the camera. His
     * feet row is where his shadow falls -- z does not move it. */
    grid->link_known = use_oracle;
    grid->link_item_get = use_oracle &&
        vox_oracle_link_holds_item((uint8_t)oracle.link_state);
    if (use_oracle) {
        grid->link_sx = oracle.link_x - oracle.cam_x - oracle.off_x;
        /* +8 calibrated against live OAM: w1Link.y sits 8px above the
         * sprite's bottom edge (feet=80 for link_y=56 with a 16px HUD). */
        grid->link_feet_sy = oracle.link_y - oracle.cam_y - oracle.off_y
                             + grid->hud_rows + 8;
        grid->link_jump = oracle.link_z < 0 ? -oracle.link_z : 0;
        grid->link_dir = oracle.link_dir;
        /* The in-game editor's brush follows Link: his cell, in room
         * cells, plus one in his facing. link_x/y are room-space
         * already; +4 biases his feet toward the cell he is standing
         * in rather than the boundary he is touching. */
        vox_edit_track(oracle.is_seasons, oracle.active_group,
                       oracle.active_room, oracle.link_x >> 4,
                       (oracle.link_y + 4) >> 4, oracle.link_dir,
                       oracle.collisions);
        if (getenv("VOXEL_DEBUG")) {
            static int n = 0;
            if ((n++ % 30) == 0) {
                fprintf(stderr,
                        "[VOXEL] link room(%3d,%3d) z=%3d -> screen(%3d,%3d)\n",
                        oracle.link_x, oracle.link_y, oracle.link_z,
                        grid->link_sx, grid->link_feet_sy);
                /* Ground truth: the OAM entries whose x is near Link's
                 * computed centre. Their feet rows calibrate the anchor. */
                bool tall = (io_reg(ctx, 0x40) & 0x04) != 0;
                for (int i = 0; i < 40; i++) {
                    const uint8_t* e = ctx->oam + i * 4;
                    int sy = (int)e[0] - 16, sx = (int)e[1] - 8;
                    if (sy <= -16 || sy >= 144) continue;
                    if (sx < grid->link_sx - 16 || sx > grid->link_sx + 8) continue;
                    fprintf(stderr, "[VOXEL]   oam#%02d x=%3d feet=%3d\n",
                            i, sx, sy + (tall ? 16 : 8));
                }
            }
        }
    }

    /* One override lookup for the whole frame. It re-reads the file when
     * the file changes on disk, so sculpting a room is edit-and-see. */
    const uint8_t* ov = use_oracle
        ? vox_override_lookup(oracle.is_seasons, oracle.active_group,
                              oracle.active_room, oracle.collisions)
        : NULL;

    for (int ty = 0; ty < VOX_TILES_H; ty++) {
        for (int tx = 0; tx < VOX_TILES_W; tx++) {
            unsigned map_x = ((scx / 8) + tx) & 31;
            /* Tile rows 0-1 under the fixed HUD are never sampled by the
             * renderer; the wrapped index they get is harmless. */
            unsigned map_y = (world_row0 >= 0)
                ? (unsigned)((world_row0 + ty - 2) & 31)
                : ((scy / 8) + ty) & 31;
            unsigned map_off = map_base + map_y * 32 + map_x;

            uint8_t tile = ctx->vram[map_off];
            uint8_t attr = ctx->vram[0x2000 + map_off];   /* CGB attrs, bank 1 */
            int pal = attr & 0x07;
            int bank = (attr >> 3) & 1;

            const uint8_t* pat = tile_pattern(ctx, tile, signed_tiles, bank);
            bool leafy = false;
            uint8_t by_colour = classify_tile(ctx, pat, pal, &leafy);
            grid->leafy[ty][tx] = leafy ? 1 : 0;

            /* Decode this tile's pixels into the sprite-free terrain
             * texture. The composed frame can't be the ground texture:
             * sprites are baked into it, so the column march used to warp
             * a flattened copy of every character into the terrain right
             * under their upright object. CGB BG tiles flip via the
             * same attr bits OBJs use. */
            {
                GBPPU* ppu = (GBPPU*)ctx->ppu;
                bool xf = (attr & 0x20) != 0;
                bool yf = (attr & 0x40) != 0;
                uint32_t pal_rgb[4];
                for (int i = 0; i < 4; i++) {
                    pal_rgb[i] = cgb_color(ppu->bg_palette_ram, pal, i);
                }
                for (int row = 0; row < 8; row++) {
                    int src = yf ? 7 - row : row;
                    uint8_t lo = pat[src * 2];
                    uint8_t hi = pat[src * 2 + 1];
                    uint32_t* dst = &grid->tex[(ty * 8 + row) * VOX_TEX_W + tx * 8];
                    for (int c = 0; c < 8; c++) {
                        int bit = xf ? c : 7 - c;
                        int idx = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                        dst[c] = pal_rgb[idx];
                    }
                }
            }

            if (use_oracle) {
                /* Sample this tile's centre in room space. Screen origin of
                 * the tile is (tx*8 - fine_x, ty*8 - fine_y); the room is
                 * drawn below the HUD band, offset by camera and any
                 * transient screen offset. */
                int sx = tx * 8 - grid->fine_x + 4;
                int sy = ty * 8 - grid->fine_y + 4;
                int wx = sx + oracle.cam_x + oracle.off_x;
                int wy = sy - grid->hud_rows + oracle.cam_y + oracle.off_y;
                int col = wx >> 4, row = wy >> 4;

                /* HUD tiles are not part of the room. Outdoors their wy
                 * is negative and the row bound already skips them, but a
                 * large scrolling interior (cam_y > 0) maps them onto
                 * real room rows -- Veran's tower drew its off-screen top
                 * wall into the HUD band as floating blocks. */
                if (sy >= grid->hud_rows &&
                    row >= 0 && row < 12 && col >= 0 && col < 16) {
                    uint8_t coll = oracle.collisions[row * 16 + col];
                    /* $01-$0F is a four-bit occupancy mask, not a height
                     * code. Preserve its exact 8x8 footprint: bit 3 is the
                     * top-left quadrant and bit 0 the bottom-right. */
                    int qx = ((wx - col * 16) >= 8) ? 1 : 0;
                    int qy = ((wy - row * 16) >= 8) ? 1 : 0;
                    int quadrant = qy * 2 + qx;
                    uint8_t hcls = vox_oracle_quadrant_height(
                        coll, by_colour, quadrant);
                    /* Hand-authored room overrides get the final word --
                     * for props the player can walk past (statue rows,
                     * gates) that read as "should be 3D" to a human and
                     * as $00 to the collision map. Looked up once per
                     * frame above, not per tile: the lookup now stats the
                     * file to catch live edits. */
                    if (ov && row < 8 && col < 10 &&
                        ov[row * 10 + col] != 0xFF) {
                        hcls = ov[row * 10 + col];
                    }
                    /* Sculpt mode: pulse the brush cell gold in the
                     * terrain texture itself, so the highlight lands on
                     * the real extruded geometry in every voxel mode
                     * rather than floating in screen space. */
                    {
                        int ec, er;
                        if (vox_edit_cursor(&ec, &er) &&
                            col == ec && row == er) {
                            int a = vox_edit_pulse();      /* /256 */
                            uint32_t* px =
                                &grid->tex[(ty * 8) * VOX_TEX_W + tx * 8];
                            for (int yy = 0; yy < 8; yy++) {
                                for (int xx = 0; xx < 8; xx++) {
                                    uint32_t c = px[yy * VOX_TEX_W + xx];
                                    int r8 = (c >> 16) & 0xFF;
                                    int g8 = (c >> 8) & 0xFF;
                                    int b8 = c & 0xFF;
                                    r8 += ((0xDE - r8) * a) >> 8;
                                    g8 += ((0xB2 - g8) * a) >> 8;
                                    b8 += ((0x4C - b8) * a) >> 8;
                                    px[yy * VOX_TEX_W + xx] =
                                        (c & 0xFF000000u) |
                                        ((uint32_t)r8 << 16) |
                                        ((uint32_t)g8 << 8) | (uint32_t)b8;
                                }
                            }
                        }
                    }
                    grid->height[ty][tx] = hcls;
                    continue;
                }
                /* Above the HUD line or outside the room: flat. */
                grid->height[ty][tx] = VOX_H_FLOOR;
                continue;
            }

            /* Oracles cart, untrusted frame: flat slab, no colour noise. */
            grid->height[ty][tx] = oracle_cart ? VOX_H_FLOOR : by_colour;
        }
    }

    s_have_world = use_oracle;

    /* A cliff is one object: let it agree with itself before the
     * renderer extrudes it. Off restores the per-tile colour vote. */
    if (use_oracle && voxel_tuning()->cliff_unify > 0.5f)
        vox_unify_solid_masses(grid->height);

scan_sprites:
    /* Foliage is classified per 8px tile, but a tree is a 16px object: one
     * half of it reading "leafy" and the other half not made the renderer's
     * footprint taper carve some sub-tiles and not others, which at tall
     * tree heights turned a forest into a comb of spikes. Spread the flag
     * across each contiguous run of equal-height cells, so one mass agrees
     * with itself. Two passes reach across a 2x2 object. */
    for (int pass = 0; pass < 2; pass++) {
        uint8_t next[VOX_TILES_H][VOX_TILES_W];
        memcpy(next, grid->leafy, sizeof(next));
        for (int ty = 0; ty < VOX_TILES_H; ty++) {
            for (int tx = 0; tx < VOX_TILES_W; tx++) {
                if (grid->leafy[ty][tx]) continue;
                uint8_t h = grid->height[ty][tx];
                if (h == VOX_H_FLOOR || h == VOX_H_WATER) continue;
                if ((tx > 0               && grid->height[ty][tx - 1] == h && grid->leafy[ty][tx - 1]) ||
                    (tx < VOX_TILES_W - 1 && grid->height[ty][tx + 1] == h && grid->leafy[ty][tx + 1]) ||
                    (ty > 0               && grid->height[ty - 1][tx] == h && grid->leafy[ty - 1][tx]) ||
                    (ty < VOX_TILES_H - 1 && grid->height[ty + 1][tx] == h && grid->leafy[ty + 1][tx])) {
                    next[ty][tx] = 1;
                }
            }
        }
        memcpy(grid->leafy, next, sizeof(next));
    }

    /* Compound architecture first. Impa's house is not six unrelated solid
     * cells: $9B/$9C/$9D are one authored 48px canopy over a matching
     * doorway row. Preserve those two live strips as one structure and mark
     * their old overhead footprint for removal from the chase heightfield.
     * The exact marker sequence and room guard avoid assigning Ages' object
    * IDs meanings in another tileset or in Seasons. */
    if (!frozen) {
        grid->structure_count = 0;
        memset(grid->structurecell, 0, sizeof(grid->structurecell));
        if (use_oracle && !oracle.is_seasons && oracle.active_group == 0 &&
            oracle.active_room == 0x3A) {
        for (int row = 0; row + 1 < 12; row++) {
            for (int col = 0; col + 2 < 16; col++) {
                int at = row * 16 + col;
                if (oracle.layout[at] != 0x9B ||
                    oracle.layout[at + 1] != 0x9C ||
                    oracle.layout[at + 2] != 0x9D)
                    continue;
                if (grid->structure_count >= VOX_MAX_STRUCTURES) break;

                int sx = col * 16 - oracle.cam_x - oracle.off_x;
                int roof_sy = row * 16 - oracle.cam_y - oracle.off_y
                              + grid->hud_rows;
                int tx0 = (sx + grid->fine_x) >> 3;
                int ty0 = (roof_sy + grid->fine_y) >> 3;
                if (tx0 < 0 || ty0 < 0 ||
                    tx0 + 5 >= VOX_TILES_W || ty0 + 3 >= VOX_TILES_H)
                    continue;

                VoxStructure* b =
                    &grid->structures[grid->structure_count++];
                b->sx = sx;
                b->sy = roof_sy + 16; /* the doorway row is its footprint */
                int px0 = tx0 * 8, py0 = ty0 * 8;
                for (int py = 0; py < VOX_STRUCTURE_H; py++) {
                    memcpy(&b->roof[py * VOX_STRUCTURE_W],
                           &grid->tex[(py0 + py) * VOX_TEX_W + px0],
                           VOX_STRUCTURE_W * sizeof(uint32_t));
                    memcpy(&b->front[py * VOX_STRUCTURE_W],
                           &grid->tex[(py0 + 16 + py) * VOX_TEX_W + px0],
                           VOX_STRUCTURE_W * sizeof(uint32_t));
                }
                for (int dy = 0; dy < 4; dy++) {
                    for (int dx = 0; dx < 6; dx++)
                        grid->structurecell[ty0 + dy][tx0 + dx] = 1;
                }
                structure_strip_mask(grid, tx0, ty0,
                                     b->roof, b->roof_solid);
                structure_strip_mask(grid, tx0, ty0 + 2,
                                     b->front, b->front_solid);
            }
        }
    }

    /* Object-aware vegetation for the chase camera: a solid room cell whose
     * four screen tiles all read leafy at full height is one 16px source
     * drawing. The renderer pulls these cells out of the heightfield, turns
     * each drawing into separate trunk and canopy tiles, and leaves real
     * ground beneath it. Every visible colour comes from the tree's own art,
     * so autumn, winter and Subrosia keep their palettes. */
    grid->tree_count = 0;
    memset(grid->treecell, 0, sizeof(grid->treecell));
    /* Outdoors only: wRoomLayout's object IDs are read against the
     * overworld tileset, and interiors reuse the same numbers for
     * furniture. */
    if (use_oracle && oracle.active_group <= 1) {
        for (int row = 0; row < 12; row++) {
            for (int col = 0; col < 16; col++) {
                if (grid->tree_count >= VOX_MAX_TREES) break;
                if (oracle.collisions[row * 16 + col] != 0x0F) continue;
                /* The layout ID names the object, which colour guessing
                 * could not: verified live in Ages, the forest border is
                 * a mosaic of $20-$3F tree-mass tiles while the cuttable
                 * bushes and grass tufts standing in the meadow are $C5 --
                 * the same cells the colour classifier scored identically.
                 * Trees stand tall; destructibles hug the ground; anything
                 * unrecognised stays extruded terrain. */
                uint8_t id = oracle.layout[row * 16 + col];
                bool is_tree = (id >= 0x20 && id <= 0x3F);
                bool is_tuft = (id >= 0xC0 && id <= 0xCF);
                if (!is_tree && !is_tuft) continue;
                /* A mod can provide alternate art. Either way the pixels are
                 * extruded later; leaving known tree IDs as terrain is what
                 * turned forests into continuous green ramparts. */
                const uint32_t* mod_art = vox_mod_tree_art(is_tree);
                int sx = col * 16 - oracle.cam_x - oracle.off_x;
                int sy = row * 16 - oracle.cam_y - oracle.off_y
                         + grid->hud_rows;
                int tx0 = (sx + grid->fine_x) >> 3;
                int ty0 = (sy + grid->fine_y) >> 3;
                if (tx0 < 0 || ty0 < 0 ||
                    tx0 + 1 >= VOX_TILES_W || ty0 + 1 >= VOX_TILES_H)
                    continue;
                bool in_structure = false;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        if (grid->structurecell[ty0 + dy][tx0 + dx])
                            in_structure = true;
                    }
                }
                if (in_structure) continue;
                /* Tree-mass IDs are the tileset's connected drawing. Some
                 * edge/corner fragments contain mostly trunk, snow or empty
                 * surround and therefore do not pass a green-pixel vote;
                 * dropping those fragments tears holes through the original
                 * crown row. The authoritative layout wins for trees. Low
                 * destructible IDs still require the visual guard because
                 * seasons/mods may restyle those reusable object slots. */
                bool all = true;
                if (!is_tree) {
                    for (int dy = 0; dy < 2 && all; dy++) {
                        for (int dx = 0; dx < 2; dx++) {
                            if (!grid->leafy[ty0 + dy][tx0 + dx] ||
                                grid->height[ty0 + dy][tx0 + dx] < VOX_H_MID) {
                                all = false;
                                break;
                            }
                        }
                    }
                }
                if (!all) continue;
                int hmin = is_tree ? VOX_H_HIGH : VOX_H_MID;

                VoxTree* t = &grid->trees[grid->tree_count];
                uint32_t source[16 * 16];
                if (mod_art) {
                    memcpy(source, mod_art, sizeof(source));
                } else {
                    int px0 = tx0 * 8, py0 = ty0 * 8;
                    for (int py = 0; py < 16; py++) {
                        memcpy(&source[py * 16],
                               &grid->tex[(py0 + py) * VOX_TEX_W + px0],
                               16 * sizeof(uint32_t));
                    }
                }
                memcpy(t->tex, source, sizeof(t->tex));

                grid->tree_count++;
                t->sx = sx;
                t->sy = sy;
                t->hcls = hmin;
                t->custom_art = mod_art != NULL;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        grid->treecell[ty0 + dy][tx0 + dx] = 1;
                    }
                }
            }
        }
    }

    for (int i = 0; i < grid->tree_count; i++) {
        VoxTree* t = &grid->trees[i];
        int tx0 = (t->sx + grid->fine_x) >> 3;
        int ty0 = (t->sy + grid->fine_y) >> 3;
        tree_relief_mask(grid, tx0, ty0, t);
    }
    if (use_oracle && oracle.active_group <= 1)
        vox_infer_plateaus(grid);
    else
        memset(grid->elevation, 0, sizeof(grid->elevation));

    /* Collision correctly describes where Link may walk, but some visible
     * objects are interactive props rather than collision walls. Apply those
     * semantic heights after mass/plateau inference so a chest cannot merge
     * into a cliff or convince the terrain around it to become a plateau.
     * The heightfield already carries the exact live BG pixels, so raising
     * these four 8px quadrants produces a textured 16x16 block rather than a
     * guessed replacement model. */
    if (use_oracle) {
        for (int row = 0; row < 12; row++) {
            for (int col = 0; col < 16; col++) {
                uint8_t hcls = vox_oracle_object_height(
                    oracle.layout[row * 16 + col]);
                if (hcls == 0xFF) continue;
                int sx = col * 16 - oracle.cam_x - oracle.off_x;
                int sy = row * 16 - oracle.cam_y - oracle.off_y
                         + grid->hud_rows;
                int tx0 = (sx + grid->fine_x) >> 3;
                int ty0 = (sy + grid->fine_y) >> 3;
                if (tx0 < 0 || ty0 < 0 ||
                    tx0 + 1 >= VOX_TILES_W || ty0 + 1 >= VOX_TILES_H)
                    continue;
                bool in_structure = false;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        if (grid->structurecell[ty0 + dy][tx0 + dx])
                            in_structure = true;
                    }
                }
                if (in_structure) continue;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        grid->height[ty0 + dy][tx0 + dx] = hcls;
                        grid->leafy[ty0 + dy][tx0 + dx] = 0;
                    }
                }
            }
        }
    }
    }   /* !frozen */

    /* Feed the persistent world: a trusted frame both refreshes this
     * room's memory and anchors the neighbour samplers to it; an
     * untrusted one (scroll, cinematic) drops the anchor so the renderer
     * falls back to its edge fade. Frozen dialog frames touch neither --
     * the world under the dialog stays exactly as it was. */
    if (!frozen) {
        if (use_oracle) vox_world_remember(&oracle, grid);
        else vox_world_lose();
    }

    /* OAM scrape: raw entries, decoded lazily at draw time. */
    sprites->count = 0;
    bool tall = (lcdc & 0x04) != 0;
    for (int i = 0; i < 40 && sprites->count < VOX_MAX_SPRITES; i++) {
        const uint8_t* e = ctx->oam + i * 4;
        int sy = (int)e[0] - 16;
        int sx = (int)e[1] - 8;
        if (sy <= -16 || sy >= 144 || sx <= -8 || sx >= 160) continue;
        /* A sprite wholly behind the opaque HUD bar is interface, not
         * world: the games park indicator sprites under it (Veran's
         * tower keeps two at sy=0), invisible in 2D but billboarded by
         * the chase cam as phantom pillars on the horizon. A sprite
         * merely crossing the top edge still shows below the bar and is
         * kept. */
        if (sy + (tall ? 16 : 8) <= grid->hud_rows) continue;
        VoxSprite* s = &sprites->entries[sprites->count++];
        s->y = (int16_t)sy;
        s->x = (int16_t)sx;
        s->tile = tall ? (e[2] & 0xFE) : e[2];
        s->attr = e[3];
        s->tall = tall;
    }
    return true;
}

void vox_decode_sprite_row(GBContext* ctx, const VoxSprite* s, int row,
                           uint32_t out[8]) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    int height = s->tall ? 16 : 8;
    int src_row = (s->attr & 0x40) ? (height - 1 - row) : row;   /* Y flip */

    uint8_t tile = s->tile + (src_row >= 8 ? 1 : 0);
    const uint8_t* pat = ctx->vram + ((s->attr >> 3) & 1) * 0x2000 + tile * 16;
    int r = src_row & 7;
    uint8_t lo = pat[r * 2];
    uint8_t hi = pat[r * 2 + 1];
    int pal = s->attr & 0x07;

    for (int px = 0; px < 8; px++) {
        int bit = (s->attr & 0x20) ? px : (7 - px);   /* X flip */
        int idx = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        out[px] = idx ? cgb_color(ppu->obj_palette_ram, pal, idx) : 0;
    }
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* VOXEL_DUMP_MAP=<frame> writes the whole 32x32 BG map (256x256 px) as a
 * PPM. The screen only ever shows a 160x144 window into it, so this is how
 * you find out how much real world data sits off-screen -- which is the
 * thing that decides whether widescreen is possible at all. */
void vox_dump_bg_map(GBContext* ctx, const char* path) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    uint8_t lcdc = io_reg(ctx, 0x40);
    bool signed_tiles = !(lcdc & 0x10);
    unsigned map_base = (lcdc & 0x08) ? 0x1C00 : 0x1800;

    static uint8_t img[256 * 256 * 3];
    for (int my = 0; my < 32; my++) {
        for (int mx = 0; mx < 32; mx++) {
            unsigned off = map_base + my * 32 + mx;
            uint8_t tile = ctx->vram[off];
            uint8_t attr = ctx->vram[0x2000 + off];
            const uint8_t* pat = tile_pattern(ctx, tile, signed_tiles, (attr >> 3) & 1);
            for (int row = 0; row < 8; row++) {
                uint8_t lo = pat[row * 2], hi = pat[row * 2 + 1];
                for (int col = 0; col < 8; col++) {
                    int idx = ((lo >> (7 - col)) & 1) | (((hi >> (7 - col)) & 1) << 1);
                    uint32_t c = cgb_color(ppu->bg_palette_ram, attr & 7, idx);
                    int px = mx * 8 + col, py = my * 8 + row;
                    uint8_t* o = &img[(py * 256 + px) * 3];
                    o[0] = (uint8_t)(c & 0xFF);
                    o[1] = (uint8_t)((c >> 8) & 0xFF);
                    o[2] = (uint8_t)((c >> 16) & 0xFF);
                }
            }
        }
    }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n256 256\n255\n");
    fwrite(img, 1, sizeof(img), f);
    fclose(f);
    fprintf(stderr, "[VOXEL] BG map dumped to %s\n", path);
}
