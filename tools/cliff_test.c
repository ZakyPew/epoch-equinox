/* Checks Oracle quarter-cell decoding and the "one cliff, one height" pass.
 *
 * Collision values $01-$0F are four quadrant bits, not a height ladder.
 * Once those bits establish the real footprint, tile colours propose a
 * visual height. That vote is per 8x8 quadrant, so a connected cliff votes
 * once. What matters is that it unifies what really is one mass and does
 * NOT reach across gaps into a separate object.
 *
 * Build like vox_shot (it links the same libraries):
 *
 *   cc -O2 -o cliff_test ../tools/cliff_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 */
#include "voxel/voxel.h"
#include "voxel/voxel_internal.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

static uint8_t g[VOX_TILES_H][VOX_TILES_W];

static void clear_to(uint8_t h) {
    for (int y = 0; y < VOX_TILES_H; y++)
        for (int x = 0; x < VOX_TILES_W; x++) g[y][x] = h;
}

static bool row_all(int y, int x0, int x1, uint8_t h) {
    for (int x = x0; x <= x1; x++) if (g[y][x] != h) return false;
    return true;
}

static void make_floor_room(VoxTileGrid* grid) {
    memset(grid, 0, sizeof(*grid));
    grid->hud_rows = 16;
    for (int y = 0; y < VOX_TILES_H; y++)
        for (int x = 0; x < VOX_TILES_W; x++)
            grid->height[y][x] = VOX_H_FLOOR;
}

