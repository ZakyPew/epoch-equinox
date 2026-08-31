/* Continue the Legend, end to end and headless.
 *
 * The handoff is the player entering YOUR transfer secret into the
 * OTHER cart by itself: boot, walk the file select to SECRETS, type
 * the code with the same typist the Esc menu uses, accept, and hand
 * over a linked game. This test does the whole journey against the
 * real carts: encode a game secret from the finished Ages save in
 * tests/saves, stage it as a handoff for Seasons, run the machine,
 * and assert the cart itself agrees -- wFileIsLinkedGame set, room
 * live, a linked file where there was none.
 *
 * HANDOFF_PROBE=1 turns it into a state dumper instead: boots and
 * mashes toward the file select printing wFileSelect.mode/mode2,
 * textInputMode, cursorPos and scroll -- how the state values the
 * machine keys on were learned in the first place.
 *
 * Build like the other probes:
 *   cc -O2 -o handoff_test ../tools/handoff_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 *
 * Run from a directory holding roms/ and the staged saves (the test
 * stages its own copies; it never touches a player's file).
 */
#include "gbrt.h"
#include "platform_sdl.h"
#include "epoch_secrets.h"
#include "epoch_handoff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t rd(GBContext* ctx, uint16_t addr) {
    return ctx->wram[addr - 0xC000];
}

static GBContext* boot(const char* rom_path) {
    FILE* f = fopen(rom_path, "rb");
    if (!f) { perror(rom_path); exit(1); }
    fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
    uint8_t* rom = (uint8_t*)malloc((size_t)size);
    if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) exit(1);
    fclose(f);
    GBConfig cfg; memset(&cfg, 0, sizeof(cfg));
    bool cgb = (rom[0x143] & 0x80) != 0;
    cfg.model = cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    cfg.cartridge_supports_cgb = cgb;
    cfg.cartridge_requires_cgb = (rom[0x143] == 0xC0);
    cfg.enable_audio = true; cfg.enable_serial = true;
    cfg.speed_percent = 100;
    GBContext* ctx = gb_context_create(&cfg);
    if (!gb_platform_init(1)) exit(1);
    gb_platform_register_context(ctx);
    gb_context_load_rom(ctx, rom, (size_t)size);
    ctx->mbc_type = rom[0x147];
    gb_context_reset(ctx, true);
    return ctx;
}

static void step(GBContext* ctx) {
    gb_reset_frame(ctx);
    ctx->stopped = 0;
    while (!ctx->frame_done) {
        gb_run_cycles(ctx, 0xFFFFFFFFu);
        if (!gb_platform_poll_events(ctx)) exit(2);
    }
}

int main(void) {
    if (getenv("HANDOFF_PROBE")) {
        GBContext* ctx = boot("roms/tlozoos.gbc");
        gb_platform_set_input_script(
            "650:S:10,1050:S:10,1150:D:8,1210:A:10,1400:D:8,1460:A:10");
        for (unsigned long i = 0; i <= 2200; i++) {
            step(ctx);
            if (i % 30 == 0 || (i > 1100 && i % 10 == 0)) {
                fprintf(stderr,
                        "[probe] f=%lu mode=%02x mode2=%02x tim=%02x "
                        "cur=%02x scroll=%02x\n",
                        i, rd(ctx, 0xCBB3), rd(ctx, 0xCBB4),
                        rd(ctx, 0xCBB7), rd(ctx, 0xCBBC), rd(ctx, 0xCD00));
            }
        }
        return 0;
    }

    int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

    /* The transfer secret, from the real endgame Ages save -- read
     * straight from tests/saves, nothing staged for the source side. */
    FILE* f = fopen("../tests/saves/ages-veran-tower.sav", "rb");
    CHECK(f != NULL, "the Ages test save is readable");
    if (!f) return 1;
    uint8_t sav[0x2000];
    if (fread(sav, 1, sizeof(sav), f) != sizeof(sav)) {
        fclose(f);
        printf("FAIL short save\n");
        return 1;
    }
    fclose(f);

    /* The cart will WRITE the linked file into the Seasons save beside
     * this binary: park whatever is there and stage the test copy, so
     * a player's real file is never the canvas. */
    remove("ZELDA DIN.sav.handoff-test-parked");
    rename("ZELDA DIN.sav", "ZELDA DIN.sav.handoff-test-parked");
    {
        FILE* src = fopen("../tests/saves/seasons-room-of-rites.sav", "rb");
        FILE* dst = fopen("ZELDA DIN.sav", "wb");
        CHECK(src && dst, "the Seasons test save stages");
        if (src && dst) {
            uint8_t buf[0x2000];
            size_t got = fread(buf, 1, sizeof(buf), src);
            fwrite(buf, 1, got, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
    }
    /* Slot 0's c6 block sits at file offset 0x010 + 0x50. */
    uint8_t cells[ES_MAX_SYMBOLS];
    int n = es_encode(sav + 0x010 + 0x50, ES_TYPE_GAME, 0, cells);
    CHECK(n == 20, "the game secret encodes to 20 symbols");

    /* Stage the handoff the launcher would write. */
    remove("states/handoff.txt");
#ifdef _WIN32
    system("mkdir states 2>nul");
#else
    system("mkdir -p states");
#endif
    FILE* h = fopen("states/handoff.txt", "w");
    fprintf(h, "to=tlozoos\nslot=1\nname=Link\nsymbols=");
    for (int i = 0; i < n; i++) fprintf(h, "%02x", cells[i]);
    fprintf(h, "\n");
    fclose(h);

    GBContext* ctx = boot("roms/tlozoos.gbc");
    epoch_handoff_arm("tlozoos");
    CHECK(epoch_handoff_active(), "the handoff arms from the file");

    bool linked = false, in_game = false;
    unsigned long frame = 0;
    for (; frame < 30000; frame++) {
        step(ctx);
        epoch_handoff_tick(ctx, "tlozoos");
        epoch_secrets_tick(ctx, "tlozoos");
        if (!epoch_handoff_active()) {
            /* Machine finished or aborted; give the game a moment and
             * read the verdict. */
            for (int j = 0; j < 120; j++) step(ctx);
            in_game = rd(ctx, 0xCD00) == 0x01;
            linked = rd(ctx, 0xC612) != 0;   /* wFileIsLinkedGame */
            break;
        }
    }
    fprintf(stderr, "[handoff] finished at frame %lu: %s\n", frame,
            epoch_handoff_message());
    CHECK(!epoch_handoff_active(), "the machine came to rest");
    CHECK(in_game, "a room is live when it rests");
    CHECK(linked, "and the cart itself says the file is LINKED");
    CHECK(fopen("states/handoff.txt", "r") == NULL,
          "the handoff file is consumed on success");

    /* Put the parked Seasons save back, whatever happened. */
    remove("ZELDA DIN.sav");
    rename("ZELDA DIN.sav.handoff-test-parked", "ZELDA DIN.sav");

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
