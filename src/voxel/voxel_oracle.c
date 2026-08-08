/* Oracle-aware terrain: read the game's own collision data instead of
 * guessing from colours.
 *
 * The colour classifier (voxel_tiles.c) infers height from what tiles look
 * like, which is why menus extrude and water only sinks when it happens to
 * be blue. Both Oracles carts keep the truth in WRAM, at addresses named by
 * Stewmath/oracles-disasm:
 *
 *   wRoomCollisions  $CE00  bank 0   collision value per 16x16 room cell
 *   wScrollMode      $CD00  bank 0   1 = normal play, bit 2 = scrolling
 *   wScreenOffsetY/X $CD08  bank 0   transient draw offset (shakes etc.)
 *   wOpenedMenuType  $CBCB  bank 0   nonzero while a menu owns the screen
 *   hCameraY/X       HRAM   16-bit camera, at different addresses per game
 *
 * Collision values (constants/common/specialCollisionValues.s):
 *   $00        walkable
 *   $01-$0F    solid shapes (corners, edges, full block)
 *   $10        hole / deep water / lava -- the things you sink into
 *   $11-$1B    bridges, minecart track, stairs -- walkable
 *   $FE/$FF    screen boundary filler
 *
 * Verified live with tools/ram_probe.c: a real room reads as a 10x8 grid
 * (stride 16) of $00/$0F with $FF boundary fill, and the buffers are empty
 * until gameplay starts -- which is exactly the "are we in a room?" signal
 * needed to stop menus and cinematics from extruding.
 *
 * Anything that isn't one of the two Oracles carts, or any frame where the
 * room state can't be trusted (menu open, mid-transition, buffers empty),
 * falls back to the colour classifier. The diorama stays game-agnostic;
 * it just gets the real answers where real answers exist.
 */
#include "voxel_internal.h"

#include <stdlib.h>
#include <string.h>

/* --- WRAM/HRAM addresses (oracles-disasm include/wram.s, hram.s) ------- */

#define WRAM_BANK_SIZE 4096

#define A_wOpenedMenuType 0xCBCB
#define A_wScrollMode     0xCD00
#define A_wScreenOffsetY  0xCD08
#define A_wScreenOffsetX  0xCD09
#define A_wRoomCollisions 0xCE00
#define A_w1Link          0xD000   /* SpecialObjectStruct, WRAM bank 1 */

#define COLL_W 16
#define COLL_H 12

/* hCameraY/X are per-game ("$ffaa/$ffa8" notation in the disasm is
 * Ages/Seasons). Offsets are into ctx->hram, which starts at $FF80. */
typedef struct {
    const char* title;      /* ROM header title at $134 */
    uint8_t cam_y_off;      /* hCameraY - $FF80 */
    uint8_t cam_x_off;      /* hCameraX - $FF80 */
} OracleProfile;

static const OracleProfile PROFILES[] = {
    {"ZELDA NAYRU", 0x2A, 0x2C},   /* Oracle of Ages */
    {"ZELDA DIN",   0x28, 0x2A},   /* Oracle of Seasons */
};

/* Cached per-ROM detection. GBContext has no user slot, so key the cache on
 * the ROM pointer -- good enough for one cart per process, which is how the
 * runner works. */
static const OracleProfile* g_profile = NULL;
static const uint8_t* g_profile_rom = NULL;

static const OracleProfile* detect(GBContext* ctx) {
    if (!ctx->rom) return NULL;
    if (ctx->rom == g_profile_rom) return g_profile;

    g_profile_rom = ctx->rom;
    g_profile = NULL;
    for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++) {
        size_t n = strlen(PROFILES[i].title);
        if (memcmp(ctx->rom + 0x134, PROFILES[i].title, n) == 0) {
            g_profile = &PROFILES[i];
            break;
        }
    }
    return g_profile;
}

static inline uint8_t wram0(GBContext* ctx, uint16_t addr) {
    return ctx->wram[addr - 0xC000];
}

