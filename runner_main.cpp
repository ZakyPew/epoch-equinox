/* Epoch & Equinox game runner.
 *
 * A native player for Game Boy / Game Boy Color ROMs, tuned for the two
 * Oracle games. There is no build-time game code here and none linked in:
 * the ROM is read from disk at startup, mods are applied to it in memory,
 * and the gbrt runtime executes it. That is why this binary can be
 * distributed prebuilt -- it contains nothing derived from any game.
 *
 * (Historical note, because the repository's shape only makes sense with
 * it: this project once compiled each ROM into ~170 MB of generated C and
 * linked it in. tools/interp_probe.c demonstrated that the generated code
 * was never executed -- the runtime's dispatch interprets straight from
 * the loaded ROM image, byte-identically. The generation step is gone;
 * this file is what remains.)
 *
 * Games are whatever sits in roms/ next to the binary. The two Oracles
 * ids get titles and hash verification; anything else runs under its
 * filename.
 *
 * Contract with the launcher (launcher/epoch_launcher.py):
 *   --games-json     print the game table as JSON and exit
 *   --game <id>      run that game
 *   --no-mods        boot the stock ROM, ignoring mods/
 *   --voxel <n>      start with the diorama renderer on (0 = off)
 */
#define SDL_MAIN_HANDLED

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern "C" {
#include "gbrt.h"
#include "gb_sha256.h"
#include "mod_loader.h"
#include "platform_compat.h"
#include "epoch_rewind.h"
#include "epoch_achievements.h"
#include "epoch_splits.h"
#include "epoch_stream.h"
#include "epoch_secrets.h"
#include "epoch_panel.h"
#include "epoch_overlay.h"
#if EPOCH_HAVE_VOXEL
#include "voxel/voxel.h"
#endif
}

#include "platform_sdl.h"

#define EPOCH_MAX_GAMES 32

typedef struct {
    char id[64];            /* rom filename without extension */
    char title[96];
    char rom_path[512];
    const char* expected_sha256;   /* NULL for unknown carts */
} EpochGame;

/* The carts this player is about: pretty titles and the revision hashes
 * verification warns against. Everything else in roms/ still runs. */
typedef struct {
    const char* id;
    const char* title;
    const char* sha256;
    const char* header_title;   /* cartridge header at $134 */
} EpochKnownGame;

static const EpochKnownGame KNOWN[] = {
    {"tlozooa", "Oracle of Ages",
     "0b56b78a9e45452e98c33edd111234931f1e034dc097f6f23082eb8db6055474",
     "ZELDA NAYRU"},
    {"tlozoos", "Oracle of Seasons",
     "862a51368fb30539279d336b3fe193b43876d2cb15c87a36f5da517804ab3971",
     "ZELDA DIN"},
};

static EpochGame g_games[EPOCH_MAX_GAMES];
static size_t g_game_count = 0;
static bool g_mods_disabled = false;

static const EpochKnownGame* known_by_id(const char* id) {
    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
        if (strcmp(KNOWN[i].id, id) == 0) return &KNOWN[i];
    }
    return NULL;
}

/* Identify a cart by the title its own header carries at $134. This is
 * what makes "name the file whatever you like" true: a No-Intro-named
 * dump is still Oracle of Seasons, and treating it as an anonymous ROM
 * cost it its proper title and its cover art in the launcher. */
static const EpochKnownGame* known_by_header(const char* rom_path) {
    FILE* f = fopen(rom_path, "rb");
    if (!f) return NULL;
    uint8_t header[0x150];
    size_t got = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (got < sizeof(header)) return NULL;
    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
        if (!KNOWN[i].header_title) continue;
        size_t n = strlen(KNOWN[i].header_title);
        if (n <= 16 && memcmp(header + 0x134, KNOWN[i].header_title, n) == 0) {
            return &KNOWN[i];
        }
    }
    return NULL;
}

static bool has_rom_ext(const char* name, const char** ext_out) {
    static const char* exts[] = {".gbc", ".gb", ".sgb"};
    size_t n = strlen(name);
    for (size_t i = 0; i < 3; i++) {
        size_t e = strlen(exts[i]);
        if (n > e && strcmp(name + n - e, exts[i]) == 0) {
            if (ext_out) *ext_out = exts[i];
            return true;
        }
    }
    return false;
}

/* Populate g_games from roms/. The two known ids are always listed --
 * present or not -- so the launcher can offer "Install ROM" for them. */
