/* Rip the games' own item icons from the player's own ROM, by asking
 * the game to draw them.
 *
 * The route: boot headlessly over the player's save, walk the menus BY
 * STATE (frame-scripted routes drift between runs -- dialog timing and
 * knockback are not deterministic enough for blind frame numbers), and
 * then, for every item in the bag: equip it to the A button, close the
 * menu, and crop the HUD's A box -- a fixed 16x16 screen rect the game
 * itself keeps current -- pairing the pixels with the item id WRAM says
 * is equipped (wInventoryB/A: the two bytes that swap when you equip).
 * The icon labels itself; no sprite-pointer archaeology, no menu-grid
 * geometry.
 *
 * Output is one RGBA PAM per item id, background keyed transparent, in
 * the directory given -- on the machine whose ROM it is. Nothing here
 * ever lands in the repo: icons are the cartridge's art and stay with
 * its owner, same rule as ROMs and saves.
 *
 * Coverage: everything that can sit on a button (13 of Ages' 16 tracker
 * items). Passive treasures -- flippers, the mermaid suit, rings --
 * live on a different subscreen and keep their text fallback.
 *
 * Build like the other probes:
 *   cc -O2 -o icon_rip ../tools/icon_rip.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 *
 * Usage: ./icon_rip <rom> <out_dir>
 */
#include "gbrt.h"
#include "platform_sdl.h"
#include "ppu.h"
#include "voxel/voxel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define ir_mkdir(p) _mkdir(p)
#else
#define ir_mkdir(p) mkdir(p, 0755)
#endif

/* The HUD's A-button box: a 16x16 icon at a fixed screen position. */
#define ABOX_X 48
#define ABOX_Y 0

/* wInventoryB / wInventoryA -- the equipped pair, bag following. The
 * two carts keep the block at different offsets, like everything else
 * in the c6xx save block. Watched live: equipping swaps these bytes. */
typedef struct {
    const char* title;      /* ROM header prefix */
    const char* cart;       /* our cart id, used in file names */
    uint16_t equipped_b, equipped_a;
} RipProfile;

static const RipProfile PROFILES[] = {
    {"ZELDA NAYRU", "tlozooa", 0xC688, 0xC689},
    {"ZELDA DIN",   "tlozoos", 0xC680, 0xC681},
};

/* The B box sits at the left edge of the bar. */
#define BBOX_X 8

static uint8_t rd(GBContext* ctx, uint16_t addr) {
    return ctx->wram[addr - 0xC000];
}

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
}

/* Queue key presses through the runtime's input script, re-armed per
 * decision -- this is what makes the walk state-driven. */
static void press(unsigned long frame, const char* btn) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%lu:%s:8", frame + 2, btn);
    gb_platform_set_input_script(buf);
}

/* Cursor moves, then A, then Start: one queued script per bag visit.
 * Rights and downs together tour the whole grid, so every slot is
 * visited even before item rotation is counted. */
static void nav_equip_close(unsigned long frame, int rights, int downs,
                            const char* equip_btn) {
    char buf[1024];
    size_t off = 0;
    unsigned long at = frame + 2;
    for (int j = 0; j < rights + downs; j++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                "%s%lu:%s:6", off ? "," : "", at,
                                j < rights ? "R" : "D");
        at += 14;
    }
    off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                            "%s%lu:%s:8", off ? "," : "", at, equip_btn);
    at += 30;
    snprintf(buf + off, sizeof(buf) - off, ",%lu:S:8", at);
    gb_platform_set_input_script(buf);
}

/* The HUD icon is not framebuffer pixels: it is OAM sprites layered
 * over the bar (that is also why the count and level digits, which are
 * BG, never contaminate it). Composite every sprite inside the box
 * from VRAM with its real CGB palette -- colour 0 is genuinely
 * transparent, so the icon lands on the overlay's own cell with no
 * background keying at all. Reuses the exact decoder the chase camera
 * draws billboards with. */
