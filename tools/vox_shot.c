/* Frame-grabber for the voxel diorama.
 *
 * Boots a ROM headlessly, optionally drives it with the runtime's input
 * script ("frame:buttons:duration", buttons U/D/L/R/A/B/S/T), and at each
 * requested frame dumps the flat frame and the diorama render side by side
 * as PPMs. This is how renderer changes get eyeballed without a display:
 *
 *   ./vox_shot <rom> <frame[,frame...]> <mode 1-3> <out_prefix> [script]
 *
 * writes <prefix>-<frame>-flat.ppm and <prefix>-<frame>-vox.ppm.
 */
#include "gbrt.h"
#include "platform_sdl.h"

#include "voxel/voxel.h"
#include "voxel/voxel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <rom> <frame[,frame...]> <mode 1-3> "
                        "<out_prefix> [input_script]\n", argv[0]);
        return 2;
    }
    int mode = atoi(argv[3]);
    if (mode < 1 || mode >= VOXEL_MODE_COUNT) mode = 2;

    /* Parse the frame list, keeping the largest as the stop point. */
    unsigned long want[64];
    int want_n = 0;
    unsigned long last = 0;
    for (char* tok = strtok(argv[2], ","); tok && want_n < 64;
         tok = strtok(NULL, ",")) {
        want[want_n] = strtoul(tok, NULL, 10);
        if (want[want_n] > last) last = want[want_n];
        want_n++;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    uint8_t* rom = (uint8_t*)malloc((size_t)size);
    if (fread(rom, 1, (size_t)size, f) != (size_t)size) return 1;
    fclose(f);

    GBConfig cfg; memset(&cfg, 0, sizeof(cfg));
    bool cgb = (rom[0x143] & 0x80) != 0;
    cfg.model = cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    cfg.cartridge_supports_cgb = cgb;
    cfg.cartridge_requires_cgb = (rom[0x143] == 0xC0);
    cfg.enable_audio = true; cfg.enable_serial = true; cfg.speed_percent = 100;

    GBContext* ctx = gb_context_create(&cfg);
    if (!gb_platform_init(3)) return 1;
    gb_platform_register_context(ctx);
    if (argc > 5) gb_platform_set_input_script(argv[5]);
    gb_context_load_rom(ctx, rom, (size_t)size);
    ctx->mbc_type = rom[0x147];
    gb_context_reset(ctx, true);

    static VoxTileGrid grid;
    static VoxSpriteList sprites;
    int shot_scale = 3;
    {
        const char* sc = getenv("VOX_SHOT_SCALE");
        if (sc) {
            int v = atoi(sc);
            if (v >= 1 && v <= 4) shot_scale = v;
        }
    }
    static uint32_t out[GB_FRAMEBUFFER_SIZE * 16];

    for (unsigned long i = 0; i <= last; i++) {
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        while (!ctx->frame_done) {
            gb_run_cycles(ctx, 0xFFFFFFFFu);
            if (!gb_platform_poll_events(ctx)) { i = last + 1; break; }
        }
        const uint32_t* fb = gb_get_framebuffer(ctx);
        if (!fb) continue;
        gb_platform_render_frame(fb);

        for (int w = 0; w < want_n; w++) {
            if (want[w] != i) continue;
            fprintf(stderr,
                    "frame %lu: scroll=%02X menu=%02X scy=%3u scx=%3u "
                    "camY=%u%u dirty=%02X\n",
                    i, ctx->wram[0xCD00 - 0xC000], ctx->wram[0xCBCB - 0xC000],
                    ctx->io[0x42], ctx->io[0x43],
                    ctx->hram[0x2B], ctx->hram[0x2A],
                    ctx->wram[0xCD01 - 0xC000]);
            if (getenv("VOX_DUMP_CC")) {
                fprintf(stderr, "  CC2C:");
                for (int c = 0; c < 12; c++) {
                    fprintf(stderr, " %02X", ctx->wram[0xCC2C + c - 0xC000]);
                }
                fprintf(stderr, "\n");
            }
            if (getenv("VOX_DUMP_LAYOUT")) {
                /* wRoomLayout $CF00: one byte per 16x16 cell naming the
                 * OBJECT there (tree, bush, sign...), not just whether it
                 * blocks. This is the table a shape-based renderer needs. */
                for (int row = 0; row < 12; row++) {
                    fprintf(stderr, "  layout %2d:", row);
                    for (int c = 0; c < 16; c++) {
                        fprintf(stderr, " %02X",
                                ctx->wram[0xCF00 + row * 16 + c - 0xC000]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (getenv("VOX_DUMP_COLL")) {
                for (int row = 0; row < 12; row++) {
                    fprintf(stderr, "  coll %2d:", row);
                    for (int c = 0; c < 16; c++) {
                        fprintf(stderr, " %02X",
                                ctx->wram[0xCE00 + row * 16 + c - 0xC000]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (getenv("VOX_DUMP_CB")) {
                for (int row = 0; row < 8; row++) {
                    fprintf(stderr, "  CB%X0:", 8 + row);
                    for (int c = 0; c < 16; c++) {
                        fprintf(stderr, " %02X",
                                ctx->wram[0xCB80 + row * 16 + c - 0xC000]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            char path[512];
            snprintf(path, sizeof(path), "%s-%lu-flat.ppm", argv[4], i);
            write_ppm(path, fb, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
            if (vox_scrape(ctx, fb, &grid, &sprites)) {
                vox_render(ctx, &grid, &sprites, fb, mode, shot_scale, out);
                snprintf(path, sizeof(path), "%s-%lu-vox.ppm", argv[4], i);
                write_ppm(path, out, GB_SCREEN_WIDTH * shot_scale,
                          GB_SCREEN_HEIGHT * shot_scale);
            } else {
                fprintf(stderr, "frame %lu: scrape declined (LCD off?)\n", i);
            }
        }
    }

    gb_platform_shutdown();
    return 0;
}
