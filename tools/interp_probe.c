/* Cart-free probe: can the runtime play a ROM with no recompiled code?
 *
 * The runtime's gb_dispatch (mock_ir.c) forwards to gb_interpret, and the
 * SDL run loop goes gb_run_cycles -> gb_step -> gb_dispatch. If that path
 * is what actually executes the game, then a binary linking only gbrt --
 * no generated cart at all -- should boot any ROM handed to it.
 *
 * This is the experiment that settles it. Nothing here links a cart.
 *
 *   ./interp_probe <rom> <frames> [dump_frame]
 */
#include "gbrt.h"
#include "platform_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <rom> <frames> [dump_frame]\n", argv[0]);
        return 2;
    }
    const char* rom_path = argv[1];
    unsigned long frames = strtoul(argv[2], NULL, 10);
    long dump_at = (argc > 3) ? strtol(argv[3], NULL, 10) : -1;

    FILE* f = fopen(rom_path, "rb");
    if (!f) { perror(rom_path); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    uint8_t* rom = (uint8_t*)malloc((size_t)size);
    if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "could not read %s\n", rom_path);
        return 1;
    }
    fclose(f);

    /* Model comes from the cartridge header, exactly as a generated
     * *_default_config() would report it: 0x143 bit 7 = CGB support,
     * value 0xC0 = CGB required. */
    uint8_t cgb_flag = rom[0x143];
    bool supports_cgb = (cgb_flag & 0x80) != 0;

    GBConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model = supports_cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    cfg.cgb_compatibility_mode = false;
    cfg.cartridge_supports_cgb = supports_cgb;
    cfg.cartridge_requires_cgb = (cgb_flag == 0xC0);
    cfg.enable_bootrom = false;
    cfg.enable_audio = true;
    cfg.enable_serial = true;
    cfg.speed_percent = 100;

    GBContext* ctx = gb_context_create(&cfg);
    if (!ctx) { fprintf(stderr, "gb_context_create failed\n"); return 1; }

    if (!gb_platform_init(3)) {
        fprintf(stderr, "gb_platform_init failed\n");
        return 1;
    }
    gb_platform_register_context(ctx);

    /* What a generated <id>_init() does, minus the generated part. */
    gb_context_load_rom(ctx, rom, (size_t)size);
    ctx->mbc_type = rom[0x147];
    gb_context_reset(ctx, true);

    fprintf(stderr, "[PROBE] %s: %ld bytes, mbc=%02X, model=%s\n",
            rom_path, size, ctx->mbc_type, supports_cgb ? "CGB" : "DMG");

    for (unsigned long i = 0; i < frames; i++) {
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        while (!ctx->frame_done) {
            gb_run_cycles(ctx, 0xFFFFFFFFu);
            if (!gb_platform_poll_events(ctx)) { frames = i; break; }
        }
        const uint32_t* fb = gb_get_framebuffer(ctx);
        if (fb) gb_platform_render_frame(fb);

        if ((long)i == dump_at && fb) {
            FILE* out = fopen("probe_frame.ppm", "wb");
            if (out) {
                fprintf(out, "P6\n160 144\n255\n");
                for (int p = 0; p < 160 * 144; p++) {
                    uint32_t c = fb[p];
                    fputc((c >> 16) & 0xFF, out);
                    fputc((c >> 8) & 0xFF, out);
                    fputc(c & 0xFF, out);
                }
                fclose(out);
                fprintf(stderr, "[PROBE] wrote probe_frame.ppm at frame %ld\n", dump_at);
            }
        }
    }

    fprintf(stderr, "[PROBE] ran %lu frames, %llu instructions\n",
            frames, (unsigned long long)gbrt_instruction_count);
    gb_platform_shutdown();
    return 0;
}
