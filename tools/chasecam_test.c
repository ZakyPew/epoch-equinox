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

int main(void) {
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
