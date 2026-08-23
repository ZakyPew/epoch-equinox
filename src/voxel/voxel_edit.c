/* In-game room sculpting.
 *
 * The override files (voxel_overrides.c) let a text file reshape a
 * room's terrain, and they already reload live while you stand in the
 * room. This is the front half that was missing: instead of alt-tabbing
 * to a text editor, walk Link up to the cell you want to change and
 * press a number. The player rewrites the room's override file itself,
 * atomically, and the live reload redraws the terrain in front of you.
 *
 * The brush is the cell Link is FACING -- you can see it, and it means
 * standing on a ledge you just raised never wedges you inside it.
 *
 * Everything here is plain C with no SDL and no ImGui: the HUD
 * (voxel_edit_hud.cpp) reads keys and draws help, the tile builder
 * (voxel_tiles.c) reports where Link is and tints the target cell, and
 * this file owns the state and the file writing -- which is why
 * tools/voxedit_test.c can drive the whole thing headlessly.
 */
#include "voxel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define ve_mkdir(p) _mkdir(p)
#else
#define ve_mkdir(p) mkdir(p, 0755)
#endif

#define VE_W 10
#define VE_H 8

static bool g_on = false;

/* The room being edited and the cell the brush is over. Tracked every
 * frame by the tile builder; painting always lands on what you can see. */
static struct {
    bool valid;
    bool is_seasons;
    int  group, room;
    int  col, row;               /* the brush cell, 0-9 x 0-7 */
    int  link_col, link_row;     /* where Link himself stands  */
} g_cur;

/* The room's override characters, as they stand on disk ('.' = keep).
 * Loaded when the tracked room changes, rewritten whole on every paint:
 * patching text in place is how half-written files happen. */
static char g_grid[VE_H][VE_W];
static bool g_grid_room_valid = false;
static bool g_grid_is_seasons;
static int  g_grid_group = -1, g_grid_room = -1;

/* Collision-derived heights, copied not pointed at -- the frame that
 * handed them over is long gone by the time a key is pressed. Written
 * into the file as a reference comment, same as the old templates. */
static uint8_t g_collisions[16 * 12];
static bool    g_have_collisions = false;

void vox_edit_set_enabled(bool on) { g_on = on; }
bool vox_edit_enabled(void) { return g_on; }

static void ve_path(char* buf, size_t n, bool is_seasons, int group, int room) {
    snprintf(buf, n, "voxel/overrides/%s-%d-%02x.txt",
             is_seasons ? "seasons" : "ages", group, room);
}

/* Read the room's existing override file into g_grid, tolerant of the
 * comments and whitespace the format allows. Anything missing or
 * malformed starts the grid at all-keep rather than failing: the next
 * paint rewrites the file cleanly either way. */
static void ve_load_grid(bool is_seasons, int group, int room) {
    memset(g_grid, '.', sizeof(g_grid));
    g_grid_is_seasons = is_seasons;
    g_grid_group = group;
    g_grid_room = room;
    g_grid_room_valid = true;

    char path[256];
    ve_path(path, sizeof(path), is_seasons, group, room);
    FILE* f = fopen(path, "r");
    if (!f) return;
    int c, row = 0, col = 0;
    bool comment = false;
    while ((c = fgetc(f)) != EOF && row < VE_H) {
        if (c == '#') comment = true;
        if (c == '\n') {
            comment = false;
            if (col > 0) { row++; col = 0; }
            continue;
        }
        if (comment || c == ' ' || c == '\t' || c == '\r') continue;
        if (!strchr("._wlmh", c)) { fclose(f); return; }  /* keep all-'.') */
        if (col < VE_W) g_grid[row][col] = (char)c;
        col++;
    }
    fclose(f);
}

/* One tick per frame -- the tile builder calls track exactly once per
 * grid build, which makes this the frame clock the pulse wants. The
 * brush cell spans four 8px tiles, so counting inside the tile loop
 * would beat four times too fast. */
static int g_pulse = 0;

int vox_edit_pulse(void) {
    /* 0.30 to 0.62 of the way to gold, on a ~1s triangle. */
    int tri = g_pulse % 60;
    if (tri >= 30) tri = 60 - tri;
    return 77 + (tri * 82) / 30;
}

