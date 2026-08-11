/* Exercises the chase camera's yaw rules without a cart or a window.
 *
 * The camera's feel is the whole point of these rules, and feel is exactly
 * what a screenshot cannot check. What IS checkable: that the camera stays
 * where it was put while Link runs around, that a recentre tap finishes
 * its swing on its own, that a held recentre keeps following, and that the
 * stick always wins.
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
    /* -- the camera stays where it is put ---------------------------- */
    voxel_tuning_reset();
    CHECK(voxel_tuning()->chase_follow == 0.0f,
          "auto-follow ships off");

    float start = voxel_chase_yaw();
    for (int i = 0; i < 120; i++) frame(1, 60.0f + i, 100.0f);   /* runs east */
    CHECK(fabsf(voxel_chase_yaw() - start) < 0.0001f,
          "walking never moves the camera on its own");

    for (int d = 0; d < 4; d++) {
        for (int i = 0; i < 30; i++) frame(d, 60.0f, 100.0f);
    }
    CHECK(fabsf(voxel_chase_yaw() - start) < 0.0001f,
          "turning on the spot never moves it either");

    /* -- a tap swings round and finishes itself ----------------------- */
    voxel_chase_recenter(true);
    voxel_chase_recenter(false);            /* released after one frame */
    for (int i = 0; i < 200; i++) frame(2, 60.0f, 100.0f);        /* facing down */
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[2]) < 0.01f,
          "a recentre tap completes the swing after the button is up");

    /* Having arrived, it must stay: no drift once the ease is spent. */
    float landed = voxel_chase_yaw();
    for (int i = 0; i < 60; i++) frame(0, 60.0f, 100.0f + i);     /* now walks north */
    CHECK(fabsf(voxel_chase_yaw() - landed) < 0.0001f,
          "after recentring it holds still again");

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

    /* -- the optional walk-follow still works when asked for ---------- */
    voxel_tuning()->chase_follow = 0.2f;
    for (int i = 0; i < 120; i++) frame(0, 60.0f, 100.0f - i);    /* walks north */
    CHECK(ang_diff(voxel_chase_yaw(), DIR_YAW[0]) < 0.01f,
          "chase_follow>0 restores the walk-follow");
    voxel_tuning_reset();

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
