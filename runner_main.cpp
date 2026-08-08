/* Oracles game runner.
 *
 * This binary only runs carts. Game selection, cover art, mod toggles and
 * settings live in the separate launcher app (see launcher/), the way
 * Zelda64Recomp and Ship of Harkinian split them -- so the UI can be iterated
 * on without a multi-minute rebuild, and a crash in the cart can't take the
 * launcher down with it.
 *
 * The cart table isn't written here. cmake/GenerateCarts.cmake emits
 * oracles_games.h describing exactly the ROMs that were recompiled, and this
 * file expands it. Drop a different ROM in roms/, reconfigure, and it appears
 * with no C to edit.
 *
 * Contract with the launcher:
 *   --games-json     print the game table as JSON and exit
 *   --game <id>      run that cart
 *   --no-mods        boot the stock ROM, ignoring mods/
 *   --voxel <n>      start with the diorama renderer on (0 = off, the default)
 * Everything else is forwarded to the cart's own argument parser.
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
#include "gb_asset_loader.h"
#include "mod_loader.h"
#if ORACLES_HAVE_VOXEL
#include "voxel/voxel.h"
#endif
}

#include "platform_sdl.h"
#include "oracles_games.h"

/* Each generated cart exposes <id>_main() and its own manifest header. */
#define ORACLES_GAME(sym, id, title, rom, size, sha256, sha1) \
    extern "C" int sym##_main(int argc, char* argv[]);
ORACLES_GAME_LIST
#undef ORACLES_GAME

typedef int (*GBRunnerMainFn)(int argc, char* argv[]);

typedef struct {
    const char* id;
    const char* title;
    const char* rom_path;
    GBRunnerMainFn main_fn;
    const char* expected_sha256;   /* lowercase hex */
    const char* expected_sha1;     /* lowercase hex */
    uint32_t rom_size;
} GBRunnerGame;

/* The generated table carries hashes as hex strings (a braced byte array
 * would break X-macro expansion -- the preprocessor reads each byte as a
 * separate argument). The asset loader wants raw bytes, so decode here. */
static bool hex_to_bytes(const char* hex, uint8_t* out, size_t out_len) {
    if (!hex || strlen(hex) != out_len * 2) return false;
    for (size_t i = 0; i < out_len; i++) {
        unsigned byte = 0;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
        out[i] = (uint8_t)byte;
    }
    return true;
}

static GBRunnerGame g_games[] = {
#define ORACLES_GAME(sym, id, title, rom, size, sha256, sha1) \
    {id, title, rom, sym##_main, sha256, sha1, size},
    ORACLES_GAME_LIST
#undef ORACLES_GAME
};

static const size_t g_game_count = sizeof(g_games) / sizeof(g_games[0]);
static bool g_mods_disabled = false;

/* The asset manifest is per-cart and lives in that cart's generated dir. All
 * of them are single-section (whole ROM as rom.bin), so the runner can stage
 * with a locally described entry rather than pulling in every header. */
typedef struct {
    uint32_t rom_offset;
    uint32_t size;
    const char* path;
} RunnerAssetEntry;

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
            "Usage: %s [--game <id>] [--no-mods] [--voxel <n>] [--games-json]\n"
            "       [--list-games] [cart arguments...]\n"
            "\n"
            "Game selection and mod management live in the launcher app;\n"
            "run `python3 launcher/oracles_launcher.py` for the UI.\n",
            program);
}

static void print_games(void) {
    fprintf(stderr, "Games in this build:\n");
    for (size_t i = 0; i < g_game_count; i++) {
        fprintf(stderr, "  %zu. %-20s [%s]%s\n", i + 1, g_games[i].title, g_games[i].id,
                game_assets_available(g_games[i].id) ? "" : "  (missing ROM)");
    }
}

/* The launcher reads this instead of hardcoding a game table. */
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

    static const RunnerAssetEntry whole_rom = {0u, 0u, "rom.bin"};
    RunnerAssetEntry manifest = whole_rom;
    manifest.size = game->rom_size;

    uint8_t sha1[20];
    if (!hex_to_bytes(game->expected_sha1, sha1, sizeof(sha1))) {
        fprintf(stderr, "[MOD] %s: bad SHA-1 in the generated game table\n", game->id);
        return;
    }

    if (!gb_mods_stage_assets(game->id, game->rom_path, sha1,
                              game->rom_size, &manifest, 1)) {
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
            fprintf(stderr, "        %-20s -> %s\n", g_games[i].title, g_games[i].rom_path);
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

        /* There is no in-process launcher to go back to, so the Esc menu
         * shows "Restart Game" instead of "Return to Launcher". */
        gb_platform_set_launcher_return_enabled(false);
        int rc = selected->main_fn(forwarded_argc, forwarded_argv);

        if (gb_platform_consume_restart_requested()) continue;
        free(forwarded_argv);
        return rc;
    }
}
