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
 * Collision values (constants/common/specialCollisionValues.s and
 * checkGivenCollision_allowHoles::_simpleCollision):
 *   $00        walkable
 *   $01-$0F    four solid-quadrant bits, from bit 3 top-left through
 *              bit 0 bottom-right
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

#define A_wCutsceneIndex  0xC2EF
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
    uint16_t disabled_addr; /* wDisabledObjects */
    bool is_seasons;
} OracleProfile;

static const OracleProfile PROFILES[] = {
    {"ZELDA NAYRU", 0x2A, 0x2C, 0xCC2D, 0xCC32, 0xCC30, 0xCC8A, false},
    {"ZELDA DIN",   0x28, 0x2A, 0xCC49, 0xCC4E, 0xCC4C, 0xCCA4, true},
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

int vox_oracle_page_offset(uint8_t raw, int page_offset) {
    int value = (int8_t)raw;
    int best = value;
    int minus_page = value - page_offset;
    int plus_page = value + page_offset;
    if (abs(minus_page) < abs(best)) best = minus_page;
    if (abs(plus_page) < abs(best)) best = plus_page;
    return best;
}

bool vox_oracle_scripted_scene(uint8_t cutscene_index,
                               uint8_t disabled_objects) {
    /* Index 1 is ordinary gameplay. Interaction scripts commonly remain at
     * index 1 but set wDisabledObjects while they stage Link and NPCs. */
    return cutscene_index > 1 || disabled_objects != 0;
}

bool vox_oracle_link_holds_item(uint8_t link_state) {
    /* constants/common/linkStates.s and treasure.s: LINK_STATE_04 is the
     * forced state used while Link raises a treasure over his head. */
    return link_state == 0x04;
}

uint8_t vox_oracle_object_height(uint8_t layout_id) {
    /* constants/common/tileIndices.s: $F0 opened chest, $F1 closed chest.
     * Their interaction/collision remains walkable, but visually each is a
     * 16x16 prop. Raising the four source quadrants preserves the exact live
     * pixels as its top and folds those same pixels onto the shallow sides. */
    return (layout_id == 0xF0 || layout_id == 0xF1) ? VOX_H_MID : 0xFF;
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
    st->cutscene_index = wram0(ctx, A_wCutsceneIndex);
    st->disabled_objects = wram0(ctx, prof->disabled_addr);
    st->scripted_scene = vox_oracle_scripted_scene(
        (uint8_t)st->cutscene_index, (uint8_t)st->disabled_objects);

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
    /* wScreenOffsetY/X carry two different things in one byte. Small values
     * are transient displacement (shake); large values select the alternate
     * page in the 256x256 double-buffered BG map. The horizontal page starts
     * at 256-room_width = 96px, while the vertical page starts one 128px room
     * down. Choose the nearest page-relative signed value so 96/160 on X and
     * 128 on Y become zero, while 95/97 remain -1/+1 shakes. Treating the
     * page origin as motion shifted Link and room objects by 96px, pulling
     * trees from the far side of the layout over unrelated props. */
    st->off_y = vox_oracle_page_offset(wram0(ctx, A_wScreenOffsetY),
                                       VOX_ROOM_H);
    st->off_x = vox_oracle_page_offset(wram0(ctx, A_wScreenOffsetX),
                                       256 - VOX_ROOM_W);
    /* That decoding is for rooms whose camera is parked at zero -- one
     * screen, where the byte can only mean shake or page. A large
     * scrolling room keeps its camera in hCameraY and uses wScreenOffset
     * as BG-page bookkeeping relative to it (Veran's tower holds -48
     * there at rest); treating that as displacement sampled terrain
     * three rows off. Room space needs only the camera in those rooms --
     * verified against Link's on-screen position: room y minus camera
     * plus the HUD band lands exactly where the flat frame draws him. */
    if (st->cam_x != 0 || st->cam_y != 0) {
        st->off_x = 0;
        st->off_y = 0;
    }
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
        st->link_state = link[0x04];     /* SpecialObjectStruct.state */
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
                    "cam (%d,%d)  off (%d,%d)  link (%d,%d)  "
                    "cut %02X disabled %02X\n",
                    st->active_group, st->active_room, st->scroll_mode,
                    st->cam_x, st->cam_y, st->off_x, st->off_y,
                    st->link_x, st->link_y, st->cutscene_index,
                    st->disabled_objects);
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

static uint8_t solid_height(uint8_t colour_class) {
    /* Collision establishes that this quadrant has volume. The artwork may
     * still distinguish a full-height wall/tree from a lower fence/rock,
     * but a visually quiet solid can never collapse to flat ground. */
    return (colour_class >= VOX_H_HIGH) ? VOX_H_HIGH : VOX_H_MID;
}

/* Map a collision value to the coarse class of its solid portion. This is
 * useful for room summaries and 16x16 override templates; rendering calls
 * vox_oracle_quadrant_height below so partial cells keep their real shape. */
uint8_t vox_oracle_height(uint8_t collision, uint8_t colour_class) {
    if (collision == 0x10) return VOX_H_WATER;              /* hole/water/lava */

    if (collision >= 0x11 && collision <= 0x1F) return VOX_H_FLOOR;  /* bridges, stairs */
    if (collision == 0xFE || collision == 0xFF) return VOX_H_FLOOR;  /* boundary fill */

    if (collision >= 0x01 && collision <= 0x0F)
        return solid_height(colour_class);

    /* Walkable ($00): flat, full stop. An earlier revision let "textured"
     * grass rise one step, which turned every decorated meadow into a
     * raised slab with walls around it -- the texture is what makes grass
     * read as grass, not a ledge. */
    (void)colour_class;
    return VOX_H_FLOOR;
}

uint8_t vox_oracle_quadrant_height(uint8_t collision,
                                   uint8_t colour_class, int quadrant) {
    if (collision >= 0x01 && collision <= 0x0F) {
        /* The original collision routine tests these in screen order:
         * bit 3 TL, bit 2 TR, bit 1 BL, bit 0 BR. A partial cell therefore
         * changes footprint, never vertical elevation. */
        if (quadrant < 0 || quadrant > 3) return VOX_H_FLOOR;
        uint8_t bit = (uint8_t)(0x08u >> quadrant);
        return (collision & bit) ? solid_height(colour_class)
                                 : VOX_H_FLOOR;
    }
    return vox_oracle_height(collision, colour_class);
}
