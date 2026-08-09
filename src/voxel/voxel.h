/**
 * @file voxel.h
 * @brief Voxel diorama renderer for the Oracles compilation.
 *
 * Re-renders the overworld as a tilted 3D diorama, built every frame from
 * the PPU's own state — BG tilemap, tile patterns, CGB palettes, OAM.
 * This is the only honest source a static recompilation has: there is no
 * scene graph, no entity list, just the VRAM the game wrote.
 *
 * The pipeline:
 *
 *   1. voxel_tiles: read the visible 21x19 BG tile window (SCX/SCY
 *      aligned), decode each tile's palette + pattern, and classify it to
 *      a terrain height. Classification is per 8x8 tile, by dominant hue
 *      and luma structure — water sinks, paths lie flat, bushes and rocks
 *      rise, trees and walls rise higher. It's presentational: it changes
 *      what the world LOOKS like, never what it is.
 *
 *   2. voxel_render: draw the height field with a pitched camera as
 *      per-column ray marches (the Comanche "voxel space" scheme): for
 *      each screen column, march the map from far to near, projecting
 *      each cell's top surface and drawing the exposed front wall where
 *      height drops. Painter's order per column, no z-buffer needed.
 *      Terrain texture is the game's own composed background pixels, so
 *      palettes/season tints carry through. OAM sprites are re-decoded
 *      from VRAM and stood upright as billboards at their map position.
 *
 * Output is a plain GB_FRAMEBUFFER_SIZE RGBA buffer substituted via the
 * runtime's frame hook, so the whole existing present path — GLES shaders,
 * scaling, screenshots, frame dumps — applies to the diorama unchanged.
 *
 * Toggle at runtime with F3 (cycles OFF -> tilt levels -> OFF), or start
 * enabled with --voxel N. The HUD rows the game draws via the window layer
 * are composited back flat on top.
 */
#ifndef EPOCH_VOXEL_H
#define EPOCH_VOXEL_H

#include <stdbool.h>
#include <stdint.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Camera presets, in the spirit of an options ladder. 0 = off. The tilt
 * levels are the fixed diorama; CHASE is a third-person perspective camera
 * floating behind the player, raycasting the same heightfield. */
#define VOXEL_MODE_OFF   0
#define VOXEL_MODE_15    1
#define VOXEL_MODE_30    2
#define VOXEL_MODE_45    3
#define VOXEL_MODE_CHASE 4
#define VOXEL_MODE_COUNT 5

/* Live-tunable shape and camera constants.
 *
 * The per-room override files decide WHICH height class a cell is; these
 * decide what a height class LOOKS like. They were compile-time constants,
 * which meant every "the trees are too tall" needed a rebuild. They are
 * sliders in the Esc menu now, saved to voxel/tuning.ini, so a good look
 * can be found while playing and then baked in as the default.
 */
typedef struct {
    float units[5];       /* extrusion per height class, indexed by class */
    float footprint;      /* px of taper at a foliage edge (0 = hard cell) */
    float tilt_scale;     /* multiplier on the diorama's vertical lift */
    float chase_back;     /* camera distance behind the player */
    float chase_height;   /* camera height above the player's ground */
    float chase_fov;      /* focal length as a fraction of output width */
    float chase_hpx;      /* screen px per height unit in perspective */
    float fog_start;      /* distance where distance fog begins */
    float fog_max;        /* strongest fog blend, 0-256 */
} VoxelTuning;

/** The live tuning block. Safe to mutate; the renderer reads it per frame. */
VoxelTuning* voxel_tuning(void);

/** Restore the shipped defaults. */
void voxel_tuning_reset(void);

/** Persist to / restore from voxel/tuning.ini next to the binary. */
void voxel_tuning_save(void);
void voxel_tuning_load(void);

/** Install the frame hook. Call once, before the cart's main loop. */
void voxel_install(void);

/** Register the diorama's section in the runtime's Esc settings menu.
 * Call after gb_platform_init(). Implemented in voxel_menu.cpp. */
void voxel_menu_install(void);

/** Rotate the pressed d-pad direction into the chase camera's frame.
 *  Call once per frame after input is polled; a no-op in every other
 *  mode. Without it, "right" walks east even when east is behind you. */
void voxel_remap_dpad(void);

/** Is terrain coming from the game's own collision data this frame?
 *  live = 1 yes, 0 no (the room renders as a flat slab). scroll_mode is
 *  the game's wScrollMode, and reason names which check refused it.
 *  Any out-param may be NULL. */
void voxel_terrain_status(int* live, int* scroll_mode, const char** reason);

/** Current mode (VOXEL_MODE_*). */
int  voxel_get_mode(void);
void voxel_set_mode(int mode);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_VOXEL_H */
