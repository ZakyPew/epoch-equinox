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

/** Install the frame hook. Call once, before the cart's main loop. */
void voxel_install(void);

/** Register the diorama's section in the runtime's Esc settings menu.
 * Call after gb_platform_init(). Implemented in voxel_menu.cpp. */
void voxel_menu_install(void);

/** Current mode (VOXEL_MODE_*). */
int  voxel_get_mode(void);
void voxel_set_mode(int mode);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_VOXEL_H */
