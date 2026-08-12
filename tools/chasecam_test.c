/* Exercises the chase camera's yaw rules without a cart or a window.
 *
 * The camera's feel is the whole point of these rules, and feel is exactly
 * what a screenshot cannot check. What IS checkable: that it trails him
 * while he walks, that it hands control over the moment the stick moves
 * and does not take it straight back, that a recentre tap finishes its
 * swing on its own, and that a held recentre keeps following.
 *
 * Build like vox_shot (it links the same libraries):
 *
 *   cc -O2 -o chasecam_test ../tools/chasecam_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 *
 * Exits nonzero if any check fails.
 */
#include "voxel/voxel.h"
#include "voxel/voxel_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

/* Yaw the camera should hold when sitting behind Link, per facing. */
static const float DIR_YAW[4] = { -1.5707963f, 0.0f, 1.5707963f, 3.1415927f };

static float ang_diff(float a, float b) {
    float d = a - b;
    while (d >  3.1415927f) d -= 6.2831853f;
    while (d < -3.1415927f) d += 6.2831853f;
    return fabsf(d);
}

/* One frame of the camera, with Link at (x, y) facing dir. The renderer
 * needs a grid; a bare one with the anchor filled in is enough for the
 * yaw rules, which is all this exercises. */
static void frame(int dir, float x, float y) {
    VoxTileGrid grid;
    memset(&grid, 0, sizeof(grid));
    grid.link_known = 1;
    grid.link_dir = dir;
    grid.link_sx = (int)x;
    grid.link_feet_sy = (int)y;
    vox_chase_step(&grid, NULL, NULL);
}

static void floor_grid(VoxTileGrid* grid) {
    memset(grid, 0, sizeof(*grid));
    for (int y = 0; y < VOX_TILES_H; y++)
        for (int x = 0; x < VOX_TILES_W; x++)
            grid->height[y][x] = VOX_H_FLOOR;
}

