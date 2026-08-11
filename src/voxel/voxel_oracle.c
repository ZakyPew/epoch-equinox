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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- WRAM/HRAM addresses (oracles-disasm include/wram.s, hram.s) ------- */

#define WRAM_BANK_SIZE 4096

#define A_wTextIsActive   0xCBA0
#define A_wOpenedMenuType 0xCBCB
#define A_wScrollMode     0xCD00
#define A_wScreenOffsetY  0xCD08
#define A_wScreenOffsetX  0xCD09
#define A_wRoomCollisions 0xCE00
#define A_wRoomLayout     0xCF00
#define A_w1Link          0xD000   /* SpecialObjectStruct, WRAM bank 1 */

#define COLL_W 16
#define COLL_H 12

/* hCameraY/X are per-game ("$ffaa/$ffa8" notation in the disasm is
 * Ages/Seasons). Offsets are into ctx->hram, which starts at $FF80. */
typedef struct {
    const char* title;      /* ROM header title at $134 */
    uint8_t cam_y_off;      /* hCameraY - $FF80 */
    uint8_t cam_x_off;      /* hCameraX - $FF80 */
    uint16_t group_addr;    /* wActiveGroup */
    uint16_t state_addr;    /* wRoomStateModifier (the season, in Seasons) */
    uint16_t room_addr;     /* wActiveRoom (verified live: changes 7A->6A
                             * crossing one room north in Ages) */
    bool is_seasons;
} OracleProfile;

static const OracleProfile PROFILES[] = {
    {"ZELDA NAYRU", 0x2A, 0x2C, 0xCC2D, 0xCC32, 0xCC30, false},
    {"ZELDA DIN",   0x28, 0x2A, 0xCC49, 0xCC4E, 0xCC4C, true},
};

/* Cached per-ROM detection. GBContext has no user slot, so key the cache on
 * the ROM pointer -- good enough for one cart per process, which is how the
 * runner works. */
static uint8_t g_last_scroll = 0;
static bool g_last_live = false;
/* Why last frame's terrain was refused, in words. A flat room is always
 * one of a short list of refusals, and the difference between them is the
 * difference between a bug in the gate and a room that genuinely has no
 * shape -- worth one screenshot instead of a guessing round trip. */
