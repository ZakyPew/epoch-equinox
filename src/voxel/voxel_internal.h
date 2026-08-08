/* Shared internals between voxel_tiles.c and voxel_render.c. */
#ifndef EPOCH_VOXEL_INTERNAL_H
#define EPOCH_VOXEL_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "gbrt.h"
#include "ppu.h"

/* The visible BG window in tiles, one extra on each axis for the partial
 * tile SCX/SCY scrolling exposes. */
#define VOX_TILES_W 21
#define VOX_TILES_H 19

/* The sprite-free terrain texture, in screen-aligned pixels (one extra
 * tile each way for scroll, like the tile grid). */
#define VOX_TEX_W (VOX_TILES_W * 8)
#define VOX_TEX_H (VOX_TILES_H * 8)

/* BG map rows at or below this index hold the status bar rather than world
 * tiles (see VoxTileGrid::hud_rows). */
#define VOX_HUD_MAP_ROW 30

/* Terrain height classes, in pixels of extrusion at full tilt. */
typedef enum {
    VOX_H_WATER  = 0,   /* sinks below ground level */
    VOX_H_FLOOR  = 1,   /* paths, floors, sand */
    VOX_H_LOW    = 2,   /* grass, flowers, shallow decor */
    VOX_H_MID    = 3,   /* bushes, rocks, fences */
    VOX_H_HIGH   = 4,   /* trees, walls, houses */
} VoxHeightClass;

/* Live game state read from the Oracles carts' own WRAM/HRAM, when the
 * running cart is one of them and the frame is trustworthy. See
 * voxel_oracle.c for the addresses and the validity rules. */
typedef struct {
    bool valid;
    /* The running cart IS one of the Oracles, even if this particular
     * frame's room state can't be trusted (mid-scroll, cinematic). Lets
     * the renderer hold steady instead of falling back to colour
     * guesswork for half a second every room change. */
    bool profile_matched;
    bool menu_open;
    /* wTextIsActive: a dialog box is on screen. Textboxes are drawn into
     * the BG tilemap, so the diorama would squash and terrain-warp the
     * words; the frame passes through flat instead, like menus. Verified
     * live: $CBA0 is 1 exactly while text shows, 0 otherwise. */
    bool text_active;
    /* wScrollMode == 0: no room is active at all (title, file select,
     * cutscenes). There is no terrain to extrude; pass through flat. */
    bool no_room;
    int  cam_y, cam_x;          /* hCameraY/X, room pixels */
    int  off_y, off_x;          /* wScreenOffsetY/X, transient draw offset */
    int  link_y, link_x;        /* w1Link whole-pixel position (room space) */
    int  link_z;                /* w1Link zh: 0 on the ground, negative airborne */
    bool is_seasons;            /* which cart the profile matched */
    int  active_group;          /* wActiveGroup: 0/1 outdoors, 2+ interiors */
    int  room_state;            /* wRoomStateModifier: the season, in Seasons */
    uint8_t collisions[16 * 12]; /* wRoomCollisions, stride 16 */
} VoxOracleState;

/* Read + validate the cart's room state. False = not an Oracles cart, or
 * this frame's room data can't be trusted (boot, transition, empty). */
bool vox_oracle_read(GBContext* ctx, VoxOracleState* st);

/* Height for one room cell: collision decides, colour breaks ties. */
uint8_t vox_oracle_height(uint8_t collision, uint8_t colour_class);

typedef struct {
    /* Per visible tile: height class and whether it's part of the window
     * layer (HUD) rather than the scrolling BG. */
    uint8_t height[VOX_TILES_H][VOX_TILES_W];
    /* Foliage flag per tile (green-leaning pattern): raised leafy cells get
     * domed tops in the renderer so trees read as canopies, not crates. */
    uint8_t leafy[VOX_TILES_H][VOX_TILES_W];
    /* Terrain texture decoded straight from the BG tilemap -- the composed
     * frame has sprites baked into it, and using it as the ground texture
     * warped a flattened copy of every character into the terrain
     * underneath their billboard. Screen-aligned: screen pixel (x, y) is
     * tex[(y + fine_y) * VOX_TEX_W + x + fine_x]. */
    uint32_t tex[VOX_TEX_H * VOX_TEX_W];
    /* True while a full-screen menu owns the display: render the frame
     * flat instead of extruding inventory screens. */
    bool flat;
    /* Link in screen space, when the oracle state was readable. The renderer
     * uses it to anchor his billboard to the ground he jumped FROM rather
     * than to wherever his sprite happens to be drawn mid-air. */
    bool link_known;
    int  link_sx;               /* centre x */
    int  link_feet_sy;          /* feet row at z=0 (his shadow's row) */
    int  link_jump;             /* pixels airborne, >= 0 */
    /* Backdrop sky, from live game state. VOX_SKY_NONE keeps the neutral
     * dark gradient (indoors, dungeons, non-Oracles carts). */
    int  sky;
    uint8_t scx, scy;          /* latched scroll for this frame */
    /* Sub-tile scroll remainder, so the height grid can be sampled in
     * screen space. */
    uint8_t fine_x, fine_y;
    /* Height in screen pixels of the HUD band at the top of the screen.
     *
     * Oracles does not use the window layer for its status bar — probing the
     * live registers during gameplay shows wy=199 (off-screen) while
     * scy=240. The bar lives in the *bottom* two rows of the 32x32 BG map
     * and the vertical wrap brings it round to the top of the screen. So the
     * band is found by walking tile rows from the top while their source map
     * row is in that reserved strip, not by reading WY. 0 = no HUD. */
    int hud_rows;
} VoxTileGrid;

typedef struct {
    int16_t x, y;              /* screen position (top-left of sprite) */
    uint8_t tile;
    uint8_t attr;
    bool    tall;              /* 8x16 mode */
} VoxSprite;

/* Sky kinds for VoxTileGrid.sky. */
enum {
    VOX_SKY_NONE = -1,
    VOX_SKY_AGES_PRESENT = 0,
    VOX_SKY_AGES_PAST,
    VOX_SKY_SPRING,
    VOX_SKY_SUMMER,
    VOX_SKY_AUTUMN,
    VOX_SKY_WINTER,
    VOX_SKY_SUBROSIA,
};

#define VOX_MAX_SPRITES 40

typedef struct {
    VoxSprite entries[VOX_MAX_SPRITES];
    int count;
} VoxSpriteList;

/* Scrape + classify the current PPU state. Returns false if the PPU isn't
 * in a state worth rendering (LCD off, mid-mode weirdness). */
bool vox_scrape(GBContext* ctx, VoxTileGrid* grid, VoxSpriteList* sprites);

/* Decode one 8x8 sprite row's pixels to RGBA (0 = transparent). Used by the
 * renderer to draw billboards straight from VRAM. */
void vox_decode_sprite_row(GBContext* ctx, const VoxSprite* s, int row,
                           uint32_t out[8]);

/* Debug: write the full 32x32 BG map as a PPM (see voxel_tiles.c). */
void vox_dump_bg_map(GBContext* ctx, const char* path);

/* Render the diorama into out, at an integer scale above the GB screen
 * (out must hold GB_FRAMEBUFFER_SIZE * scale^2 pixels). fb is the game's
 * own composed frame, used for the HUD rows and flat passthrough. */
void vox_render(GBContext* ctx, const VoxTileGrid* grid,
                const VoxSpriteList* sprites, const uint32_t* fb,
                int mode, int scale, uint32_t* out);

#endif /* EPOCH_VOXEL_INTERNAL_H */