int main(void) {
    /* -- live-state coordinate and scripted-camera rules -------------- */
    CHECK(vox_oracle_page_offset(0, 96) == 0 &&
          vox_oracle_page_offset(96, 96) == 0 &&
          vox_oracle_page_offset(160, 96) == 0,
          "horizontal BG-map pages normalize to the same room origin");
    CHECK(vox_oracle_page_offset(95, 96) == -1 &&
          vox_oracle_page_offset(97, 96) == 1 &&
          vox_oracle_page_offset(127, 128) == -1 &&
          vox_oracle_page_offset(129, 128) == 1,
          "small page-relative screen offsets survive as signed motion");
    CHECK(vox_oracle_object_height(0xF0) == VOX_H_MID &&
          vox_oracle_object_height(0xF1) == VOX_H_MID &&
          vox_oracle_object_height(0xF2) == 0xFF,
          "open and closed chest metatiles are semantic 3D props");
    CHECK(vox_oracle_scripted_scene(2, 0) &&
          vox_oracle_scripted_scene(1, 1) &&
          !vox_oracle_scripted_scene(1, 0),
          "formal and interaction scripts select staged camera framing");
    CHECK(vox_oracle_link_holds_item(0x04) &&
          !vox_oracle_link_holds_item(0x03) &&
          !vox_oracle_link_holds_item(0x05),
          "only Link's real item-get state anchors an overhead sprite");
    CHECK(vox_scene_render_mode(VOXEL_MODE_CHASE, true) == VOXEL_MODE_30 &&
          vox_scene_render_mode(VOXEL_MODE_CHASE, false) == VOXEL_MODE_CHASE &&
          vox_scene_render_mode(VOXEL_MODE_45, true) == VOXEL_MODE_45,
          "only chase mode yields to the fixed voxel cutscene camera");

    /* Screen-relative movement follows the same forward/right basis as the
     * projection. At north, controls are unchanged; after a quarter/half
     * orbit each key maps to the world direction now visible on screen. */
    CHECK(vox_chase_remap_pressed(0x04, DIR_YAW[0]) == 0x04 &&
          vox_chase_remap_pressed(0x08, DIR_YAW[0]) == 0x08 &&
          vox_chase_remap_pressed(0x02, DIR_YAW[0]) == 0x02 &&
          vox_chase_remap_pressed(0x01, DIR_YAW[0]) == 0x01,
          "north-facing chase controls preserve W S A D");
    CHECK(vox_chase_remap_pressed(0x04, DIR_YAW[3]) == 0x02 &&
          vox_chase_remap_pressed(0x08, DIR_YAW[3]) == 0x01 &&
          vox_chase_remap_pressed(0x02, DIR_YAW[3]) == 0x08 &&
          vox_chase_remap_pressed(0x01, DIR_YAW[3]) == 0x04,
          "west-facing chase controls stay aligned with the screen");
    CHECK(vox_chase_remap_pressed(0x04, DIR_YAW[1]) == 0x01 &&
          vox_chase_remap_pressed(0x08, DIR_YAW[1]) == 0x02 &&
          vox_chase_remap_pressed(0x02, DIR_YAW[1]) == 0x04 &&
          vox_chase_remap_pressed(0x01, DIR_YAW[1]) == 0x08,
          "east-facing chase controls stay aligned with the screen");

    /* -- first live frame anchors both aim and position ---------------- */
    voxel_set_mode(VOXEL_MODE_OFF);
    voxel_set_mode(VOXEL_MODE_CHASE);
    VoxTileGrid anchor_grid;
    floor_grid(&anchor_grid);
    anchor_grid.link_known = true;
    anchor_grid.link_dir = 2;
    anchor_grid.link_sx = 73;
    anchor_grid.link_feet_sy = 81;
    float anchor_x = -1.0f, anchor_y = -1.0f;
    vox_chase_step(&anchor_grid, &anchor_x, &anchor_y);
    CHECK(anchor_x == 73.0f && anchor_y == 81.0f,
          "the chase step returns Link's live anchor");
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[2]) < 0.001f,
          "a fresh chase camera starts directly behind Link");

    /* -- camera body does not cross solid terrain -------------------- */
    VoxTileGrid collision_grid;
    floor_grid(&collision_grid);
    CHECK(fabsf(vox_chase_camera_back(&collision_grid, 72.0f, 80.0f,
                                      0.0f, -1.0f, 62.0f) - 62.0f) < 0.01f,
          "open ground keeps the requested camera distance");
    for (int x = 0; x < VOX_TILES_W; x++)
        collision_grid.height[16][x] = VOX_H_HIGH;
    float wall_back = vox_chase_camera_back(&collision_grid, 72.0f, 80.0f,
                                             0.0f, -1.0f, 62.0f);
    CHECK(wall_back >= 30.0f && wall_back < 48.0f,
          "a solid row behind Link clamps the camera before the wall");
    floor_grid(&collision_grid);
    collision_grid.height[15][13] = VOX_H_HIGH;
    float corner_back = vox_chase_camera_back(&collision_grid, 102.0f, 80.0f,
                                               0.0f, -1.0f, 62.0f);
    CHECK(corner_back < 62.0f,
          "the camera footprint catches a wall corner off its centre line");
    floor_grid(&collision_grid);
    collision_grid.height[16][9] = VOX_H_HIGH;
    collision_grid.treecell[16][9] = 1;
    CHECK(fabsf(vox_chase_camera_back(&collision_grid, 72.0f, 80.0f,
                                      0.0f, -1.0f, 62.0f) - 62.0f) < 0.01f,
          "volumetric tree cells do not shove the camera into Link");

    /* -- it trails him while he walks -------------------------------- */
    voxel_tuning_reset();
    CHECK(voxel_tuning()->chase_follow > 0.0f,
          "trailing ships on -- a chase camera that never chases is broken");

    for (int i = 0; i < 200; i++) frame(1, 60.0f + i, 100.0f);   /* runs east */
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[1]) < 0.02f,
          "walking east swings the camera round behind him");
    for (int i = 0; i < 200; i++) frame(0, 60.0f, 100.0f - i);   /* runs north */
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[0]) < 0.02f,
          "and follows him round the corner");

    /* Standing still and turning to talk to someone must not move it. */
    float held_still = voxel_chase_yaw();
    for (int d = 0; d < 4; d++) {
        for (int i = 0; i < 30; i++) frame(d, 60.0f, 100.0f);
    }
    CHECK(fabsf(voxel_chase_yaw() - held_still) < 0.0001f,
          "turning on the spot never moves it");

    /* -- the stick wins, and keeps winning for a moment --------------- */
    for (int i = 0; i < 200; i++) frame(2, 60.0f, 100.0f + i);   /* settle, walking south */
    float before = voxel_chase_yaw();
    voxel_chase_turn(1.0f);
    frame(2, 60.0f, 300.0f);
    float nudged = voxel_chase_yaw();
    CHECK(fabsf(nudged - before) > 0.01f, "the stick turns the camera");

    /* The old version resumed the instant the stick went still, which is
     * what made it feel like a fight. It has to hold for a beat. */
    for (int i = 0; i < 10; i++) frame(2, 60.0f, 310.0f + i);
    CHECK(fabsf(voxel_chase_yaw() - nudged) < 0.0001f,
          "it stays where the stick left it for a moment, mid-walk");

    /* ...and then eases back on its own rather than staying put forever. */
    for (int i = 0; i < 300; i++) frame(2, 60.0f, 400.0f + i);
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[2]) < 0.02f,
          "after the pause it trails him again");

    /* The ease is gradual: one frame of it must not close the whole gap. */
    voxel_chase_turn(6.0f);
    frame(2, 60.0f, 800.0f);
    float far_off = voxel_chase_yaw();
    for (int i = 0; i < 40; i++) frame(2, 60.0f, 810.0f + i);
    float part_way = voxel_chase_yaw();
    CHECK(ang_diff(part_way, far_off) > 0.01f &&
          ang_diff(part_way, DIR_YAW[2]) > 0.01f,
          "the swing is eased, not a snap");

    /* -- holding still is still available ----------------------------- */
    voxel_tuning()->chase_follow = 0.0f;
    float parked = voxel_chase_yaw();
    for (int i = 0; i < 200; i++) frame(1, 60.0f + i, 100.0f);
    CHECK(fabsf(voxel_chase_yaw() - parked) < 0.0001f,
          "trailing off holds whatever angle it was left at");
    voxel_tuning_reset();

    /* -- a tap swings round and finishes itself ----------------------- */
    voxel_chase_recenter(true);
    voxel_chase_recenter(false);            /* released after one frame */
    for (int i = 0; i < 200; i++) frame(2, 60.0f, 100.0f);        /* facing down */
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[2]) < 0.01f,
          "a recentre tap completes the swing after the button is up");

    /* Having arrived, it must stay while he stands there. */
    float landed = voxel_chase_yaw();
    for (int i = 0; i < 60; i++) frame(2, 60.0f, 100.0f);
    CHECK(fabsf(voxel_chase_yaw() - landed) < 0.0001f,
          "after recentring it holds still while he does");

    /* -- a held recentre keeps following ------------------------------ */
    for (int i = 0; i < 90; i++) {
        voxel_chase_recenter(true);
        frame(3, 60.0f, 100.0f);            /* facing left, held */
    }
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[3]) < 0.01f,
          "held recentre parks behind him");
    for (int i = 0; i < 90; i++) {
        voxel_chase_recenter(true);
        frame(1, 60.0f, 100.0f);            /* he turns around under it */
    }
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[1]) < 0.01f,
          "held recentre keeps following when he turns");
    voxel_chase_recenter(false);

    /* -- recentring is idempotent when already there ------------------ */
    for (int i = 0; i < 40; i++) { voxel_chase_recenter(true); frame(2, 60.0f, 100.0f); }
    voxel_chase_recenter(false);
    float settled = voxel_chase_yaw();
    for (int i = 0; i < 40; i++) { voxel_chase_recenter(true); frame(2, 60.0f, 100.0f); }
    voxel_chase_recenter(false);
    CHECK(fabsf(voxel_chase_yaw() - settled) < 0.0001f,
          "recentring again when already behind him is a no-op");

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
