/* PPU scraping + terrain classification for the voxel diorama.
 *
 * Everything here reads the same VRAM/OAM/IO state the PPU renders from.
 * Nothing is game-specific by symbol — classification works off what the
 * tiles look like, so it holds up across both Oracles carts (and any other
 * top-down GBC cart, roughly).
 */
#include "voxel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* raw PPU access                                                      */
/* ------------------------------------------------------------------ */

static inline uint8_t io_reg(GBContext* ctx, uint8_t reg) {
    return ctx->io[reg];
}

/* CGB BGR555 -> RGBA8888 (matches the runtime's own conversion closely
 * enough for classification and billboard decode). */
static uint32_t cgb_color(const uint8_t* pal_ram, int pal, int idx) {
    int off = pal * 8 + idx * 2;
    uint16_t raw = (uint16_t)(pal_ram[off] | (pal_ram[off + 1] << 8));
    uint32_t r = (raw & 0x1F) * 255 / 31;
    uint32_t g = ((raw >> 5) & 0x1F) * 255 / 31;
    uint32_t b = ((raw >> 10) & 0x1F) * 255 / 31;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
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
    *r = (int)(c & 0xFF);
    *g = (int)((c >> 8) & 0xFF);
    *b = (int)((c >> 16) & 0xFF);
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

static uint8_t classify_tile(GBContext* ctx, const uint8_t* pat, int pal) {
    TileFeatures f = tile_features(ctx, pat, pal);

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

bool vox_scrape(GBContext* ctx, VoxTileGrid* grid, VoxSpriteList* sprites) {
    if (!ctx || !ctx->vram || !ctx->oam || !ctx->io || !ctx->ppu) return false;

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

    for (int ty = 0; ty < VOX_TILES_H; ty++) {
        for (int tx = 0; tx < VOX_TILES_W; tx++) {
            unsigned map_x = ((scx / 8) + tx) & 31;
            unsigned map_y = ((scy / 8) + ty) & 31;
            unsigned map_off = map_base + map_y * 32 + map_x;

            uint8_t tile = ctx->vram[map_off];
            uint8_t attr = ctx->vram[0x2000 + map_off];   /* CGB attrs, bank 1 */
            int pal = attr & 0x07;
            int bank = (attr >> 3) & 1;

            const uint8_t* pat = tile_pattern(ctx, tile, signed_tiles, bank);
            grid->height[ty][tx] = classify_tile(ctx, pat, pal);
        }
    }

    /* OAM scrape: raw entries, decoded lazily at draw time. */
    sprites->count = 0;
    bool tall = (lcdc & 0x04) != 0;
    for (int i = 0; i < 40 && sprites->count < VOX_MAX_SPRITES; i++) {
        const uint8_t* e = ctx->oam + i * 4;
        int sy = (int)e[0] - 16;
        int sx = (int)e[1] - 8;
        if (sy <= -16 || sy >= 144 || sx <= -8 || sx >= 160) continue;
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
