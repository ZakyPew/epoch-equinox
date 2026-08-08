/* Oracles game runner.
 *
 * This binary only runs carts. Game selection, cover art, mod toggles and
 * settings live in the separate launcher app (see launcher/), the way
 * Zelda64Recomp and Ship of Harkinian split them — so the UI can be
 * iterated on without a a multi-minute C++ rebuild, and a crash in the cart
 * can't take the launcher down with it.
 *
 * Contract with the launcher:
 *   --games-json     print the game table as JSON and exit
 *   --game <id>      run that cart
 *   --no-mods        boot the stock ROM, ignoring mods/
 * Everything else is forwarded to the cart's own argument parser.
 */
#define SDL_MAIN_HANDLED
extern "C" {
#include "tlozooa.h"
#if ORACLES_HAVE_SEASONS
#include "tlozoos.h"
#endif
#include "gb_sha256.h"
#include "gb_asset_loader.h"
#include "mod_loader.h"
#if ORACLES_HAVE_VOXEL
#include "voxel/voxel.h"
#endif
}

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform_sdl.h"

extern "C" {
#include "assets_manifest_tlozooa.h"
#if ORACLES_HAVE_SEASONS
#include "assets_manifest_tlozoos.h"
#endif
}

typedef int (*GBRunnerMainFn)(int argc, char* argv[]);

typedef struct {
    const char* id;
    const char* title;
    const char* rom_path;
    GBRunnerMainFn main_fn;
    const char* expected_sha256;
    const uint8_t expected_sha1[20];
    uint32_t rom_size;
    const void* manifest;
    uint32_t manifest_count;
} GBRunnerGame;

static int launch_tlozooa(int argc, char* argv[]) { return tlozooa_main(argc, argv); }

/* SHA-256 of the canonical ROM each cart was recompiled from. */
static const char TLOZOOA_EXPECTED_SHA256[] =
    "0b56b78a9e45452e98c33edd111234931f1e034dc097f6f23082eb8db6055474";

#if ORACLES_HAVE_SEASONS
static int launch_tlozoos(int argc, char* argv[]) { return tlozoos_main(argc, argv); }
static const char TLOZOOS_EXPECTED_SHA256[] =
    "862a51368fb30539279d336b3fe193b43876d2cb15c87a36f5da517804ab3971";
#endif

static GBRunnerGame g_games[] = {
    {"tlozooa", "Oracle of Ages", "roms/tlozooa.gbc",
     launch_tlozooa, TLOZOOA_EXPECTED_SHA256,
     {0x88, 0x03, 0x74, 0xfb, 0x97, 0x8b, 0x18, 0xaf, 0x4a, 0xa5,
      0x29, 0xe2, 0xe3, 0x2f, 0x7f, 0xfb, 0x4d, 0x7d, 0xd2, 0xf4},
     1048576u, TLOZOOA_ASSETS_MANIFEST, TLOZOOA_ASSETS_MANIFEST_COUNT},
#if ORACLES_HAVE_SEASONS
    {"tlozoos", "Oracle of Seasons", "roms/tlozoos.gbc",
     launch_tlozoos, TLOZOOS_EXPECTED_SHA256,
     {0xba, 0x12, 0x68, 0x29, 0x0f, 0xb2, 0xb1, 0xb7, 0x05, 0x05,
      0xd2, 0xd7, 0xb5, 0x82, 0x5f, 0xc8, 0xa4, 0x81, 0x6a, 0x4b},
     1048576u, TLOZOOS_ASSETS_MANIFEST, TLOZOOS_ASSETS_MANIFEST_COUNT},
#endif
};

static const size_t g_game_count = sizeof(g_games) / sizeof(g_games[0]);
static bool g_mods_disabled = false;

static bool game_assets_available(const char* id) {
    if (!id || !*id) return false;
    char path[512];
    struct stat st;

    snprintf(path, sizeof(path), "assets/%s", id);
    if (stat(path, &st) == 0 && (st.st_mode & S_IFDIR)) return true;

    static const char* extensions[] = {".gb", ".gbc", ".sgb"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        snprintf(path, sizeof(path), "roms/%s%s", id, extensions[i]);
        if (stat(path, &st) == 0) return true;
    }
    return false;
}

static bool rom_file_present(const char* rom_path) {
    struct stat st;
    return rom_path && stat(rom_path, &st) == 0;
}

static const GBRunnerGame* find_game_by_id(const char* id) {
    if (!id) return NULL;
    for (size_t i = 0; i < g_game_count; i++) {
        if (strcmp(g_games[i].id, id) == 0) return &g_games[i];
    }
    return NULL;
}

static void print_usage(const char* program) {
    fprintf(stderr,
            "Usage: %s [--game <id>] [--no-mods] [--games-json] [--list-games]\n"
            "       [cart arguments...]\n"
            "\n"
            "Game selection and mod management live in the launcher app;\n"
            "run `python3 launcher/oracles_launcher.py` for the UI.\n",
            program);
}

