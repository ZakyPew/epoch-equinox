/**
 * @file mod_loader.h
 * @brief Mod discovery and ROM patching.
 *
 * The runner loads the stock ROM into memory and hands the buffer here;
 * enabled mods are applied in place before the emulator ever sees it. The
 * file on disk is never modified, so disabling a mod needs no undo -- the
 * next launch simply reloads stock. Two runs with the same mod set are
 * byte-identical by construction.
 *
 * Mod layout, one directory per mod under `mods/`:
 *
 *   mods/my-randomizer/
 *     manifest.json     required — see fields below
 *     seed.bps          patch named by manifest "patch"
 *     overlay/          optional raw byte overlays, "<hex-offset>.bin"
 *
 * manifest.json (flat; unknown keys ignored so newer mods stay loadable):
 *
 *   {
 *     "id":       "my-randomizer",
 *     "name":     "Seasons Randomizer",
 *     "version":  "1.0.0",
 *     "games":    ["tlozoos"],       // which carts it applies to
 *     "patch":    "seed.bps",        // .ips or .bps, relative to mod dir
 *     "overlay":  "overlay",         // dir of <hex-offset>.bin files
 *     "priority": 100,               // lower applies first
 *     "enabled":  true
 *   }
 */
#ifndef GB_MOD_LOADER_H
#define GB_MOD_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_MOD_ID_MAX      64
#define GB_MOD_NAME_MAX    128
#define GB_MOD_VER_MAX     32
#define GB_MOD_PATH_MAX    512
#define GB_MOD_MAX_ENTRIES 64

typedef enum {
    GB_MOD_PATCH_NONE = 0,
    GB_MOD_PATCH_IPS,
    GB_MOD_PATCH_BPS,
} GBModPatchKind;

typedef struct {
    char id[GB_MOD_ID_MAX];
    char name[GB_MOD_NAME_MAX];
    char version[GB_MOD_VER_MAX];
    char dir[GB_MOD_PATH_MAX];      /* mods/<dirname> */
    char patch_file[GB_MOD_PATH_MAX]; /* "" if none */
    char overlay_dir[GB_MOD_PATH_MAX];/* "" if none */
    GBModPatchKind patch_kind;
    int  priority;
    bool enabled;
    bool applies;                   /* matched the active game_id */
} GBModInfo;

/**
 * Scan `mods/` and parse every manifest, keeping those whose "games" list
 * contains @p game_id (a manifest with no "games" key applies to all).
 * Replaces any previously scanned list. Returns the number kept, or -1 if
 * `mods/` does not exist.
 */
int gb_mods_scan(const char* game_id);

int               gb_mods_count(void);
const GBModInfo*  gb_mods_get(int index);
void              gb_mods_set_enabled(int index, bool enabled);

/**
 * Apply every enabled mod, in priority order, directly to @p rom in memory.
 * The buffer is the freshly loaded stock ROM; the file on disk is never
 * touched, so "off" needs no undo at all -- next launch reloads stock.
 *
 * Call gb_mods_scan() first. Returns the number of mods that applied
 * cleanly; failed mods are skipped with a log line and the buffer keeps
 * every earlier mod's work.
 */
int gb_mods_apply_buffer(uint8_t* rom, uint32_t rom_size);

/**
 * Find @p rel (a path relative to a mod's directory, e.g. "voxel/tree.ppm")
 * among the enabled mods that matched the active game. Later-applying mods
 * win, mirroring patch order: the mod with the last word on ROM bytes gets
 * it on assets too. On success @p out holds "mods/<dir>/<rel>".
 */
bool gb_mods_find_asset(const char* rel, char* out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* GB_MOD_LOADER_H */