static void scan_games(void) {
    g_game_count = 0;

    bool have_known[sizeof(KNOWN) / sizeof(KNOWN[0])] = {false};

    EpochDir* d = epoch_dir_open("roms");
    const char* name;
    while (d && (name = epoch_dir_next(d)) != NULL &&
           g_game_count < EPOCH_MAX_GAMES) {
        const char* ext = NULL;
        if (!has_rom_ext(name, &ext)) continue;

        EpochGame* g = &g_games[g_game_count];
        size_t stem = strlen(name) - strlen(ext);
        if (stem >= sizeof(g->id)) stem = sizeof(g->id) - 1;
        memcpy(g->id, name, stem);
        g->id[stem] = '\0';
        snprintf(g->rom_path, sizeof(g->rom_path), "roms/%s", name);

        const EpochKnownGame* k = known_by_id(g->id);
        if (!k) k = known_by_header(g->rom_path);
        /* First file claiming a known cart wins its identity; a second
         * copy stays a plain ROM entry instead of a duplicate id. */
        if (k) {
            for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
                if (&KNOWN[i] == k && have_known[i]) k = NULL;
            }
        }
        if (k) {
            snprintf(g->id, sizeof(g->id), "%s", k->id);
            snprintf(g->title, sizeof(g->title), "%s", k->title);
            g->expected_sha256 = k->sha256;
            for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
                if (&KNOWN[i] == k) have_known[i] = true;
            }
        } else {
            snprintf(g->title, sizeof(g->title), "%s", g->id);
            g->expected_sha256 = NULL;
        }
        g_game_count++;
    }
    if (d) epoch_dir_close(d);

    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]) &&
                       g_game_count < EPOCH_MAX_GAMES; i++) {
        if (have_known[i]) continue;
        EpochGame* g = &g_games[g_game_count++];
        snprintf(g->id, sizeof(g->id), "%s", KNOWN[i].id);
        snprintf(g->title, sizeof(g->title), "%s", KNOWN[i].title);
        snprintf(g->rom_path, sizeof(g->rom_path), "roms/%s.gbc", KNOWN[i].id);
        g->expected_sha256 = KNOWN[i].sha256;
    }
}

static bool rom_present(const EpochGame* g) {
    struct stat st;
    return stat(g->rom_path, &st) == 0;
}

static const EpochGame* find_game(const char* id) {
    for (size_t i = 0; i < g_game_count; i++) {
        if (strcmp(g_games[i].id, id) == 0) return &g_games[i];
    }
    return NULL;
}

static void print_games(void) {
    fprintf(stderr, "Games (roms/ next to the binary):\n");
    for (size_t i = 0; i < g_game_count; i++) {
        fprintf(stderr, "  %-20s [%s]%s\n", g_games[i].title, g_games[i].id,
                rom_present(&g_games[i]) ? "" : "  (missing ROM)");
    }
}

static void print_games_json(void) {
    printf("{\"games\":[");
    for (size_t i = 0; i < g_game_count; i++) {
        const EpochGame* g = &g_games[i];
        struct stat st;
        long size = (stat(g->rom_path, &st) == 0) ? (long)st.st_size : 0;
        bool present = size > 0;
        printf("%s{\"id\":\"%s\",\"title\":\"%s\",\"rom_path\":\"%s\","
               "\"rom_size\":%ld,\"sha256\":\"%s\",\"rom_present\":%s,\"ready\":%s}",
               i ? "," : "", g->id, g->title, g->rom_path, size,
               g->expected_sha256 ? g->expected_sha256 : "",
               present ? "true" : "false", present ? "true" : "false");
    }
    printf("]}\n");
}

static void print_usage(const char* program) {
    fprintf(stderr,
            "Usage: %s [--game <id>] [--no-mods] [--voxel <n>] [--games-json]\n"
            "       [--list-games] [--input <script>] [--limit-frames <n>]\n"
            "       [--dump-frames <list>]\n"
            "\n"
            "Drop ROMs in roms/ next to this binary. The launcher UI is\n"
            "launcher/epoch_launcher.py.\n",
            program);
}

/* ------------------------------------------------------------------ */
/* running one game                                                    */
/* ------------------------------------------------------------------ */

