/* Continue the Legend, end to end and headless.
 *
 * The handoff is the player entering YOUR transfer secret into the
 * OTHER cart by itself: boot, walk the file select to SECRETS, type
 * the code with the same typist the Esc menu uses, accept, and hand
 * over a linked game. This test does the whole journey against the
 * real carts: encode a game secret from a finished save in
 * tests/saves, stage it as a handoff for the other cart, run the
 * machine, and assert the cart itself agrees -- the linked (or hero)
 * flag set, the hero's name carried across, a room live.
 *
 * Scenarios, chosen by environment:
 *   HANDOFF_TO=tlozoos|tlozooa  which cart receives (default tlozoos)
 *   HANDOFF_SLOT=0..2           the file to link into (default 1)
 *   HANDOFF_FRESH=1             no save at all on the receiving side
 *   HANDOFF_EXPECT=link|stop|takeover
 *       link      a linked game standing in a room (default)
 *       stop      the slot is really occupied: the machine must notice
 *                 the game that started instead and stop at once
 *       takeover  a key still held from the launcher is ignored, a
 *                 fresh press later stops the machine
 *
 * HANDOFF_PROBE=1 turns it into a state dumper instead: boots and
 * mashes toward the file select printing wFileSelect.mode/mode2,
 * textInputMode, cursorPos, scroll and the frame's cycle count --
 * how the state values the machine keys on were learned.
 *
 * Build like the other probes:
 *   cc -O2 -o handoff_test ../tools/handoff_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 *
 * Run from a directory holding roms/. The receiving cart's save is
 * parked and restored around the run; a player's file is never the
 * canvas.
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

static bool read_file(const char* path, uint8_t* buf, size_t n) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
}

static bool write_file(const char* path, const uint8_t* buf, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(buf, 1, n, f);
    fclose(f);
    return put == n;
}

int main(void) {
    if (getenv("HANDOFF_PROBE")) {
        const char* pto = getenv("HANDOFF_TO") ? getenv("HANDOFF_TO") : "tlozoos";
        GBContext* ctx = boot(strcmp(pto, "tlozooa") == 0 ? "roms/tlozooa.gbc"
                                                         : "roms/tlozoos.gbc");
        gb_platform_set_input_script(
            "650:S:10,1050:S:10,1150:D:8,1210:A:10,1400:D:8,1460:A:10");
        for (unsigned long i = 0; i <= 2200; i++) {
            step(ctx);
            if (i % 30 == 0 || (i > 1100 && i % 10 == 0)) {
                fprintf(stderr,
                        "[probe] f=%lu mode=%02x mode2=%02x tim=%02x "
                        "cur=%02x scroll=%02x cycles=%u key1=%02x\n",
                        i, rd(ctx, 0xCBB3), rd(ctx, 0xCBB4),
                        rd(ctx, 0xCBB7), rd(ctx, 0xCBBC), rd(ctx, 0xCD00),
                        ctx->frame_cycles, ctx->io[0x4D]);
            }
        }
        return 0;
    }

    /* ---- the scenario ------------------------------------------------ */
    const char* to = getenv("HANDOFF_TO") ? getenv("HANDOFF_TO") : "tlozoos";
    const bool to_seasons = strcmp(to, "tlozoos") == 0;
    const int slot = getenv("HANDOFF_SLOT") ? atoi(getenv("HANDOFF_SLOT")) : 1;
    const bool fresh = getenv("HANDOFF_FRESH") != NULL;
    const char* expect = getenv("HANDOFF_EXPECT") ? getenv("HANDOFF_EXPECT")
                                                  : "link";
    const char* rom      = to_seasons ? "roms/tlozoos.gbc" : "roms/tlozooa.gbc";
    const char* src_path = to_seasons ? "../tests/saves/ages-veran-tower.sav"
                                      : "../tests/saves/seasons-room-of-rites.sav";
    const char* tgt_path = to_seasons ? "../tests/saves/seasons-room-of-rites.sav"
                                      : "../tests/saves/ages-veran-tower.sav";
    const char* tgt_sav  = to_seasons ? "ZELDA DIN.sav" : "ZELDA NAYRUAZ8E.sav";
    char parked[64];
    snprintf(parked, sizeof(parked), "%s.handoff-test-parked", tgt_sav);
    fprintf(stderr, "[handoff-test] to=%s slot=%d fresh=%d expect=%s\n",
            to, slot, fresh, expect);

    int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

    /* The transfer secret, from the finished save on the sending side
     * -- read straight from tests/saves, nothing staged for it. */
    uint8_t sav[0x2000];
    CHECK(read_file(src_path, sav, sizeof(sav)), "the sending save is readable");
    if (failures) return 1;
    /* Slot 0's c6 block sits at file offset 0x010 + 0x50. */
    const uint8_t* src_c6 = sav + 0x010 + 0x50;
    uint8_t cells[ES_MAX_SYMBOLS];
    int n = es_encode(src_c6, ES_TYPE_GAME, 0, cells);
    CHECK(n == 20, "the game secret encodes to 20 symbols");

    /* The cart will WRITE the linked file into the receiving save
     * beside this binary: park whatever is there, then stage the test
     * copy -- or nothing at all, for a cart that has never been saved. */
    remove(parked);
    rename(tgt_sav, parked);
    if (!fresh) {
        uint8_t buf[0x2000];
        bool ok = read_file(tgt_path, buf, sizeof(buf)) &&
                  write_file(tgt_sav, buf, sizeof(buf));
        CHECK(ok, "the receiving save stages");
    } else {
        CHECK(fopen(tgt_sav, "rb") == NULL, "the receiving side has no save");
    }

    /* Stage the handoff the launcher would write. */
    remove("states/handoff.txt");
