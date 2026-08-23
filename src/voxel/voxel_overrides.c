/* Per-room voxel height overrides.
 *
 * The diorama raises what the game's collision grid says is solid --
 * which is right until a room decorates itself with props the player can
 * walk past: statue rows, gates, cave mouths. Those read as "should be
 * 3D" to a human and as $00 to the collision map. This module lets a
 * plain text file have the final word, per room.
 *
 * Files live in voxel/overrides/ next to the binary, one per room:
 *
 *     voxel/overrides/ages-0-6a.txt        (game, group, room in hex)
 *
 * containing (comments allowed, '#' to end of line) 8 rows of 10
 * characters, one per 16x16 room cell:
 *
 *     .  keep whatever collision decided     _  force flat
 *     w  water (sinks)   l  low   m  mid   h  high
 *
 * VOXEL_EDIT=1 writes a ready-to-edit template (all '.') for any room
 * you stand in that has no file yet, with the collision-derived heights
 * included as a comment so you can see what you are overriding.
 *
 * Editing is live: the file's timestamp is polled a few times a second
 * while you stand in the room, so saving in a text editor reshapes the
 * terrain in front of you. No restart, no leaving the room.
 */
#include "voxel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define vox_mkdir(p) _mkdir(p)
#else
#define vox_mkdir(p) mkdir(p, 0755)
#endif

#define OV_W 10
#define OV_H 8

typedef struct {
    bool valid;          /* cache entry in use */
    bool present;        /* file existed and parsed */
    bool is_seasons;
    int group, room;
    long mtime;          /* file timestamp the cache was built from */
    uint8_t grid[OV_H * OV_W];   /* height class, or 0xFF = keep */
} VoxOverride;

static VoxOverride g_cache;      /* one room at a time is plenty */

static void override_path(char* buf, size_t n, bool is_seasons, int group,
                          int room) {
    snprintf(buf, n, "voxel/overrides/%s-%d-%02x.txt",
             is_seasons ? "seasons" : "ages", group, room);
}

static bool parse_override(FILE* f, uint8_t* grid) {
    memset(grid, 0xFF, OV_H * OV_W);
    int row = 0, col = 0;
    int c;
    bool comment = false;
    while ((c = fgetc(f)) != EOF && row < OV_H) {
        if (c == '#') comment = true;
        if (c == '\n') {
            comment = false;
            if (col > 0) { row++; col = 0; }
            continue;
        }
        if (comment || c == ' ' || c == '\t' || c == '\r') continue;
        uint8_t v;
        switch (c) {
            case '.': v = 0xFF; break;
            case '_': v = VOX_H_FLOOR; break;
            case 'w': v = VOX_H_WATER; break;
            case 'l': v = VOX_H_LOW; break;
            case 'm': v = VOX_H_MID; break;
            case 'h': v = VOX_H_HIGH; break;
            default: return false;
        }
        if (col < OV_W) grid[row * OV_W + col] = v;
        col++;
    }
    return true;
}

/* Write a template for a room with no override yet, so authoring one is
 * "walk in with VOXEL_EDIT=1, open the file it made". */
static void write_template(const char* path, bool is_seasons, int group,
                           int room, const uint8_t* collisions) {
    vox_mkdir("voxel");
    vox_mkdir("voxel/overrides");
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# %s group %d room %02x\n", is_seasons ? "seasons" : "ages",
            group, room);
    fprintf(f, "# . keep   _ flat   w water   l low   m mid   h high\n");
    fprintf(f, "# collision-derived heights for reference:\n");
    for (int r = 0; r < OV_H; r++) {
        fprintf(f, "#   ");
        for (int c = 0; c < OV_W; c++) {
            uint8_t v = collisions ? collisions[r * 16 + c] : 0;
            fputc(v == 0x0F ? 'm' : (v == 0x10 ? 'w' :
                  (v >= 0x01 && v <= 0x0E) ? 'l' : '_'), f);
        }
        fputc('\n', f);
    }
    for (int r = 0; r < OV_H; r++) {
        fprintf(f, "..........\n");
    }
    fclose(f);
    fprintf(stderr, "[VOXEL] wrote override template %s\n", path);
}

/* Drop the cache so the next lookup re-reads the file. The in-game
 * editor calls this after writing: the poll compares mtimes, and two
 * paints inside the same second would otherwise leave the second one
 * invisible until the clock ticked over. */
void vox_override_invalidate(void) {
    g_cache.valid = false;
}

const uint8_t* vox_override_lookup(bool is_seasons, int group, int room,
                                   const uint8_t* collisions) {
    if (g_cache.valid && g_cache.is_seasons == is_seasons &&
        g_cache.group == group && g_cache.room == room) {
        /* Same room as last frame. Poll the file a couple of times a
         * second so an edit lands while you are standing in the room --
         * save the file, watch the terrain change. Cheap: this runs once
         * per frame, and a stat of a warm path costs microseconds. */
        static int poll = 0;
        if (++poll < 20) {
            return g_cache.present ? g_cache.grid : NULL;
        }
        poll = 0;
        char probe[256];
        override_path(probe, sizeof(probe), is_seasons, group, room);
        struct stat st;
        long now = (stat(probe, &st) == 0) ? (long)st.st_mtime : -1;
        if (now == g_cache.mtime) {
            return g_cache.present ? g_cache.grid : NULL;
        }
        /* Changed on disk: fall through and re-read it. */
    }

    g_cache.valid = true;
    g_cache.present = false;
    g_cache.is_seasons = is_seasons;
    g_cache.group = group;
    g_cache.room = room;

    char path[256];
    override_path(path, sizeof(path), is_seasons, group, room);
    {
        struct stat st;
        g_cache.mtime = (stat(path, &st) == 0) ? (long)st.st_mtime : -1;
    }
    FILE* f = fopen(path, "r");
    if (!f) {
        if (getenv("VOXEL_EDIT")) {
            write_template(path, is_seasons, group, room, collisions);
        }
        return NULL;
    }
    bool ok = parse_override(f, g_cache.grid);
    fclose(f);
    if (!ok) {
        fprintf(stderr, "[VOXEL] bad override file %s (ignored)\n", path);
        return NULL;
    }
    g_cache.present = true;
    return g_cache.grid;
}
