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

/* One overworld room, in pixels: the play area below the HUD, exactly. Both
 * the world cache's grid and the screen-offset paging in voxel_oracle.c are
 * measured in these. */
#define VOX_ROOM_W 160
#define VOX_ROOM_H 128

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
    int  scroll_mode;          /* wScrollMode, for the status readout */
    int  cam_y, cam_x;          /* hCameraY/X, room pixels */
    int  off_y, off_x;          /* wScreenOffsetY/X, transient draw offset */
    int  link_y, link_x;        /* w1Link whole-pixel position (room space) */
    int  link_z;                /* w1Link zh: 0 on the ground, negative airborne */
    int  link_dir;              /* w1Link direction: 0 up, 1 right, 2 down, 3 left */
    bool is_seasons;            /* which cart the profile matched */
    int  active_group;          /* wActiveGroup: 0/1 outdoors, 2+ interiors */
    int  active_room;           /* wActiveRoom: room index within the group */
    int  room_state;            /* wRoomStateModifier: the season, in Seasons */
    uint8_t collisions[16 * 12]; /* wRoomCollisions, stride 16 */
    /* wRoomLayout $CF00: the OBJECT in each cell (tree mass, bush, rock,
     * sign...), not just whether it blocks. Same stride as collisions. */
    uint8_t layout[16 * 12];
} VoxOracleState;

/* Read + validate the cart's room state. False = not an Oracles cart, or
 * this frame's room data can't be trusted (boot, transition, empty). */
bool vox_oracle_read(GBContext* ctx, VoxOracleState* st);

/* Last frame's terrain decision, for the HUD: whether the game's own data
 * drove the terrain, the wScrollMode it saw, and why it was refused. */
void vox_oracle_status(int* live, int* scroll_mode, const char** reason);

/* Height for one room cell: collision decides, colour breaks ties. */
uint8_t vox_oracle_height(uint8_t collision, uint8_t colour_class);

/* Per-room hand-authored height overrides (voxel/overrides/*.txt next to
 * the binary). Returns a 8x10 grid of height classes (0xFF = keep) or
 * NULL. With VOXEL_EDIT=1 set, visiting a room with no file writes a
 * ready-to-edit template. */
const uint8_t* vox_override_lookup(bool is_seasons, int group, int room,
                                   const uint8_t* collisions);

/* One 16x16 tree object, for the chase camera's billboard trees. The
 * canopy palette is sampled from the tree's own tile art so seasonal
 * palettes carry over. Screen px of the cell's top-left corner. */
typedef struct {
    int sx, sy;
    int hcls;                  /* VOX_H_MID = shrub, VOX_H_HIGH = tree */
    uint32_t lit, mid, dark;   /* canopy shades, bright to shadow */
    uint32_t bark;             /* trunk */
    /* The tree's own 16x16 tile art, upright: the canopy billboard is
     * textured with this (masked to a rounded silhouette), so a tree in
     * 3D is literally the tree the 2D game draws -- and modded tilesets
     * carry over for free. */
    uint32_t tex[16 * 16];
} VoxTree;

#define VOX_MAX_TREES 64

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
    /* Dialog box floating over a live room: the diorama renders from the
     * FROZEN previous state (the game is paused under dialog), and the
     * box rectangle is blitted flat on top from the composed frame. */
    bool text_overlay;
    int box_x, box_y, box_w, box_h;    /* GB pixels, screen space */
    /* Link in screen space, when the oracle state was readable. The renderer
     * uses it to anchor his billboard to the ground he jumped FROM rather
     * than to wherever his sprite happens to be drawn mid-air. */
    bool link_known;
    int  link_sx;               /* centre x */
    int  link_feet_sy;          /* feet row at z=0 (his shadow's row) */
    int  link_jump;             /* pixels airborne, >= 0 */
    int  link_dir;              /* facing: 0 up, 1 right, 2 down, 3 left */
    /* Backdrop sky, from live game state. VOX_SKY_NONE keeps the neutral
     * dark gradient (indoors, dungeons, non-Oracles carts). */
    int  sky;
    uint8_t scx, scy;          /* latched scroll for this frame */
    /* Sub-tile scroll remainder, so the height grid can be sampled in
     * screen space. */
    uint8_t fine_x, fine_y;
    /* Billboard trees (chase camera only): 16px room cells that are one
     * whole tree. treecell marks their 8px tiles so the chase heightfield
     * can treat them as open ground under a free-standing canopy; the
     * diorama ignores both fields and keeps its extruded domes. */
    uint8_t treecell[VOX_TILES_H][VOX_TILES_W];
    VoxTree trees[VOX_MAX_TREES];
    int tree_count;
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
bool vox_scrape(GBContext* ctx, const uint32_t* fb, VoxTileGrid* grid,
                VoxSpriteList* sprites);

/* Decode one 8x8 sprite row's pixels to RGBA (0 = transparent). Used by the
 * renderer to draw billboards straight from VRAM. */
void vox_decode_sprite_row(GBContext* ctx, const VoxSprite* s, int row,
                           uint32_t out[8]);

/* Debug: write the full 32x32 BG map as a PPM (see voxel_tiles.c). */
void vox_dump_bg_map(GBContext* ctx, const char* path);

/* ---- persistent world (voxel_world.c) ----------------------------- */

/* Remember the on-screen room in the world cache and anchor the samplers
 * to it. Call once per trusted scrape; frames that can't be trusted call
 * vox_world_lose() instead and every sampler answers "unknown" until the
 * next good frame. */
void vox_world_remember(const VoxOracleState* st, const VoxTileGrid* grid);
void vox_world_lose(void);

/* Sample the remembered world at SCREEN coordinates -- which may run off
 * any edge of the screen; that is the point. All return false where the
 * position is unknown or the room there has never been visited (or was
 * last seen in a different season). Heights use chase-camera semantics:
 * billboard tree cells read as open ground. */
bool vox_world_height(float sx, float sy, float* out);
bool vox_world_tex(float sx, float sy, uint32_t* out);
/* The 8px tile art under a position, one row of it, for texturing riser
 * faces; leafy (optional) tells trunk shading from cliff courses. */
bool vox_world_face(float sx, float sy, int art_row, uint32_t* out,
                    bool* leafy);

/* Billboard trees of the cached rooms around the current one (the current
 * room's own trees stay live in the grid). Screen-space positions. */
typedef struct {
    const VoxTree* t;
    int sx, sy;
} VoxWorldTree;
int vox_world_neighbor_trees(VoxWorldTree* out, int max);

/* Render the diorama into out, at an integer scale above the GB screen
 * (out must hold GB_FRAMEBUFFER_SIZE * scale^2 pixels). fb is the game's
 * own composed frame, used for the HUD rows and flat passthrough. */
void vox_render(GBContext* ctx, const VoxTileGrid* grid,
                const VoxSpriteList* sprites, const uint32_t* fb,
                int mode, int scale, uint32_t* out);

/** One frame of chase-camera aim: reads the stick and the recentre
 *  button, updates the heading, and reports Link's held anchor. Split out
 *  of the renderer so tools/chasecam_test.c can drive the yaw rules with
 *  no window and no cart. Either out-param may be NULL. */
void vox_chase_step(const VoxTileGrid* grid, float* out_lx, float* out_ly);

#endif /* EPOCH_VOXEL_INTERNAL_H */
