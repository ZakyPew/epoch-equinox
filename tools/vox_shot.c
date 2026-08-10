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
 *
 * Env knobs: VOX_SHOT_SCALE (1-4), VOX_SHOT_STATE (resume from a savestate),
 * VOX_SHOT_NOCACHE (render as if no room had ever been visited).
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

    /* VOX_SHOT_STATE=<path>: resume from a savestate instead of cold boot.
     * Menus cost a thousand frames of scripted input to cross and the
     * interesting rooms sit far beyond them; a rewind snapshot from a real
     * session (states/rewind/NN.state) starts the probe already standing
     * there. Frame numbers then count from the state, not from power-on. */
    {
        const char* st = getenv("VOX_SHOT_STATE");
        if (st && *st) {
            if (gb_context_load_state_file(ctx, st)) {
                fprintf(stderr, "resumed from %s\n", st);
            } else {
                fprintf(stderr, "failed to load state %s\n", st);
                return 1;
            }
        }
    }

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

        /* VOX_TRACE_SCROLL=1: log wScrollMode every time it changes.
         * Sampling fixed frames answers "what was it at frame N"; the
         * terrain gate needs "which values does this game ever sit in",
         * and only the transitions show that. */
        if (getenv("VOX_TRACE_SCROLL")) {
            static int prev = -1;
            int now = ctx->wram[0xCD00 - 0xC000];
            if (now != prev) {
                fprintf(stderr, "scroll %02X -> %02X @frame %lu "
                        "(group=%02X room=%02X)\n",
                        prev < 0 ? 0 : prev, now, i,
                        ctx->wram[0xCC2D - 0xC000], ctx->wram[0xCC30 - 0xC000]);
                prev = now;
            }
        }

        /* Scrape and render EVERY frame, not just the wanted ones. Both
         * carry state across frames -- the world cache remembers each room
         * walked through, the chase camera eases its yaw and position --
         * and sampling only the frames being written left that state as it
         * was a thousand frames ago: rooms never remembered, the camera
         * still snapped to wherever it last looked. Off-frames render at
         * 1x, which is cheap, and only wanted frames pay for the scale. */
        bool wanted = false;
        for (int w = 0; w < want_n; w++) if (want[w] == i) wanted = true;

        bool scraped = vox_scrape(ctx, fb, &grid, &sprites);
        if (scraped) {
            /* VOX_SHOT_NOCACHE=1: drop the world anchor between the scrape
             * and the render, so the chase cam falls back to the edge fade.
             * The frame is otherwise identical, which is what makes a
             * persistent-world before/after pair comparable pixel for
             * pixel. The rooms stay remembered -- only this frame's view of
             * them is withheld. */
            if (getenv("VOX_SHOT_NOCACHE")) vox_world_lose();
            vox_render(ctx, &grid, &sprites, fb, mode,
                       wanted ? shot_scale : 1, out);
        }
        if (!wanted) continue;

        {
            char path[512];
            snprintf(path, sizeof(path), "%s-%lu-flat.ppm", argv[4], i);
            write_ppm(path, fb, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
            if (scraped) {
                snprintf(path, sizeof(path), "%s-%lu-vox.ppm", argv[4], i);
                write_ppm(path, out, GB_SCREEN_WIDTH * shot_scale,
                          GB_SCREEN_HEIGHT * shot_scale);
            } else {
                fprintf(stderr, "frame %lu: scrape declined (LCD off?)\n", i);
            }
            fprintf(stderr,
                    "frame %lu: scroll=%02X menu=%02X scy=%3u scx=%3u "
                    "camY=%u%u dirty=%02X trees=%d text=%02X dir=%d "
                    "link=(%d,%d) yaw=%+.3f\n",
                    i, ctx->wram[0xCD00 - 0xC000], ctx->wram[0xCBCB - 0xC000],
                    ctx->io[0x42], ctx->io[0x43],
                    ctx->hram[0x2B], ctx->hram[0x2A],
                    ctx->wram[0xCD01 - 0xC000], grid.tree_count,
                    ctx->wram[0xCBA0 - 0xC000], grid.link_dir,
                    grid.link_sx, grid.link_feet_sy, voxel_chase_yaw());
            if (getenv("VOX_DUMP_WORLD")) {
                /* Ask the persistent world what it knows just past each
                 * border of the room on screen. A row of misses means the
                 * chase cam has nothing to draw out there and will fall
                 * back to the edge fade, however many rooms were walked. */
                const int HUDR = 16, W = GB_SCREEN_WIDTH, H = GB_SCREEN_HEIGHT;
                struct { const char* n; float x, y; } p[] = {
                    { "north", W * 0.5f, HUDR - 40.0f },
                    { "south", W * 0.5f, H + 40.0f },
                    { "west",  -40.0f,   (HUDR + H) * 0.5f },
                    { "east",  W + 40.0f,(HUDR + H) * 0.5f },
                };
                fprintf(stderr, "  world:");
                for (int q = 0; q < 4; q++) {
                    float h;
                    fprintf(stderr, " %s=%s", p[q].n,
                            vox_world_height(p[q].x, p[q].y, &h) ? "hit" : "miss");
                }
                fprintf(stderr, "\n");
            }
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
            if (getenv("VOX_DUMP_HGT")) {
                fprintf(stderr, "  hud_rows=%d fine=(%d,%d)\n",
                        grid.hud_rows, grid.fine_x, grid.fine_y);
                for (int ty = 0; ty < VOX_TILES_H; ty++) {
                    fprintf(stderr, "  hgt %2d: ", ty);
                    for (int tx = 0; tx < VOX_TILES_W; tx++) {
                        int h = grid.height[ty][tx];
                        fputc(grid.leafy[ty][tx] ? "wfLMH"[h] : "01234"[h],
                              stderr);
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (getenv("VOX_DUMP_TREE")) {
                /* Why each solid cell did or didn't become a billboard
                 * tree: for every room cell, the height class + leafy flag
                 * of its four screen tiles, using the same mapping the
                 * extractor uses. */
                int cam_y = (int)ctx->hram[0x2A] | ((int)ctx->hram[0x2B] << 8);
                int cam_x = (int)ctx->hram[0x2C] | ((int)ctx->hram[0x2D] << 8);
                int off_y = (int8_t)ctx->wram[0xCD08 - 0xC000];
                int off_x = (int8_t)ctx->wram[0xCD09 - 0xC000];
                for (int row = 0; row < 12; row++) {
                    fprintf(stderr, "  tree %2d: ", row);
                    for (int col = 0; col < 16; col++) {
                        uint8_t cv = ctx->wram[0xCE00 + row * 16 + col - 0xC000];
                        if (cv != 0x0F) { fputc('.', stderr); continue; }
                        int sx = col * 16 - cam_x - off_x;
                        int sy = row * 16 - cam_y - off_y + grid.hud_rows;
                        int tx0 = (sx + grid.fine_x) >> 3;
                        int ty0 = (sy + grid.fine_y) >> 3;
                        if (tx0 < 0 || ty0 < 0 ||
                            tx0 + 1 >= VOX_TILES_W || ty0 + 1 >= VOX_TILES_H) {
                            fputc('o', stderr);   /* out of tile grid */
                            continue;
                        }
                        int hi = 9, lo = -1, leaf = 0;
                        for (int dy = 0; dy < 2; dy++)
                            for (int dx = 0; dx < 2; dx++) {
                                int h = grid.height[ty0 + dy][tx0 + dx];
                                if (h < hi) hi = h;
                                if (h > lo) lo = h;
                                leaf += grid.leafy[ty0 + dy][tx0 + dx];
                            }
                        char m = (hi == VOX_H_HIGH && leaf == 4) ? 'T'
                               : (leaf == 4) ? 'h'   /* leafy, not all HIGH */
                               : (hi >= VOX_H_MID) ? 'l'  /* tall, not leafy */
                               : '?';
                        fprintf(stderr, "%c", m);
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
        }
    }

    gb_platform_shutdown();
    return 0;
}