static void print_games(void) {
    fprintf(stderr, "Games in this build:\n");
    for (size_t i = 0; i < g_game_count; i++) {
        fprintf(stderr, "  %zu. %-18s [%s]%s\n", i + 1, g_games[i].title, g_games[i].id,
                game_assets_available(g_games[i].id) ? "" : "  (missing ROM)");
    }
}

/* The launcher reads this instead of hardcoding a game table, so the two
 * stay in sync from one source. */
static void print_games_json(void) {
    printf("{\"games\":[");
    for (size_t i = 0; i < g_game_count; i++) {
        const GBRunnerGame* g = &g_games[i];
        printf("%s{\"id\":\"%s\",\"title\":\"%s\",\"rom_path\":\"%s\","
               "\"rom_size\":%u,\"sha256\":\"%s\",\"rom_present\":%s,\"ready\":%s}",
               i ? "," : "", g->id, g->title, g->rom_path, g->rom_size,
               g->expected_sha256,
               rom_file_present(g->rom_path) ? "true" : "false",
               game_assets_available(g->id) ? "true" : "false");
    }
    printf("]}\n");
}

static void prepare_mods_for(const GBRunnerGame* game) {
    if (!game) return;
    if (g_mods_disabled) {
        if (gb_mods_restore_stock(game->id)) {
            fprintf(stderr, "[MOD] --no-mods: stock ROM restored for %s\n", game->id);
        }
        return;
    }
    if (!gb_mods_stage_assets(game->id, game->rom_path, game->expected_sha1,
                              game->rom_size, game->manifest, game->manifest_count)) {
        return; /* nothing staged; the cart reports the real error */
    }
    gb_mods_scan(game->id);
    if (gb_mods_count() > 0) {
        gb_mods_apply(game->id, game->rom_size);
    } else {
        gb_mods_restore_stock(game->id);
    }
}

int main(int argc, char* argv[]) {
    const GBRunnerGame* selected = NULL;
    char** forwarded_argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
    int forwarded_argc = 1;
    if (!forwarded_argv) {
        fprintf(stderr, "Failed to allocate runner argument buffer\n");
        return 1;
    }
    forwarded_argv[0] = argv[0];

    /* --games-json reports asset state, which is relative to the binary. */
    if (!gb_chdir_to_exe_dir()) {
        fprintf(stderr, "[RUN] warning: could not chdir to the executable's directory\n");
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--games-json") == 0) {
            print_games_json();
            free(forwarded_argv);
            return 0;
        }
        if (strcmp(argv[i], "--list-games") == 0) {
            print_games();
            free(forwarded_argv);
            return 0;
        }
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            print_usage(argv[0]);
            print_games();
            free(forwarded_argv);
            return 0;
        }
        if (strcmp(argv[i], "--no-mods") == 0) {
            g_mods_disabled = true;
            continue;
        }
#if ORACLES_HAVE_VOXEL
        if (strcmp(argv[i], "--voxel") == 0 && i + 1 < argc) {
            voxel_set_mode((int)strtol(argv[++i], NULL, 10));
            continue;
        }
#endif
        if (strcmp(argv[i], "--game") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --game\n");
                print_usage(argv[0]);
                free(forwarded_argv);
                return 1;
            }
            selected = find_game_by_id(argv[++i]);
            if (!selected) {
                fprintf(stderr, "Unknown game id '%s'\n", argv[i]);
                print_games();
                free(forwarded_argv);
                return 1;
            }
            continue;
        }
        forwarded_argv[forwarded_argc++] = argv[i];
    }
    forwarded_argv[forwarded_argc] = NULL;

    /* Launched bare (double-clicked, say): fall back to the first cart whose
     * ROM is actually there rather than dying with a usage message. */
    if (!selected) {
        for (size_t i = 0; i < g_game_count && !selected; i++) {
            if (game_assets_available(g_games[i].id)) selected = &g_games[i];
        }
    }
    if (!selected) {
        fprintf(stderr, "[RUN] No playable game found. Drop your ROMs in roms/:\n");
        for (size_t i = 0; i < g_game_count; i++) {
            fprintf(stderr, "        %s  ->  %s\n", g_games[i].title, g_games[i].rom_path);
        }
        free(forwarded_argv);
        return 1;
    }

#if ORACLES_HAVE_VOXEL
    voxel_install();
#endif

    for (;;) {
        fprintf(stderr, "[RUN] Starting %s [%s]\n", selected->title, selected->id);
        if (selected->expected_sha256 && selected->rom_path) {
            gb_sha256_verify_file(selected->rom_path, selected->expected_sha256,
                                  selected->rom_path);
        }
        prepare_mods_for(selected);

        /* There is no in-process launcher to go back to any more; the Esc
         * menu shows "Restart Game" instead of "Return to Launcher". */
        gb_platform_set_launcher_return_enabled(false);
        int rc = selected->main_fn(forwarded_argc, forwarded_argv);

        if (gb_platform_consume_restart_requested()) continue;
        free(forwarded_argv);
        return rc;
    }
}