void vox_edit_track(bool is_seasons, int group, int room,
                    int link_col, int link_row, int dir,
                    const uint8_t* collisions) {
    /* VOXEL_EDIT=1 arms sculpting from boot -- the old template-writing
     * env var, grown up. Checked once; F4 and the menu own it after. */
    static bool env_checked = false;
    if (!env_checked) {
        env_checked = true;
        const char* e = getenv("VOXEL_EDIT");
        if (e && *e && *e != '0') g_on = true;
    }
    if (getenv("VOXEL_EDIT_TRACE")) {
        static int n = 0;
        if ((n++ % 30) == 0)
            fprintf(stderr, "[EDIT] track on=%d room=%d-%02x link=(%d,%d) dir=%d\n",
                    (int)g_on, group, room, link_col, link_row, dir);
    }
    if (!g_on) { g_cur.valid = false; return; }
    g_pulse++;

    /* The brush is one cell ahead of Link's facing. */
    static const int DX[4] = { 0, 1, 0, -1 };
    static const int DY[4] = { -1, 0, 1, 0 };
    int col = link_col + DX[dir & 3];
    int row = link_row + DY[dir & 3];
    if (col < 0) col = 0;
    if (col >= VE_W) col = VE_W - 1;
    if (row < 0) row = 0;
    if (row >= VE_H) row = VE_H - 1;

    if (!g_grid_room_valid || g_grid_is_seasons != is_seasons ||
        g_grid_group != group || g_grid_room != room) {
        ve_load_grid(is_seasons, group, room);
    }
    if (collisions) {
        memcpy(g_collisions, collisions, sizeof(g_collisions));
        g_have_collisions = true;
    }

    g_cur.valid = true;
    g_cur.is_seasons = is_seasons;
    g_cur.group = group;
    g_cur.room = room;
    g_cur.col = col;
    g_cur.row = row;
    g_cur.link_col = link_col;
    g_cur.link_row = link_row;
}

bool vox_edit_cursor(int* col, int* row) {
    if (!g_on || !g_cur.valid) return false;
    if (col) *col = g_cur.col;
    if (row) *row = g_cur.row;
    return true;
}

char vox_edit_cell(void) {
    if (!g_cur.valid || !g_grid_room_valid) return '.';
    return g_grid[g_cur.row][g_cur.col];
}

bool vox_edit_room(bool* is_seasons, int* group, int* room) {
    if (!g_cur.valid) return false;
    if (is_seasons) *is_seasons = g_cur.is_seasons;
    if (group) *group = g_cur.group;
    if (room) *room = g_cur.room;
    return true;
}

/* Write the whole file: header, the collision reference comment the old
 * templates carried, then the grid. Beside-and-rename, because the
 * overrides module stats this path a few times a second while the room
 * is on screen and half a file would parse as garbage. */
static bool ve_write(void) {
    ve_mkdir("voxel");
    ve_mkdir("voxel/overrides");

    char path[256], tmp[272];
    ve_path(path, sizeof(path), g_cur.is_seasons, g_cur.group, g_cur.room);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "# %s group %d room %02x -- written by the in-game editor\n",
            g_cur.is_seasons ? "seasons" : "ages", g_cur.group, g_cur.room);
    fprintf(f, "# . keep   _ flat   w water   l low   m mid   h high\n");
    if (g_have_collisions) {
        fprintf(f, "# collision-derived heights for reference:\n");
        for (int r = 0; r < VE_H; r++) {
            fprintf(f, "#   ");
            for (int c = 0; c < VE_W; c++) {
                uint8_t v = g_collisions[r * 16 + c];
                fputc(v == 0x0F ? 'm' : (v == 0x10 ? 'w' :
                      (v >= 0x01 && v <= 0x0E) ? 'l' : '_'), f);
            }
            fputc('\n', f);
        }
    }
    for (int r = 0; r < VE_H; r++) {
        fwrite(g_grid[r], 1, VE_W, f);
        fputc('\n', f);
    }
    fclose(f);
#ifdef _WIN32
    remove(path);   /* Windows rename refuses to replace */
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

bool vox_edit_paint(char code) {
    if (!g_on || !g_cur.valid || !g_grid_room_valid) return false;
    if (!strchr("._wlmh", code)) return false;
    if (g_grid[g_cur.row][g_cur.col] == code) return true;  /* nothing to do */
    g_grid[g_cur.row][g_cur.col] = code;
    if (!ve_write()) {
        fprintf(stderr, "[VOXEL] cannot write override for room %d-%02x\n",
                g_cur.group, g_cur.room);
        return false;
    }
    /* mtime has one-second granularity; two paints in the same second
     * must not leave the second invisible until the clock ticks. */
    vox_override_invalidate();
    return true;
}