static int write_icon_pam(GBContext* ctx, const char* dir,
                          const char* cart, int id, int box_x) {
    uint32_t canvas[16 * 16];
    memset(canvas, 0, sizeof(canvas));
    bool tall = (ctx->io[0x40] & 0x04) != 0;
    int height = tall ? 16 : 8;
    int found = 0;

    /* Later OAM entries draw under earlier ones; walk backwards so the
     * front sprite wins the composite. */
    for (int i = 39; i >= 0; i--) {
        const uint8_t* e = ctx->oam + i * 4;
        int sy = (int)e[0] - 16;
        int sx = (int)e[1] - 8;
        if (sy <= -height || sy >= 16) continue;
        if (sx < box_x - 4 || sx >= box_x + 16) continue;
        VoxSprite s;
        s.y = (int16_t)sy;
        s.x = (int16_t)sx;
        s.tile = tall ? (e[2] & 0xFE) : e[2];
        s.attr = e[3];
        s.tall = tall;
        found++;
        for (int row = 0; row < height; row++) {
            int cy = sy + row - ABOX_Y;
            if (cy < 0 || cy >= 16) continue;
            uint32_t px8[8];
            vox_decode_sprite_row(ctx, &s, row, px8);
            for (int px = 0; px < 8; px++) {
                int cx = sx + px - box_x;
                if (cx < 0 || cx >= 16 || !px8[px]) continue;
                canvas[cy * 16 + cx] = px8[px];
            }
        }
    }
    if (!found) return 0;
    /* A freshly equipped icon blinks; an off-phase capture composites
     * to nothing. Refuse it -- the item stays equipped, stops blinking,
     * and rips clean from whichever box it occupies a pass later. */
    int opaque = 0;
    for (int i = 0; i < 16 * 16; i++) opaque += (canvas[i] != 0);
    if (opaque < 24) return 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%02x.pam", dir, cart, id);
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fprintf(f, "P7\nWIDTH 16\nHEIGHT 16\nDEPTH 4\nMAXVAL 255\n"
               "TUPLTYPE RGB_ALPHA\nENDHDR\n");
    for (int i = 0; i < 16 * 16; i++) {
        uint32_t c = canvas[i];
        uint8_t px[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8),
                          (uint8_t)c, (uint8_t)(c ? 255 : 0) };
        fwrite(px, 1, 4, f);
    }
    fclose(f);
    fprintf(stderr, "[rip] %s\n", path);
    return 1;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <rom> <out_dir>\n", argv[0]);
        return 2;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    uint8_t* rom = (uint8_t*)malloc((size_t)size);
    if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) return 1;
    fclose(f);

    const RipProfile* prof = NULL;
    for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
        if (memcmp(rom + 0x134, PROFILES[i].title,
                   strlen(PROFILES[i].title)) == 0)
            prof = &PROFILES[i];
    }
    if (!prof) {
        fprintf(stderr, "[rip] not an Oracle cart\n");
        return 1;
    }
    ir_mkdir(argv[2]);

    GBConfig cfg; memset(&cfg, 0, sizeof(cfg));
    bool cgb = (rom[0x143] & 0x80) != 0;
    cfg.model = cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    cfg.cartridge_supports_cgb = cgb;
    cfg.cartridge_requires_cgb = (rom[0x143] == 0xC0);
    cfg.enable_audio = true; cfg.enable_serial = true; cfg.speed_percent = 100;

    GBContext* ctx = gb_context_create(&cfg);
    if (!gb_platform_init(1)) return 1;
    gb_platform_register_context(ctx);
    gb_context_load_rom(ctx, rom, (size_t)size);
    ctx->mbc_type = rom[0x147];
    gb_context_reset(ctx, true);

    enum { BOOT, SETTLE, CLEAR, OPEN, VISIT, CAPTURE, DONE } phase = BOOT;
    unsigned long mark = 0;
    int presses = 0;
    int pass = 0, idle_passes = 0;
    bool seen[256];
    memset(seen, 0, sizeof(seen));
    int ripped = 0;

    for (unsigned long i = 0; i < 40000 && phase != DONE; i++) {
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        while (!ctx->frame_done) {
            gb_run_cycles(ctx, 0xFFFFFFFFu);
            if (!gb_platform_poll_events(ctx)) { phase = DONE; break; }
        }
        const uint32_t* fb = gb_get_framebuffer(ctx);
        if (!fb) continue;
        gb_platform_render_frame(fb);

        uint8_t scroll = rd(ctx, 0xCD00);   /* wScrollMode */
        uint8_t text   = rd(ctx, 0xCBA0);   /* wTextIsActive */
        uint8_t menu   = rd(ctx, 0xCBCB);   /* wOpenedMenuType */
        if (getenv("RIP_TRACE")) {
            static int pm = -1, pp = -1;
            if ((int)menu != pm || (int)phase != pp)
                fprintf(stderr, "[st] frame=%lu phase=%d menu=%02x "
                        "text=%d scroll=%02x\n",
                        i, (int)phase, menu, (int)text, scroll);
            pm = menu; pp = (int)phase;
        }

        {   /* RIP_SNAP=<frame>: dump the screen there, for eyeballing
             * a state the trace cannot explain. */
            const char* sn = getenv("RIP_SNAP");
            if (sn && strtoul(sn, NULL, 10) == i) {
                char path[512];
                snprintf(path, sizeof(path), "%s-snap.ppm", argv[2]);
                write_ppm(path, fb, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
            }
        }
        switch (phase) {
        case BOOT:
            /* Anything on the way in -- splash, title, file pick, the
             * message-speed prompt -- advances on Start or A. Alternate
             * them until the game says a room is live. */
            if (scroll == 0x01) {
                fprintf(stderr, "[rip] in-game at frame %lu\n", i);
                phase = SETTLE;
                mark = i;
                break;
            }
            if (i > 400 && (i % 40) == 0)
                press(i, (presses++ & 1) ? "A" : "S");
            break;
        case SETTLE:
            if (i - mark > 90) phase = CLEAR;
            break;
        case CLEAR:
            /* A queued cutscene dialog swallows Start; feed it A until
             * the text engine has been quiet for a second. */
            if (text) {
                mark = i;
                if ((i % 45) == 0) press(i, "A");
            } else if (i - mark > 60) {
                phase = OPEN;
            }
            break;
        case OPEN:
            if (menu != 0) {
                mark = i;
                phase = VISIT;
            } else if ((i % 60) == 0) {
                press(i, "S");
            }
            break;
        case VISIT:
            /* Menu is up. Give the fly-in a moment, then walk the
             * cursor `pass` cells right, equip to A, close. The item
             * ids label themselves on the way out, so the exact cursor
             * path does not need to be right -- every bag slot just
             * needs to be under the cursor on SOME pass. */
            if (i - mark > 120) {
                nav_equip_close(i, pass % 4, (pass / 4) % 4,
                                (pass & 1) ? "B" : "A");
                mark = i;
                phase = CAPTURE;
            }
            break;
        case CAPTURE:
            /* Equipping can pop a panel inside the menu (the seed
             * satchel opens its seed picker), and the description bar
             * keeps wTextIsActive high the whole time the menu is up --
             * it cannot distinguish states. Alternate B (backs out of
             * any panel, harmless at the grid) and Start (closes the
             * menu) until the menu is actually gone. */
            if (menu != 0 && i - mark > 180 && (i % 45) == 0) {
                static int nudge = 0;
                press(i, (nudge++ & 1) ? "S" : "B");
            }
            if (menu == 0 && i - mark > 120) {
                int id_a = rd(ctx, prof->equipped_a);
                int id_b = rd(ctx, prof->equipped_b);
                if (getenv("RIP_TRACE"))
                    fprintf(stderr, "[rip] pass=%d frame=%lu a=%02x "
                            "b=%02x\n", pass, i, id_a, id_b);
                int fresh = 0;
                if (id_a > 0 && !seen[id_a] &&
                    write_icon_pam(ctx, argv[2], prof->cart, id_a, ABOX_X)) {
                    seen[id_a] = true;
                    ripped++; fresh++;
                }
                if (id_b > 0 && !seen[id_b] &&
                    write_icon_pam(ctx, argv[2], prof->cart, id_b, BBOX_X)) {
                    seen[id_b] = true;
                    ripped++; fresh++;
                }
                if (fresh) idle_passes = 0; else idle_passes++;
                pass++;
                /* A full lap of the bag with nothing new means every
                 * equippable item has been seen. */
                if (idle_passes >= 18 || pass > 64) {
                    fprintf(stderr, "[rip] done: %d icon(s)\n", ripped);
                    phase = DONE;
                } else {
                    phase = OPEN;
                }
            }
            break;
        case DONE:
            break;
        }
    }

    gb_platform_shutdown();
    return ripped > 0 ? 0 : 1;
}