#ifdef _WIN32
    if (system("mkdir states 2>nul")) {}
#else
    if (system("mkdir -p states")) {}
#endif
    FILE* h = fopen("states/handoff.txt", "w");
    fprintf(h, "to=%s\nslot=%d\nname=Link\nsymbols=", to, slot);
    for (int i = 0; i < n; i++) fprintf(h, "%02x", cells[i]);
    fprintf(h, "\n");
    fclose(h);

    GBContext* ctx = boot(rom);
    epoch_handoff_arm(to);
    CHECK(epoch_handoff_active(), "the handoff arms from the file");
    CHECK(fopen("states/handoff.txt", "r") == NULL,
          "the handoff file is consumed the moment it is read");

    const bool takeover = strcmp(expect, "takeover") == 0;
    bool in_game = false, linked = false, hero = false, name_ok = false;
    bool still_armed_past_held_key = false;
    unsigned live_gid = 0xFFFF, live_playtime = 0xFFFFFFFFu;
    /* The secret carries the sending file's Game ID; the file the cart
     * creates from it inherits that ID, and the staged neighbour file
     * has a different one -- so the loaded file's ID says whether the
     * NEW file started, not merely some file. Playtime backs it up:
     * a file born seconds ago has none. */
    const unsigned src_gid = src_c6[0x00] | ((src_c6[0x01] & 0x7F) << 8);
    unsigned long frame = 0;
    for (; frame < 30000; frame++) {
        step(ctx);
        if (takeover) {
            /* A key held down since before the cart booted -- the one
             * that confirmed the launcher's dialog -- and one real
             * press much later. */
            if (frame < 300) g_joypad_buttons = (uint8_t)~0x08;   /* Start */
            if (frame == 400) still_armed_past_held_key = epoch_handoff_active();
            if (frame == 1300) g_joypad_buttons = (uint8_t)~0x01; /* A */
        }
        epoch_handoff_tick(ctx, to);
        epoch_secrets_tick(ctx, to);
        if (getenv("HANDOFF_TRACE") && frame >= 1100 && frame % 10 == 0) {
            int done = 0, total = 0;
            epoch_secrets_status(&done, &total, NULL);
            fprintf(stderr,
                    "[trace] f=%lu mode=%02x mode2=%02x tim=%02x cur=%02x "
                    "scroll=%02x typed=%d/%d msg=%s\n",
                    frame, rd(ctx, 0xCBB3), rd(ctx, 0xCBB4), rd(ctx, 0xCBB7),
                    rd(ctx, 0xCBBC), rd(ctx, 0xCD00), done, total,
                    epoch_handoff_message());
        }
        if (!epoch_handoff_active()) {
            /* Machine finished or aborted; give the game a moment and
             * read the verdict. */
            for (int j = 0; j < 120; j++) step(ctx);
            in_game = rd(ctx, 0xCD00) == 0x01;
            linked = rd(ctx, 0xC612) != 0;   /* wFileIsLinkedGame */
            hero   = rd(ctx, 0xC613) != 0;   /* wFileIsHeroGame   */
            name_ok = memcmp(&ctx->wram[0xC602 - 0xC000], src_c6 + 0x02, 6) == 0;
            live_gid = rd(ctx, 0xC600) | ((rd(ctx, 0xC601) & 0x7F) << 8);
            live_playtime = rd(ctx, 0xC622) | (rd(ctx, 0xC623) << 8) |
                            (rd(ctx, 0xC624) << 16) | ((unsigned)rd(ctx, 0xC625) << 24);
            break;
        }
    }
    const char* message = epoch_handoff_message();
    fprintf(stderr, "[handoff] finished at frame %lu (outcome %d): %s\n",
            frame, epoch_handoff_outcome(), message ? message : "(silent)");
    CHECK(!epoch_handoff_active(), "the machine came to rest");
    CHECK(message != NULL, "and left a verdict on screen");

    if (strcmp(expect, "link") == 0) {
        CHECK(epoch_handoff_outcome() == 1, "it reports success");
        CHECK(in_game, "a room is live when it rests");
        CHECK(linked || hero, "and the cart itself says the file is LINKED");
        fprintf(stderr, "[handoff] cart flags: linked=%d hero=%d gid=%04x "
                        "(sender %04x) playtime=%u frames\n",
                linked, hero, live_gid, src_gid, live_playtime);
        CHECK(name_ok, "the hero's name crossed over intact");
        CHECK(live_gid == src_gid, "the file that started carries the sender's Game ID");
        CHECK(live_playtime < 60u * 60u * 5u, "and it is brand new, not a neighbour");
    } else if (strcmp(expect, "stop") == 0) {
        CHECK(epoch_handoff_outcome() == -1, "it reports that it stopped");
        CHECK(message && strstr(message, "not empty") != NULL,
              "because the file was not empty");
        CHECK(in_game, "leaving the player's own game on screen");
        CHECK(frame < 2000, "and it noticed within seconds");
    } else if (takeover) {
        CHECK(still_armed_past_held_key,
              "a key held since the launcher does not count as a takeover");
        CHECK(epoch_handoff_outcome() == -1, "a real press later stops it");
        CHECK(message && strstr(message, "took over") != NULL,
              "and says so");
        CHECK(frame >= 1300 && frame < 1310, "on that very frame");
    }

    /* The verdict lingers, then goes away by itself. */
    for (int j = 0; j < 400; j++) { step(ctx); epoch_handoff_tick(ctx, to); }
    CHECK(epoch_handoff_message() == NULL, "the verdict clears after a while");

    /* Put the parked receiving save back, whatever happened. */
    remove(tgt_sav);
    rename(parked, tgt_sav);

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