static const char* g_last_reason = "";

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
    g_last_live = false;
    g_last_reason = "";

    /* A/B switch: VOXEL_NO_ORACLE=1 forces the colour-only classifier,
     * which is how the two are compared on identical frames. */
    if (getenv("VOXEL_NO_ORACLE")) { g_last_reason = "forced off"; return false; }

    const OracleProfile* prof = detect(ctx);
    if (!prof || !ctx->wram || !ctx->hram) {
        g_last_reason = "not an Oracles cart";
        return false;
    }
    st->profile_matched = true;

    st->menu_open = wram0(ctx, A_wOpenedMenuType) != 0;
    st->text_active = wram0(ctx, A_wTextIsActive) != 0;

    /* wScrollMode is 1 in normal play. Anything else -- 0 (no room), bit 2
     * (scrolling), and the other transition values the disasm names -- means
     * the screen shows a mix of two rooms while wRoomCollisions already
     * holds the NEW room's grid. Extruding that draws the wrong terrain
     * under half the picture (verified live: the old `bit 2` filter let
     * several mid-scroll frames through), so accept exactly the one value
     * that means "one room, at rest". */
    uint8_t scroll = wram0(ctx, A_wScrollMode);
    st->no_room = (scroll == 0);
    st->scroll_mode = scroll;
    g_last_scroll = scroll;
    /* Trust the room unless the screen is showing two of them.
     *
     * This gate was once "bit 2 set", which let transitions through (they
     * read 8), and was then tightened to "exactly 1" -- which fixed
     * transitions and quietly flattened every area that idles in some
     * other mode, including whole groups nobody had walked through yet.
     * Reject the states that mean no room (0) or a room change (bit 2, and
     * the 8 seen live mid-scroll); anything else is a room at rest. */
    if (scroll == 0 || (scroll & 0x04) || scroll == 0x08) {
        g_last_reason = (scroll == 0) ? "no room active" : "room change";
        return false;
    }

    st->cam_y = (int)ctx->hram[prof->cam_y_off] |
                ((int)ctx->hram[prof->cam_y_off + 1] << 8);
    st->cam_x = (int)ctx->hram[prof->cam_x_off] |
                ((int)ctx->hram[prof->cam_x_off + 1] << 8);
    /* wScreenOffsetY carries two different things in one byte. Small values
     * are the transient draw shift this field is named for (screen shake),
     * and screen->room conversions have to undo them. But a room whose
     * tilemap lives in the lower half of the 256px-tall BG map reports a
     * whole room height here -- 0x80, seen on every odd overworld row in
     * Ages (SCY 112 there against 240 on even rows) -- and that is where
     * the room was *drawn*, not where it moved to. Feeding it to the
     * converters pushed every collision and layout lookup a full room off
     * the table, which is why odd-row rooms rendered as bare smeared ground
     * with no trees and never made it into the world cache. Page offsets
     * are whole rooms, shakes never are, so fold them out here. */
    int8_t oy = (int8_t)wram0(ctx, A_wScreenOffsetY);
    st->off_y = (oy % VOX_ROOM_H == 0) ? 0 : oy;
    st->off_x = (int8_t)wram0(ctx, A_wScreenOffsetX);
    st->is_seasons = prof->is_seasons;
    st->active_group = wram0(ctx, prof->group_addr);
    st->active_room = wram0(ctx, prof->room_addr);
    st->room_state = wram0(ctx, prof->state_addr);

    /* w1Link lives in WRAM bank 1 regardless of the currently-mapped bank:
     * index the backing store directly. Offsets are SpecialObjectStruct's
     * yh/xh/zh -- whole pixels; z is signed and negative while airborne. */
    {
        const uint8_t* link = ctx->wram + 1 * WRAM_BANK_SIZE + (A_w1Link - 0xD000);
        st->link_y = link[0x0B];
        st->link_x = link[0x0D];
        st->link_z = (int8_t)link[0x0F];
        st->link_dir = link[0x08] & 3;   /* SpecialObjectStruct.direction */
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
        st->layout[i] = wram0(ctx, A_wRoomLayout + i);
        if (!seen[v]) { seen[v] = 1; distinct++; }
    }
    if (distinct < 2) {
        g_last_reason = "collision grid empty";
        return false;
    }

    st->valid = true;
    g_last_live = true;

    /* VOX_DUMP_ROOM=1 prints the room the renderer is actually working
     * from, once a second: the camera numbers that map screen to room,
     * and the collision grid with the height class each cell becomes.
     * Indoors and outdoors differ in ways screenshots only hint at. */
    if (getenv("VOX_DUMP_ROOM")) {
        static int tick = 0;
        if ((tick++ % 60) == 0) {
            fprintf(stderr,
                    "\n[room] group %02X room %02X  scroll %02X  "
                    "cam (%d,%d)  off (%d,%d)  link (%d,%d)\n",
                    st->active_group, st->active_room, st->scroll_mode,
                    st->cam_x, st->cam_y, st->off_x, st->off_y,
                    st->link_x, st->link_y);
            for (int r = 0; r < COLL_H; r++) {
                fprintf(stderr, "  ");
                for (int c = 0; c < COLL_W; c++)
                    fprintf(stderr, "%02X ", st->collisions[r * COLL_W + c]);
                fprintf(stderr, "  |");
                for (int c = 0; c < COLL_W; c++) {
                    static const char CH[5] = { '~', '.', 'o', 'O', '#' };
                    uint8_t h = vox_oracle_height(
                        st->collisions[r * COLL_W + c], VOX_H_FLOOR);
                    fprintf(stderr, "%c", h < 5 ? CH[h] : '?');
                }
                fprintf(stderr, "|\n");
            }
        }
    }
    return true;
}

/* What the last frame's terrain decision was, for the on-screen readout:
 * a flat room is almost always this gate saying no, and the scroll mode
 * is the number that explains which room state it saw. */
void vox_oracle_status(int* live, int* scroll_mode, const char** reason) {
    if (live) *live = g_last_live ? 1 : 0;
    if (scroll_mode) *scroll_mode = (int)g_last_scroll;
    if (reason) *reason = g_last_reason ? g_last_reason : "";
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

    /* Walkable ($00): flat, full stop. An earlier revision let "textured"
     * grass rise one step, which turned every decorated meadow into a
     * raised slab with walls around it -- the texture is what makes grass
     * read as grass, not a ledge. */
    (void)colour_class;
    return VOX_H_FLOOR;
}