static uint8_t* load_rom_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0) { fclose(f); return NULL; }
    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static int run_game(const EpochGame* game, unsigned long long frame_limit) {
    size_t rom_size = 0;
    uint8_t* rom = load_rom_file(game->rom_path, &rom_size);
    if (!rom) {
        fprintf(stderr, "[RUN] cannot read %s\n", game->rom_path);
        return 1;
    }

    if (game->expected_sha256) {
        gb_sha256_verify_file(game->rom_path, game->expected_sha256, game->rom_path);
    }

    /* Mods: applied to the in-memory image only. Stock on disk, always. */
    if (!g_mods_disabled) {
        gb_mods_scan(game->id);
        if (gb_mods_count() > 0) {
            int n = gb_mods_apply_buffer(rom, (uint32_t)rom_size);
            if (n > 0) {
                char digest[65];
                gb_sha256_hex(rom, rom_size, digest);
                fprintf(stderr, "[MOD] %d mod(s) applied; image sha256=%s\n", n, digest);
            }
        }
    }

    /* Model from the cartridge header, the same policy the generated
     * launchers used: CGB when supported, DMG otherwise. */
    uint8_t cgb_flag = rom_size > 0x143 ? rom[0x143] : 0;
    GBConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model = (cgb_flag & 0x80) ? GB_MODEL_CGB : GB_MODEL_DMG;
    cfg.cgb_compatibility_mode = false;
    cfg.cartridge_supports_cgb = (cgb_flag & 0x80) != 0;
    cfg.cartridge_requires_cgb = (cgb_flag == 0xC0);
    cfg.enable_bootrom = false;
    cfg.enable_audio = true;
    cfg.enable_serial = true;
    cfg.speed_percent = 100;

    GBContext* ctx = gb_context_create(&cfg);
    if (!ctx) {
        fprintf(stderr, "[RUN] gb_context_create failed\n");
        free(rom);
        return 1;
    }

    gb_platform_register_context(ctx);
    gb_platform_set_game_id(ctx, game->id);   /* saves, prefs, cheats key */
    gb_platform_set_title(game->title);

    gb_context_load_rom(ctx, rom, rom_size);
    ctx->mbc_type = rom_size > 0x147 ? rom[0x147] : 0;
    gb_context_reset(ctx, true);

    fprintf(stderr, "[RUN] %s [%s] %zu KiB mbc=%02X model=%s\n",
            game->title, game->id, rom_size / 1024, ctx->mbc_type,
            cfg.model == GB_MODEL_CGB ? "CGB" : "DMG");

    /* The frame loop, matching what the generated launchers did: run in
     * slices so long guest frames stay smooth, pace against the guest
     * clock, and hand every finished frame to the platform (where the
     * voxel hook, shaders and scaling live). */
    const uint32_t slice_cycles_budget = 70224u;
    unsigned long long frame_index = 0;
    bool running = true;
    int exit_code = 0;

    /* Boot boost: the known carts spend their first seconds on licensing
     * splashes. Run the console unpaced through those frames so they flash
     * past in a blink and the title appears immediately. Nothing is
     * patched or skipped -- the game still plays every frame, just not at
     * 60Hz. EPOCH_NO_BOOT_BOOST=1 restores the leisurely original. */
    unsigned long long boost_frames = 0;
    if (game->expected_sha256 && !getenv("EPOCH_NO_BOOT_BOOST")) {
        boost_frames = 160;
    }

    while (running) {
        uint32_t paced_cycles = 0;
        gb_reset_frame(ctx);
        ctx->stopped = 0;

        bool boosting = frame_index < boost_frames;
        while (!ctx->frame_done) {
            bool smooth = gb_platform_get_smooth_lcd_transitions() && !boosting;
            uint32_t budget = smooth ? slice_cycles_budget : 0xFFFFFFFFu;
            uint32_t slice_start = ctx->frame_cycles;
            gb_run_cycles(ctx, budget);
            uint32_t slice_cycles = ctx->frame_cycles - slice_start;

            if (!gb_platform_poll_events(ctx)) {
                running = false;
                break;
            }
#if EPOCH_HAVE_VOXEL
            /* The chase camera can face any direction, so the d-pad's
             * world-fixed east/west stops matching the screen until it is
             * rotated into the camera's frame. */
            voxel_remap_dpad();
#endif
            /* After poll_events, so the injected buttons survive into
             * the frame the cart runs; the runtime rebuilds the joypad
             * globals from the keyboard on every poll. */
            epoch_panel_tick(ctx);
            epoch_secrets_tick(ctx, game->id);
            if (smooth && !ctx->frame_done && slice_cycles >= slice_cycles_budget) {
                if (ctx->lcd_off_active || !(ctx->io[0x40] & 0x80)) {
                    gb_platform_render_lcd_off_frame();
                } else {
                    const uint32_t* slice_fb = gb_get_framebuffer(ctx);
                    if (slice_fb) gb_platform_present_framebuffer(slice_fb);
                }
                gb_platform_vsync(slice_cycles);
                paced_cycles += slice_cycles;
            }
        }
        if (!running) break;

        frame_index++;
        /* Rewind services the frame before it is shown, so a restored
         * state is what gets presented rather than the frame we just
         * ran and threw away. */
        epoch_rewind_tick(ctx, game->id);
        epoch_achievements_tick(ctx, game->id);
        epoch_stream_tick(ctx, game->id);
        epoch_splits_tick(ctx, game->id);
        const uint32_t* fb = gb_get_framebuffer(ctx);
        if (fb) gb_platform_render_frame(fb);

        uint32_t remaining = ctx->frame_cycles > paced_cycles
                                 ? ctx->frame_cycles - paced_cycles : 0;
        if (remaining > 0 && !boosting) gb_platform_vsync(remaining);

        if (frame_limit > 0 && frame_index >= frame_limit) {
            fprintf(stderr, "[LIMIT] Reached frame limit %llu\n", frame_limit);
            break;
        }
    }

    if (gb_platform_get_exit_action() == GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER) {
        exit_code = GB_PLATFORM_RETURN_TO_LAUNCHER_EXIT_CODE;
    }
    gb_context_destroy(ctx);   /* battery RAM save happens in here */
    free(rom);
    return exit_code;
}