int main(void) {
    /* -- collision nibble is a quarter-cell occupancy mask ------------ */
    CHECK(vox_oracle_quadrant_height(0x0F, VOX_H_FLOOR, 0) == VOX_H_MID &&
          vox_oracle_quadrant_height(0x0F, VOX_H_FLOOR, 1) == VOX_H_MID &&
          vox_oracle_quadrant_height(0x0F, VOX_H_FLOOR, 2) == VOX_H_MID &&
          vox_oracle_quadrant_height(0x0F, VOX_H_FLOOR, 3) == VOX_H_MID,
          "$0F fills all four quadrants");
    CHECK(vox_oracle_quadrant_height(0x0C, VOX_H_HIGH, 0) == VOX_H_HIGH &&
          vox_oracle_quadrant_height(0x0C, VOX_H_HIGH, 1) == VOX_H_HIGH &&
          vox_oracle_quadrant_height(0x0C, VOX_H_HIGH, 2) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x0C, VOX_H_HIGH, 3) == VOX_H_FLOOR,
          "$0C fills the top half, not a short bevel");
    CHECK(vox_oracle_quadrant_height(0x03, VOX_H_FLOOR, 0) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x03, VOX_H_FLOOR, 1) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x03, VOX_H_FLOOR, 2) == VOX_H_MID &&
          vox_oracle_quadrant_height(0x03, VOX_H_FLOOR, 3) == VOX_H_MID,
          "$03 fills the bottom half");
    CHECK(vox_oracle_quadrant_height(0x0A, VOX_H_FLOOR, 0) == VOX_H_MID &&
          vox_oracle_quadrant_height(0x0A, VOX_H_FLOOR, 1) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x0A, VOX_H_FLOOR, 2) == VOX_H_MID &&
          vox_oracle_quadrant_height(0x0A, VOX_H_FLOOR, 3) == VOX_H_FLOOR,
          "$0A fills the left half");
    CHECK(vox_oracle_quadrant_height(0x05, VOX_H_FLOOR, 0) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x05, VOX_H_FLOOR, 1) == VOX_H_MID &&
          vox_oracle_quadrant_height(0x05, VOX_H_FLOOR, 2) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x05, VOX_H_FLOOR, 3) == VOX_H_MID,
          "$05 fills the right half");
    CHECK(vox_oracle_quadrant_height(0x08, VOX_H_HIGH, 0) == VOX_H_HIGH &&
          vox_oracle_quadrant_height(0x08, VOX_H_HIGH, 3) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x01, VOX_H_HIGH, 0) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x01, VOX_H_HIGH, 3) == VOX_H_HIGH,
          "corner bits preserve their screen positions");
    CHECK(vox_oracle_quadrant_height(0x00, VOX_H_HIGH, 0) == VOX_H_FLOOR &&
          vox_oracle_quadrant_height(0x10, VOX_H_HIGH, 2) == VOX_H_WATER &&
          vox_oracle_quadrant_height(0x11, VOX_H_HIGH, 3) == VOX_H_FLOOR,
          "walkable and special collision values keep their semantics");

    /* -- a ragged cliff top comes out flat ---------------------------- */
    clear_to(VOX_H_FLOOR);
    /* Ten cells of cliff; the colour classifier called three of them MID
     * because their art is shaded. That is the notch being fixed. */
    for (int x = 2; x <= 11; x++) g[5][x] = VOX_H_HIGH;
    g[5][4] = VOX_H_MID;
    g[5][7] = VOX_H_MID;
    g[5][8] = VOX_H_MID;
    vox_unify_solid_masses(g);
    CHECK(row_all(5, 2, 11, VOX_H_HIGH),
          "a mostly-tall cliff loses its notches");

    /* -- and the reverse: a rock shelf is not promoted ---------------- */
    clear_to(VOX_H_FLOOR);
    for (int x = 2; x <= 11; x++) g[5][x] = VOX_H_MID;
    g[5][6] = VOX_H_HIGH;                     /* one bright tile */
    vox_unify_solid_masses(g);
    CHECK(row_all(5, 2, 11, VOX_H_MID),
          "one bright tile does not raise a whole rock shelf");

    /* -- a tie goes tall ---------------------------------------------- */
    clear_to(VOX_H_FLOOR);
    for (int x = 2; x <= 5; x++) g[7][x] = VOX_H_HIGH;
    for (int x = 6; x <= 9; x++) g[7][x] = VOX_H_MID;
    vox_unify_solid_masses(g);
    CHECK(row_all(7, 2, 9, VOX_H_HIGH), "an even split goes tall");

    /* -- separate objects stay separate ------------------------------- */
    clear_to(VOX_H_FLOOR);
    for (int x = 2; x <= 5; x++) g[3][x] = VOX_H_HIGH;   /* cliff */
    for (int x = 8; x <= 10; x++) g[3][x] = VOX_H_MID;   /* rocks, gap of 2 */
    vox_unify_solid_masses(g);
    CHECK(row_all(3, 2, 5, VOX_H_HIGH) && row_all(3, 8, 10, VOX_H_MID),
          "a gap keeps two masses independent");

    /* -- diagonals do not join (4-connectivity) ----------------------- */
    clear_to(VOX_H_FLOOR);
    g[10][4] = VOX_H_HIGH;
    g[11][5] = VOX_H_MID;                     /* touching only at a corner */
    vox_unify_solid_masses(g);
    CHECK(g[10][4] == VOX_H_HIGH && g[11][5] == VOX_H_MID,
          "corner contact alone does not merge two masses");

    /* -- flat and water are never touched ----------------------------- */
    /* LOW is non-collision decoration and is never part of the mass vote. */
    clear_to(VOX_H_FLOOR);
    g[2][2] = VOX_H_WATER;
    g[2][3] = VOX_H_LOW;
    for (int x = 6; x <= 8; x++) g[2][x] = VOX_H_HIGH;
    vox_unify_solid_masses(g);
    CHECK(g[2][2] == VOX_H_WATER, "water is left alone");
    CHECK(g[2][3] == VOX_H_LOW, "low decoration stays low");
    CHECK(g[2][0] == VOX_H_FLOOR, "floor is left alone");

    /* -- an L-shaped mass is one mass --------------------------------- */
    clear_to(VOX_H_FLOOR);
    for (int x = 3; x <= 8; x++) g[12][x] = VOX_H_HIGH;
    for (int y = 13; y <= 16; y++) g[y][8] = VOX_H_MID;   /* the leg */
    vox_unify_solid_masses(g);
    /* 6 HIGH against 4 MID: the whole L goes tall. */
    CHECK(row_all(12, 3, 8, VOX_H_HIGH) && g[16][8] == VOX_H_HIGH,
          "an L-shaped mass votes as one");

    /* -- non-collision LOW decoration is never pulled into a cliff ---- */
    clear_to(VOX_H_FLOOR);
    for (int x = 2; x <= 11; x++) g[6][x] = VOX_H_HIGH;
    g[6][5] = VOX_H_LOW;
    g[6][8] = VOX_H_LOW;
    vox_unify_solid_masses(g);
    CHECK(g[6][5] == VOX_H_LOW && g[6][8] == VOX_H_LOW,
          "low decoration is not promoted into collision geometry");
    CHECK(g[6][4] == VOX_H_HIGH && g[6][9] == VOX_H_HIGH,
          "the wall around it stays tall");

    /* -- low decoration beside a mid mass also stays independent ------ */
    clear_to(VOX_H_FLOOR);
    g[8][3] = VOX_H_LOW;
    for (int x = 4; x <= 6; x++) g[8][x] = VOX_H_MID;   /* only rocks nearby */
    vox_unify_solid_masses(g);
    CHECK(g[8][3] == VOX_H_LOW,
          "low decoration beside a mid mass remains low");

    /* -- a full-grid mass does not run off the end -------------------- */
    clear_to(VOX_H_HIGH);
    vox_unify_solid_masses(g);
    CHECK(g[0][0] == VOX_H_HIGH && g[VOX_TILES_H - 1][VOX_TILES_W - 1] == VOX_H_HIGH,
          "a mass covering the whole grid is handled");

    /* -- enclosed ground meets the top of its actual cliff class ------ */
    {
        VoxTileGrid p;
        make_floor_room(&p);
        for (int x = 5; x <= 10; x++) {
            p.height[5][x] = VOX_H_MID;
            p.height[10][x] = VOX_H_MID;
        }
        for (int y = 6; y < 10; y++) {
            p.height[y][5] = VOX_H_MID;
            p.height[y][10] = VOX_H_MID;
        }
        p.height[7][7] = VOX_H_MID; /* compact prop on the shelf */
        vox_infer_plateaus(&p);
        CHECK(p.elevation[7][8] == VOX_H_MID,
              "floor enclosed by a mid cliff rises exactly to its lip");
        CHECK(p.elevation[7][7] == VOX_H_MID,
              "a compact prop inherits the shelf's mid base class");
        CHECK(p.elevation[5][7] == 0 && p.elevation[4][7] == 0,
              "the cliff lip and outside datum are not double-raised");

        for (int x = 5; x <= 10; x++)
            p.height[5][x] = p.height[10][x] = VOX_H_HIGH;
        for (int y = 6; y < 10; y++)
            p.height[y][5] = p.height[y][10] = VOX_H_HIGH;
        vox_infer_plateaus(&p);
        CHECK(p.elevation[7][8] == VOX_H_HIGH,
              "floor enclosed by a high cliff rises exactly to its lip");
        CHECK(p.elevation[7][7] == VOX_H_HIGH,
              "a compact prop inherits the shelf's high base class");
    }

    /* -- vegetation blocks movement, but does not change land level --- */
    {
        VoxTileGrid p;
        make_floor_room(&p);
        for (int x = 5; x <= 10; x++) {
            p.height[5][x] = p.height[10][x] = VOX_H_HIGH;
            p.treecell[5][x] = p.treecell[10][x] = 1;
        }
        for (int y = 6; y < 10; y++) {
            p.height[y][5] = p.height[y][10] = VOX_H_HIGH;
            p.treecell[y][5] = p.treecell[y][10] = 1;
        }
        vox_infer_plateaus(&p);
        CHECK(p.elevation[7][7] == 0,
              "a tree ring does not invent a plateau");
    }

    /* -- a water-separated island has no architectural height cue ----- */
    {
        VoxTileGrid p;
        make_floor_room(&p);
        for (int x = 5; x <= 10; x++)
            p.height[5][x] = p.height[10][x] = VOX_H_WATER;
        for (int y = 6; y < 10; y++)
            p.height[y][5] = p.height[y][10] = VOX_H_WATER;
        vox_infer_plateaus(&p);
        CHECK(p.elevation[7][7] == 0,
              "water alone does not turn an island into a plateau");
    }

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
