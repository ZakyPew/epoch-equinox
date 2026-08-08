/**
 * @file mod_loader.h
 * @brief Mod discovery and ROM patching for the Oracles compilation.
 *
 * Design constraint: neither cart's generated `*_main.c` may be edited.
 * `tlozooa_*.c` is generated-but-committed here, and `tlozoos_*.c` is
 * FetchContent'd from GB-Recomp/tlozoos, so there is no hook point
 * inside either cart we can rely on.
 *
 * So mods are applied *between* the two things the runtime already does:
 *
 *   1. `gb_load_assets()` extracts the verified ROM into
 *      `assets/<id>/rom.bin` (SHA-1 checked against the source ROM).
 *   2. On every later boot it reads `assets/<id>/rom.bin` straight back
 *      with no hash check at all (`load_from_assets()` in the runtime's
 *      gb_asset_loader.c only verifies on the extract path).
 *
 * Step 2 is the seam. The launcher stages assets, keeps a pristine
 * `rom.bin.orig` snapshot, and rewrites `rom.bin` from that snapshot plus
 * whatever mods are enabled — before handing control to the cart. The
 * cart then boots a patched ROM without knowing anything happened, and
 * disabling a mod is just a re-stage from the snapshot.
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
 * Rebuild `assets/<game_id>/rom.bin` from the pristine snapshot plus every
 * enabled mod, in priority order.
 *
 * On first call for a game it creates the snapshot (`rom.bin.orig`) from
 * the freshly extracted rom.bin. Callers must have staged assets already —
 * see gb_mods_stage_assets().
 *
 * Safe to call with zero enabled mods: that restores the stock ROM, which
 * is exactly what "turn everything off" should do.
 *
 * @return true on success; false leaves rom.bin restored to the snapshot.
 */
bool gb_mods_apply(const char* game_id, uint32_t rom_size);

/**
 * Ensure `assets/<game_id>/rom.bin` exists, extracting from @p rom_path if
 * needed. Thin wrapper over the runtime's gb_load_assets() using a
 * temporary staging buffer, so the launcher can patch before the cart
 * boots. @p expected_sha1 is the 20-byte hash of the stock ROM.
 */
bool gb_mods_stage_assets(const char* game_id,
                          const char* rom_path,
                          const uint8_t expected_sha1[20],
                          uint32_t rom_size,
                          const void* manifest,
                          uint32_t manifest_count);

/** Restore the stock ROM and forget the snapshot (used by "verify files"). */
bool gb_mods_restore_stock(const char* game_id);

#ifdef __cplusplus
}
#endif

#endif /* GB_MOD_LOADER_H */