/* ------------------------------------------------------------------ */

int main(int argc, char* argv[]) {
    /* roms/, mods/, covers/, saves: all relative to the binary, however it
     * was launched -- double-clicked included. */
    if (!epoch_chdir_to_exe_dir()) {
        fprintf(stderr, "[RUN] warning: could not locate the executable's "
                        "directory; using the current directory\n");
    }

    scan_games();

    const EpochGame* selected = NULL;
    unsigned long long frame_limit = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--games-json") == 0) { print_games_json(); return 0; }
        if (strcmp(argv[i], "--list-games") == 0) { print_games(); return 0; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            print_games();
            return 0;
        }
        if (strcmp(argv[i], "--no-mods") == 0) { g_mods_disabled = true; continue; }
        if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            selected = find_game(argv[++i]);
            if (!selected) {
                fprintf(stderr, "Unknown game id '%s'\n", argv[i]);
                print_games();
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--limit-frames") == 0 && i + 1 < argc) {
            frame_limit = strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            gb_platform_set_input_script(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--dump-frames") == 0 && i + 1 < argc) {
            gb_platform_set_dump_frames(argv[++i]);
            continue;
        }
#if EPOCH_HAVE_VOXEL
        if (strcmp(argv[i], "--voxel") == 0 && i + 1 < argc) {
            voxel_set_mode((int)strtol(argv[++i], NULL, 10));
            continue;
        }
#endif
        fprintf(stderr, "Unknown option '%s'\n", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    if (!selected) {
        for (size_t i = 0; i < g_game_count && !selected; i++) {
            if (rom_present(&g_games[i])) selected = &g_games[i];
        }
    }
    if (!selected || !rom_present(selected)) {
        fprintf(stderr,
                "[RUN] No playable game. Drop your ROMs in roms/ next to this "
                "binary:\n"
                "        roms/tlozooa.gbc   Oracle of Ages    (USA, Australia)\n"
                "        roms/tlozoos.gbc   Oracle of Seasons (USA, Australia)\n");
        return 1;
    }

    /* Windows scales non-DPI-aware windows in the compositor -- a blurry
     * stretch applied AFTER our integer-scaled viewport, which reads as
     * "integer scaling doesn't work" on any display above 100%. Declare
     * per-monitor awareness before SDL creates the window. No effect
     * elsewhere. */
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif

    if (!gb_platform_init(5)) {
        fprintf(stderr, "[RUN] platform init failed\n");
        return 1;
    }
    gb_platform_set_launcher_return_enabled(false);
#if EPOCH_HAVE_VOXEL
    voxel_install();
#endif
    epoch_overlay_install();   /* rewind HUD, toasts, and the Esc-menu pages */

    /* "Restart Game" from the Esc menu loops back around; everything is
     * rebuilt from the stock file, so a restart also re-applies mods
     * fresh. */
    int rc;
    do {
        rc = run_game(selected, frame_limit);
    } while (gb_platform_consume_restart_requested());

    gb_platform_shutdown();
    return rc;
}
