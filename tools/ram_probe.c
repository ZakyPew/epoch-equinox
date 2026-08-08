/* Verify that the disassembly's WRAM symbols hold what they claim.
 *
 * Before rebuilding the voxel classifier on the game's own terrain data,
 * check the data is actually there and actually looks like a room. This
 * dumps the buffers oracles-disasm names, at a frame of your choosing, and
 * prints them as a grid.
 *
 * WRAM layout in gbrt: ctx->wram is 8 banks x 4 KiB.
 *   $C000-$CFFF  -> wram[addr - 0xC000]        (bank 0, unbanked)
 *   $D000-$DFFF  -> wram[bank * 4096 + (addr - 0xD000)]
 *
 * So the banked symbols need their bank named explicitly: w1Link is bank 1,
 * w3TileCollisions is bank 3.
 *
 *   ./ram_probe <rom> <frames>
 */
#include "gbrt.h"
#include "platform_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRAM_BANK 4096

/* From Stewmath/oracles-disasm include/wram.s */
#define A_wRoomCollisions   0xCE00  /* $c0 bytes, bank 0 */
#define A_wRoomLayout       0xCF00  /* $c0 bytes, bank 0 */
#define A_w1Link            0xD000  /* SpecialObjectStruct, bank 1 */
#define A_w3TileCollisions  0xDB00  /* $100 bytes, bank 3 */

/* SpecialObjectStruct offsets */
#define O_direction 0x08
#define O_angle     0x09
#define O_y         0x0A
#define O_yh        0x0B
#define O_x         0x0C
#define O_xh        0x0D
#define O_z         0x0E
#define O_zh        0x0F

static uint8_t lo(GBContext* ctx, uint16_t addr) {          /* bank 0 */
    return ctx->wram[addr - 0xC000];
}
static uint8_t banked(GBContext* ctx, int bank, uint16_t addr) {
    return ctx->wram[bank * WRAM_BANK + (addr - 0xD000)];
}

int main(int argc, char* argv[]) {
    if (argc < 3) { fprintf(stderr, "usage: %s <rom> <frames>\n", argv[0]); return 2; }
    unsigned long frames = strtoul(argv[2], NULL, 10);

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
    gb_context_load_rom(ctx, rom, (size_t)size);
    ctx->mbc_type = rom[0x147];
    gb_context_reset(ctx, true);

    for (unsigned long i = 0; i < frames; i++) {
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        while (!ctx->frame_done) {
            gb_run_cycles(ctx, 0xFFFFFFFFu);
            if (!gb_platform_poll_events(ctx)) { i = frames; break; }
        }
        const uint32_t* fb = gb_get_framebuffer(ctx);
        if (fb) gb_platform_render_frame(fb);
    }

    printf("\n=== w1Link ($D000, bank 1) ===\n");
    printf("  pos    Y=%3u.%02u  X=%3u.%02u  Z=%3u.%02u\n",
           banked(ctx, 1, A_w1Link + O_yh), banked(ctx, 1, A_w1Link + O_y),
           banked(ctx, 1, A_w1Link + O_xh), banked(ctx, 1, A_w1Link + O_x),
           banked(ctx, 1, A_w1Link + O_zh), banked(ctx, 1, A_w1Link + O_z));
    printf("  facing dir=%u angle=%u\n",
           banked(ctx, 1, A_w1Link + O_direction), banked(ctx, 1, A_w1Link + O_angle));

    /* $c0 bytes = 192 = 16 wide x 12 tall, the Oracles room grid. */
    printf("\n=== wRoomLayout ($CF00) -- tile index per cell ===\n");
    for (int row = 0; row < 12; row++) {
        printf("  ");
        for (int col = 0; col < 16; col++) printf("%02X ", lo(ctx, A_wRoomLayout + row * 16 + col));
        printf("\n");
    }

    printf("\n=== wRoomCollisions ($CE00) -- collision per cell ===\n");
    for (int row = 0; row < 12; row++) {
        printf("  ");
        for (int col = 0; col < 16; col++) printf("%02X ", lo(ctx, A_wRoomCollisions + row * 16 + col));
        printf("\n");
    }

    /* Sanity: an all-zero or all-identical buffer means the address is wrong
     * or the game has not populated it yet. */
    int layout_distinct = 0, coll_distinct = 0;
    int seen_l[256] = {0}, seen_c[256] = {0};
    for (int i = 0; i < 192; i++) {
        uint8_t l = lo(ctx, A_wRoomLayout + i), c = lo(ctx, A_wRoomCollisions + i);
        if (!seen_l[l]) { seen_l[l] = 1; layout_distinct++; }
        if (!seen_c[c]) { seen_c[c] = 1; coll_distinct++; }
    }
    printf("\n=== sanity ===\n");
    printf("  distinct layout values:    %d %s\n", layout_distinct,
           layout_distinct > 1 ? "(populated)" : "(EMPTY - wrong address or not loaded)");
    printf("  distinct collision values: %d %s\n", coll_distinct,
           coll_distinct > 1 ? "(populated)" : "(EMPTY - wrong address or not loaded)");

    int nonzero_tilecoll = 0;
    for (int i = 0; i < 256; i++) if (banked(ctx, 3, A_w3TileCollisions + i)) nonzero_tilecoll++;
    printf("  w3TileCollisions nonzero:  %d/256 %s\n", nonzero_tilecoll,
           nonzero_tilecoll ? "(populated)" : "(EMPTY)");

    gb_platform_shutdown();
    return 0;
}