bool vox_oracle_read(GBContext* ctx, VoxOracleState* st) {
    memset(st, 0, sizeof(*st));

    /* A/B switch: VOXEL_NO_ORACLE=1 forces the colour-only classifier,
     * which is how the two are compared on identical frames. */
    if (getenv("VOXEL_NO_ORACLE")) return false;

    const OracleProfile* prof = detect(ctx);
    if (!prof || !ctx->wram || !ctx->hram) return false;

    st->menu_open = wram0(ctx, A_wOpenedMenuType) != 0;

    /* wScrollMode is 1 in normal play; bit 2 set (or value 8) while the
     * screen scrolls to the next room, during which wRoomCollisions already
     * holds the NEW room while the screen still shows a mix of both. The
     * colour fallback covers that half-second better than a misaligned
     * grid would. 0 means no room is active at all. */
    uint8_t scroll = wram0(ctx, A_wScrollMode);
    if (scroll == 0 || (scroll & 0x04)) return false;

    st->cam_y = (int)ctx->hram[prof->cam_y_off] |
                ((int)ctx->hram[prof->cam_y_off + 1] << 8);
    st->cam_x = (int)ctx->hram[prof->cam_x_off] |
                ((int)ctx->hram[prof->cam_x_off + 1] << 8);
    st->off_y = (int8_t)wram0(ctx, A_wScreenOffsetY);
    st->off_x = (int8_t)wram0(ctx, A_wScreenOffsetX);

    /* w1Link lives in WRAM bank 1 regardless of the currently-mapped bank:
     * index the backing store directly. Offsets are SpecialObjectStruct's
     * yh/xh/zh -- whole pixels; z is signed and negative while airborne. */
    {
        const uint8_t* link = ctx->wram + 1 * WRAM_BANK_SIZE + (A_w1Link - 0xD000);
        st->link_y = link[0x0B];
        st->link_x = link[0x0D];
        st->link_z = (int8_t)link[0x0F];
    }

    /* Copy the room's collision grid, and require it to be populated:
     * during boot cinematics it's all zero, and one distinct value means
     * "not a room". */
    int distinct = 0;
    int seen[256];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < COLL_W * COLL_H; i++) {
        uint8_t v = wram0(ctx, A_wRoomCollisions + i);
        st->collisions[i] = v;
        if (!seen[v]) { seen[v] = 1; distinct++; }
    }
    if (distinct < 2) return false;

    st->valid = true;
    return true;
}

/* Map a collision value to a height class, using the tile's colour features
 * only to split ambiguous cases (solid: wall vs bush; walkable: grass vs
 * path). The load-bearing decisions -- what sinks, what rises, what is
 * flat -- come from the game. */
uint8_t vox_oracle_height(uint8_t collision, uint8_t colour_class) {
    if (collision == 0x10) return VOX_H_WATER;              /* hole/water/lava */

    if (collision >= 0x11 && collision <= 0x1F) return VOX_H_FLOOR;  /* bridges, stairs */
    if (collision == 0xFE || collision == 0xFF) return VOX_H_FLOOR;  /* boundary fill */

    if (collision == 0x0F) {
        /* Full solid block. The colour classifier is good at telling a
         * tree from a fence once it KNOWS the thing is solid -- its
         * failure mode was calling flat things tall, not mis-ranking
         * tall things. */
        return (colour_class >= VOX_H_HIGH) ? VOX_H_HIGH : VOX_H_MID;
    }
    if (collision >= 0x01 && collision <= 0x0E) {
        /* Partial shapes: diagonal cliff corners and edges. Rendering them
         * at full height turns every diagonal into a hard staircase; a low
         * bevel keeps the slope readable. */
        return VOX_H_LOW;
    }

    /* Walkable ($00): flat by default; keep grass texture if the colours
     * found some, but never let a walkable cell rise above LOW. */
    return (colour_class == VOX_H_LOW) ? VOX_H_LOW : VOX_H_FLOOR;
}
