/* Exercises the in-game room sculptor without a cart or a window.
 *
 * The editor's whole job is writing files the override loader reads
 * back, so the loop that matters is closed right here: paint a cell,
 * then ask vox_override_lookup -- the code the renderer actually calls
 * -- what the room looks like now. Runs in a temp directory, because
 * both halves use paths relative to the working directory.
 *
 * Build like the other probes (it links the same libraries):
 *
 *   cc -O2 -o voxedit_test ../tools/voxedit_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 */
#include "voxel/voxel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

/* A fake room: border of walls, floor inside, one pond cell. */
static uint8_t collisions[16 * 12];
static void make_room(void) {
    memset(collisions, 0, sizeof(collisions));
    for (int c = 0; c < 10; c++) {
        collisions[0 * 16 + c] = 0x0F;
        collisions[7 * 16 + c] = 0x0F;
    }
    collisions[3 * 16 + 4] = 0x10;   /* water */
}

/* Track with Link at (link_col, link_row) facing dir. */
static void stand(int link_col, int link_row, int dir) {
    vox_edit_track(false, 0, 0x6a, link_col, link_row, dir, collisions);
}

int main(void) {
    char dir[] = "/tmp/voxedit_test_XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) {
        printf("FAIL cannot make a temp dir\n");
        return 1;
    }
    make_room();

    /* -- off means off -------------------------------------------------- */
    stand(4, 3, 1);
    CHECK(!vox_edit_cursor(NULL, NULL),
          "with sculpting off there is no brush");
    CHECK(!vox_edit_paint('m'), "and painting is refused");

    /* -- the brush is the cell Link faces -------------------------------- */
    vox_edit_set_enabled(true);
    int col, row;
    stand(4, 3, 1);                       /* facing right */
    CHECK(vox_edit_cursor(&col, &row) && col == 5 && row == 3,
          "facing right, the brush is one cell right");
    stand(4, 3, 0);                       /* facing up */
    CHECK(vox_edit_cursor(&col, &row) && col == 4 && row == 2,
          "facing up, one cell up");
    stand(0, 0, 3);                       /* facing left at the wall */
    CHECK(vox_edit_cursor(&col, &row) && col == 0 && row == 0,
          "the brush never leaves the room");

    /* -- painting reaches the renderer ----------------------------------- */
    stand(4, 3, 1);                       /* brush at 5,3 */
    CHECK(vox_edit_paint('m'), "painting mid succeeds");
    {
        const uint8_t* ov = vox_override_lookup(false, 0, 0x6a, collisions);
        CHECK(ov != NULL, "the override loader sees the new file");
        CHECK(ov && ov[3 * 10 + 5] == VOX_H_MID,
              "and the painted cell reads back as mid");
        CHECK(ov && ov[3 * 10 + 4] == 0xFF,
              "while its neighbour is still 'keep'");
    }

    /* -- a second paint in the same second still lands -------------------- *
     * mtime is whole seconds; without the cache invalidation the loader
     * would keep serving the first paint until the clock ticked. */
    stand(4, 3, 2);                       /* brush at 4,4 */
    CHECK(vox_edit_paint('h'), "a second cell paints immediately");
    {
        const uint8_t* ov = vox_override_lookup(false, 0, 0x6a, collisions);
        CHECK(ov && ov[4 * 10 + 4] == VOX_H_HIGH,
              "the loader sees it in the same second");
        CHECK(ov && ov[3 * 10 + 5] == VOX_H_MID,
              "and the first cell survived the rewrite");
    }

    /* -- erasing puts collision back in charge ---------------------------- */
    stand(4, 3, 1);
    CHECK(vox_edit_paint('.'), "painting keep succeeds");
    {
        const uint8_t* ov = vox_override_lookup(false, 0, 0x6a, collisions);
        CHECK(ov && ov[3 * 10 + 5] == 0xFF,
              "the erased cell defers to collision again");
    }

    /* -- garbage is refused ----------------------------------------------- */
    CHECK(!vox_edit_paint('x'), "an unknown code is refused");
    CHECK(!vox_edit_paint('\n'), "so is a control character");

    /* -- the file is honest ------------------------------------------------ */
    {
        FILE* f = fopen("voxel/overrides/ages-0-6a.txt", "r");
        CHECK(f != NULL, "the file is where the docs say");
        if (f) {
            char text[2048];
            size_t n = fread(text, 1, sizeof(text) - 1, f);
            text[n] = 0;
            fclose(f);
            CHECK(strstr(text, "# . keep") != NULL,
                  "it carries the legend a hand editor needs");
            CHECK(strstr(text, "collision-derived") != NULL,
                  "and the collision reference comment");
            CHECK(strstr(text, "....h.....") != NULL,
                  "and the painted grid itself");
        }
        CHECK(fopen("voxel/overrides/ages-0-6a.txt.tmp", "r") == NULL,
              "no temp file is left behind");
    }

    /* -- hand edits are respected, not clobbered --------------------------- */
    {
        FILE* f = fopen("voxel/overrides/ages-0-1b.txt", "w");
        fprintf(f, "# hand-made\nllllllllll\n..........\n..........\n"
                   "..........\n..........\n..........\n..........\n"
                   "..........\n");
        fclose(f);
        vox_edit_track(false, 0, 0x1b, 4, 3, 1, collisions);
        CHECK(vox_edit_cell() == '.',
              "entering a hand-edited room reads its file");
        vox_edit_track(false, 0, 0x1b, 4, 0, 1, collisions);
        CHECK(vox_edit_cell() == 'l',
              "including the cells it authored");
        CHECK(vox_edit_paint('w'), "painting into it works");
        const uint8_t* ov = vox_override_lookup(false, 0, 0x1b, collisions);
        CHECK(ov && ov[0 * 10 + 0] == VOX_H_LOW,
              "and the hand-authored cells survive the rewrite");
        CHECK(ov && ov[0 * 10 + 5] == VOX_H_WATER,
              "alongside the new paint");
    }

    /* -- undo walks the paint trail backwards, per room --------------------- *
     * The sections above left a trail: in room 6a, mid onto (5,3), high
     * onto (4,4), then keep back onto (5,3); in room 1b, water onto a
     * hand-authored 'l' cell. Undo must pop each room's own edits,
     * newest first, and leave the other room's alone. */
    {
        /* Still standing in room 1b, where one paint happened. */
        vox_edit_track(false, 0, 0x1b, 4, 0, 1, collisions);
        CHECK(vox_edit_undo_count() == 1, "room 1b has one undoable paint");
        CHECK(vox_edit_undo(), "and undoing it succeeds");
        const uint8_t* ov = vox_override_lookup(false, 0, 0x1b, collisions);
        CHECK(ov && ov[0 * 10 + 5] == VOX_H_LOW,
              "the hand-authored value it painted over comes back");
        CHECK(!vox_edit_undo(),
              "a second undo here is refused: room 6a's edits are not ours");

        /* Back to room 6a: its three paints unwind newest-first. */
        stand(4, 3, 1);
        CHECK(vox_edit_undo_count() == 3, "room 6a still holds its three");
        CHECK(vox_edit_undo(), "undo one");
        ov = vox_override_lookup(false, 0, 0x6a, collisions);
        CHECK(ov && ov[3 * 10 + 5] == VOX_H_MID,
              "the keep-erase reverts: mid is back");
        CHECK(vox_edit_undo(), "undo two");
        ov = vox_override_lookup(false, 0, 0x6a, collisions);
        CHECK(ov && ov[4 * 10 + 4] == 0xFF,
              "the high paint reverts to keep");
        CHECK(vox_edit_undo(), "undo three");
        ov = vox_override_lookup(false, 0, 0x6a, collisions);
        CHECK(ov && ov[3 * 10 + 5] == 0xFF,
              "the first paint reverts too: the room is untouched again");
        CHECK(!vox_edit_undo() && vox_edit_undo_count() == 0,
              "and the trail is empty");
    }

    /* -- the pulse stays in its band, and off means off again --------------- */
    {
        for (int i = 0; i < 130; i++) {
            stand(4, 3, 1);
            int p = vox_edit_pulse();
            if (p < 77 || p > 159) {
                CHECK(false, "pulse stays inside its blend band");
                break;
            }
            if (i == 129) CHECK(true, "pulse stays inside its blend band");
        }
        vox_edit_set_enabled(false);
        CHECK(!vox_edit_cursor(NULL, NULL), "disabling clears the brush");
        CHECK(!vox_edit_paint('m'), "and painting is refused again");
        CHECK(!vox_edit_undo(), "and so is undo");
    }

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
