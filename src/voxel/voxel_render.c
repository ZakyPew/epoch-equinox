/* Diorama renderer + frame hook for the voxel mode.
 *
 * The scheme is the classic "voxel space" column march, adapted to a
 * screen-sized diorama: for each screen column, walk the visible map from
 * far (top) to near (bottom), project each cell's top surface under the
 * tilted camera, and fill the gap a height drop opens with that cell's
 * front wall. Painter's order per column — near rows overdraw far rows —
 * so no depth buffer is needed.
 *
 * The whole scene is rendered at an integer SCALE above the GB screen
 * (default 3x, VOXEL_SCALE=1..4 to override) and handed to the runtime
 * through the scaled frame hook. Texels stay chunky -- that is the pixel
 * art -- but silhouettes, domes and the camera tilt resolve at sub-GB
 * precision instead of a 160x144 staircase.
 *
 * The terrain texture is decoded straight from the BG tilemap (see
 * VoxTileGrid::tex), which keeps every palette, season tint and animation
 * exactly as the cart drew it -- without the sprites the composed frame
 * bakes in. Sprites are re-decoded from OAM/VRAM and stood upright as
 * billboards, interleaved with the terrain rows in painter's order so a
 * wall in front of a character actually hides them.
 */
#include "voxel.h"
#include "voxel_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "platform_sdl.h"

#ifdef GB_HAS_SDL2
#include <SDL.h>
#endif

#define VOX_MAX_SCALE 4

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static int g_mode = VOXEL_MODE_OFF;
static float g_chase_yaw = -1.5708f;   /* camera heading, radians */
static bool g_chase_heading_live = false;
static bool g_chase_pose_reset = true;
static const float CHASE_DIR_YAW[4] = {
    -1.5707963f, 0.0f, 1.5707963f, 3.1415927f
};
/* Recentring: `held` is the button's live state, `active` survives the
 * release so a tap finishes its swing instead of stopping half way. */
static bool  g_recentre_held = false;
static bool  g_recentring = false;
static int g_scale = 3;
static uint32_t g_out[GB_FRAMEBUFFER_SIZE * VOX_MAX_SCALE * VOX_MAX_SCALE];
static VoxTileGrid g_grid;
static VoxSpriteList g_sprites;

/* Per-mode camera: row squash (cos pitch) and per-height-unit lift in
 * pixels (sin pitch, scaled to taste). */
typedef struct { float squash; float lift; } VoxCam;
static const VoxCam VOX_CAMS[VOXEL_MODE_COUNT] = {
    {1.00f, 0.0f},   /* OFF (unused) */
    {0.966f, 1.1f},  /* 15 deg */
    {0.866f, 2.2f},  /* 30 deg */
    {0.707f, 3.2f},  /* 45 deg */
};

/* Shape and camera constants, live-tunable from the Esc menu. Defaults
 * are the values these had as compile-time constants.
 *
 * g_tune starts AT the defaults rather than zero: direct callers like
 * vox_shot use vox_render without going through voxel_install(), and a
 * zeroed tuning block is a dead renderer (FOV 0 -- every chase ray
 * degenerate, every height flat). */
#define VOX_TUNE_INIT {                     \
    .units = {                              \
        [VOX_H_WATER] = -1.5f,              \
        [VOX_H_FLOOR] = 0.0f,               \
        [VOX_H_LOW]   = 1.0f,               \
        [VOX_H_MID]   = 3.0f,               \
        [VOX_H_HIGH]  = 6.0f,               \
    },                                      \
    .footprint    = 3.0f,                   \
    .tilt_scale   = 1.0f,                   \
    .chase_back   = 62.0f,                  \
    .chase_height = 36.0f,                  \
    .chase_fov    = 0.62f,                  \
    .chase_hpx    = 3.2f,                   \
    .chase_follow = 0.05f,                  \
    .chase_recenter = 0.16f,                \
    .cliff_unify = 1.0f,                    \
    .fog_start    = 70.0f,                  \
    .fog_max      = 150.0f,                 \
    .bevel        = 0.55f,                  \
    .dof          = 0.55f,                  \
}

static VoxelTuning g_tune = VOX_TUNE_INIT;
static const VoxelTuning VOX_TUNE_DEFAULTS = VOX_TUNE_INIT;

/* VOX_UNITS was an array; keep the spelling so the call sites read the
 * same, but source the numbers from the live block. */
#define VOX_UNITS (g_tune.units)

VoxelTuning* voxel_tuning(void) { return &g_tune; }

float voxel_chase_yaw(void) { return g_chase_yaw; }

/* Set by voxel_chase_turn, consumed and cleared by the next step. */
static float g_turn_request = 0.0f;
/* Frames left before the camera may drift again; see the step below. */
static int   g_manual_hold = 0;

void voxel_chase_turn(float amount) { g_turn_request += amount; }

void voxel_chase_recenter(bool held) {
    if (held && !g_recentre_held) g_recentring = true;   /* on the press */
    g_recentre_held = held;
    if (held) g_recentring = true;
}

void voxel_tuning_reset(void) { g_tune = VOX_TUNE_DEFAULTS; }

static const char* TUNE_PATH = "voxel/tuning.ini";

void voxel_tuning_save(void) {
#ifdef _WIN32
    _mkdir("voxel");
#else
    mkdir("voxel", 0755);
#endif
    FILE* f = fopen(TUNE_PATH, "w");
    if (!f) return;
    fprintf(f, "# Epoch & Equinox voxel tuning. Delete this file to go back\n"
               "# to the shipped defaults, or edit it -- the Esc menu writes it.\n");
    fprintf(f, "water=%.3f\nlow=%.3f\nmid=%.3f\nhigh=%.3f\n",
            g_tune.units[VOX_H_WATER], g_tune.units[VOX_H_LOW],
            g_tune.units[VOX_H_MID], g_tune.units[VOX_H_HIGH]);
    fprintf(f, "footprint=%.3f\ntilt_scale=%.3f\n",
            g_tune.footprint, g_tune.tilt_scale);
    fprintf(f, "chase_back=%.3f\nchase_height=%.3f\nchase_fov=%.3f\n"
               "chase_hpx=%.3f\nchase_follow=%.3f\nchase_recenter=%.3f\n"
               "cliff_unify=%.0f\n"
               "fog_start=%.3f\n"
               "fog_max=%.3f\n",
            g_tune.chase_back, g_tune.chase_height, g_tune.chase_fov,
            g_tune.chase_hpx, g_tune.chase_follow,
            g_tune.chase_recenter, g_tune.cliff_unify, g_tune.fog_start,
            g_tune.fog_max);
    fprintf(f, "bevel=%.3f\ndof=%.3f\n", g_tune.bevel, g_tune.dof);
    fclose(f);
    fprintf(stderr, "[VOXEL] tuning saved to %s\n", TUNE_PATH);
}

void voxel_tuning_load(void) {
    voxel_tuning_reset();
    FILE* f = fopen(TUNE_PATH, "r");
    if (!f) return;
    char key[64];
    float val;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        if (sscanf(line, "%63[^=]=%f", key, &val) != 2) continue;
        if      (!strcmp(key, "water"))        g_tune.units[VOX_H_WATER] = val;
        else if (!strcmp(key, "low"))          g_tune.units[VOX_H_LOW]   = val;
        else if (!strcmp(key, "mid"))          g_tune.units[VOX_H_MID]   = val;
        else if (!strcmp(key, "high"))         g_tune.units[VOX_H_HIGH]  = val;
        else if (!strcmp(key, "footprint"))    g_tune.footprint    = val;
        else if (!strcmp(key, "tilt_scale"))   g_tune.tilt_scale   = val;
        else if (!strcmp(key, "chase_back"))   g_tune.chase_back   = val;
        else if (!strcmp(key, "chase_height")) g_tune.chase_height = val;
        else if (!strcmp(key, "chase_fov"))    g_tune.chase_fov    = val;
        else if (!strcmp(key, "chase_hpx"))    g_tune.chase_hpx    = val;
        else if (!strcmp(key, "chase_follow")) g_tune.chase_follow = val;
        else if (!strcmp(key, "chase_recenter")) g_tune.chase_recenter = val;
        else if (!strcmp(key, "cliff_unify")) g_tune.cliff_unify = val;
        else if (!strcmp(key, "fog_start"))    g_tune.fog_start    = val;
        else if (!strcmp(key, "fog_max"))      g_tune.fog_max      = val;
        else if (!strcmp(key, "bevel"))        g_tune.bevel        = val;
        else if (!strcmp(key, "dof"))          g_tune.dof          = val;
    }
    fclose(f);
    fprintf(stderr, "[VOXEL] tuning loaded from %s\n", TUNE_PATH);
}

int voxel_get_mode(void) { return g_mode; }

int vox_scene_render_mode(int requested_mode, bool scripted_scene) {
    /* A chase camera is attached to Link's gameplay object. Scripts can
     * freeze, hide, replace or move that object independently of the scene,
     * so preserve the room's authored framing with a fixed voxel camera. */
    if (requested_mode == VOXEL_MODE_CHASE && scripted_scene)
        return VOXEL_MODE_30;
    return requested_mode;
}

void voxel_set_mode(int mode) {
    if (mode < 0 || mode >= VOXEL_MODE_COUNT) mode = VOXEL_MODE_OFF;
    if (mode != g_mode) {
        if (mode == VOXEL_MODE_CHASE) {
            g_chase_heading_live = false;
            g_chase_pose_reset = true;
        }
        g_mode = mode;
        static const char* names[VOXEL_MODE_COUNT] = {"OFF", "15", "30", "45",
                                                      "CHASE"};
        fprintf(stderr, "[VOXEL] mode: %s\n", names[mode]);
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint32_t shade(uint32_t c, int mul /* 0..~272 */) {
    /* Clamp per channel: brightening (mul > 256) overflowed bright
     * channels into their neighbours' bytes, which is why top-lit yellow
     * flowers came out the wrong colour entirely. */
    uint32_t r = ((c & 0x0000FF) * (uint32_t)mul) >> 8;
    uint32_t g = (((c >> 8) & 0xFF) * (uint32_t)mul) >> 8;
    uint32_t b = (((c >> 16) & 0xFF) * (uint32_t)mul) >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* The pixel-cube bevel: treat every source pixel as a tiny block by
 * lighting its top-left edge and shading its bottom-right one, from the
 * fractional position of a sample inside its texel. This is what turns
 * smooth extruded terrain into the built-from-cubes look -- the art's
 * own colours still do all the drawing, the edges only modulate them. */
static inline uint32_t bevel_px(uint32_t c, float u, float v) {
    if (g_tune.bevel <= 0.01f) return c;
    float fu = u - (float)(int)u;
    float fv = v - (float)(int)v;
    const float e = 0.30f;
    int k = (int)(g_tune.bevel * 44.0f);
    if (fu > 1.0f - e || fv > 1.0f - e) return shade(c, 256 - k);
    if (fu < e || fv < e) return shade(c, 256 + (k * 3) / 4);
    return c;
}

/* Cube shading for object faces (trees, tufts, structures): orientation
 * sets the base light, and a faint checker across the face's pixel grid
 * makes each 1px quad read as its own block rather than wallpaper. */
static inline int cube_face_shade(int base, int u, int v) {
    if (g_tune.bevel <= 0.01f) return base;
    int k = (int)(g_tune.bevel * 20.0f);
    return base + (((u + v) & 1) ? -k : k / 3);
}

/* Tilt-shift depth of field: a separable blur whose radius grows with
 * distance from a focus row, zero inside the focus band. This is the
 * diorama-photo look -- the world is sharp where the subject stands and
 * melts a little at the frame's edges. Runs on the world band only; the
 * HUD and dialog are composited after and stay crisp. */
static uint32_t s_dof_tmp[GB_FRAMEBUFFER_SIZE * VOX_MAX_SCALE * VOX_MAX_SCALE];

static inline int dof_radius(int Y, int focus, int OH, int S) {
    float t = (float)(Y - focus) / ((float)OH * 0.40f);
    if (t < 0.0f) t = -t;
    t -= 0.28f;                       /* the sharp band around the focus */
    if (t <= 0.0f) return 0;
    float r = t * t * g_tune.dof * 6.0f * (float)S;
    int ri = (int)r;
    int rmax = 3 * S;
    return ri > rmax ? rmax : ri;
}

static void vox_tilt_shift(uint32_t* out, int OW, int OH, int S,
                           int top, int focus) {
    if (g_tune.dof <= 0.01f) return;
    /* Horizontal pass into the temp buffer, sliding-window box sum. */
    for (int Y = top; Y < OH; Y++) {
        int r = dof_radius(Y, focus, OH, S);
        uint32_t* dst = &s_dof_tmp[Y * OW];
        const uint32_t* src = &out[Y * OW];
        if (r == 0) {
            memcpy(dst, src, (size_t)OW * sizeof(uint32_t));
            continue;
        }
        int n = 2 * r + 1;
        unsigned sr = 0, sg = 0, sb = 0;
        for (int X = -r; X <= r; X++) {
            uint32_t c = src[X < 0 ? 0 : X];
            sr += c & 0xFF; sg += (c >> 8) & 0xFF; sb += (c >> 16) & 0xFF;
        }
        for (int X = 0; X < OW; X++) {
            dst[X] = 0xFF000000u | ((sb / (unsigned)n) << 16)
                     | ((sg / (unsigned)n) << 8) | (sr / (unsigned)n);
            uint32_t add = src[X + r + 1 >= OW ? OW - 1 : X + r + 1];
            uint32_t del = src[X - r < 0 ? 0 : X - r];
            sr += (add & 0xFF) - (del & 0xFF);
            sg += ((add >> 8) & 0xFF) - ((del >> 8) & 0xFF);
            sb += ((add >> 16) & 0xFF) - ((del >> 16) & 0xFF);
        }
    }
    /* Vertical pass back into the frame; the radius varies per row, so a
     * plain windowed sum per pixel keeps it exact. */
    for (int Y = top; Y < OH; Y++) {
        int r = dof_radius(Y, focus, OH, S);
        uint32_t* dst = &out[Y * OW];
        const uint32_t* t0 = &s_dof_tmp[Y * OW];
        if (r == 0) {
            memcpy(dst, t0, (size_t)OW * sizeof(uint32_t));
            continue;
        }
        for (int X = 0; X < OW; X++) {
            unsigned sr = 0, sg = 0, sb = 0;
            for (int dy = -r; dy <= r; dy++) {
                int YY = Y + dy;
                if (YY < top) YY = top;
                if (YY >= OH) YY = OH - 1;
                uint32_t c = s_dof_tmp[YY * OW + X];
                sr += c & 0xFF; sg += (c >> 8) & 0xFF; sb += (c >> 16) & 0xFF;
            }
            unsigned n = (unsigned)(2 * r + 1);
            dst[X] = 0xFF000000u | ((sb / n) << 16) | ((sg / n) << 8)
                     | (sr / n);
        }
    }
}

/* While the chase camera renders, tree cells drop out of the heightfield:
 * their original tile art is drawn as a separate relief volume, so the
 * ground plane runs beneath it. The diorama never sets this and keeps its
 * terrain extrusion. */
static bool g_flatten_trees = false;

static inline float cell_base(const VoxTileGrid* grid, int tx, int ty) {
    uint8_t base_class = grid->elevation[ty][tx];
    return base_class >= VOX_H_FLOOR && base_class <= VOX_H_HIGH
        ? VOX_UNITS[base_class] : 0.0f;
}

/* Complete surface height of one tile, clamped to the grid. Plateau height
 * is independent from object extrusion: a tree can be flattened to the
 * shelf beneath it, while a cliff rim and the walkable shelf behind it meet
 * at the same top elevation. */
static inline float cell_h(const VoxTileGrid* grid, int tx, int ty) {
    if (tx < 0) tx = 0;
    if (tx >= VOX_TILES_W) tx = VOX_TILES_W - 1;
    if (ty < 0) ty = 0;
    if (ty >= VOX_TILES_H) ty = VOX_TILES_H - 1;
    float base = cell_base(grid, tx, ty);
    if (g_flatten_trees &&
        (grid->treecell[ty][tx] || grid->structurecell[ty][tx]))
        return base;
    return base + VOX_UNITS[grid->height[ty][tx]];
}

/* Height at a world (screen-space) position, with a footprint for growing
 * things.
 *
 * Raising a vegetation cell to its full height across its whole square is
 * what made forests read as terraced mesas: the grass around a tree rose
 * with the tree. Foliage now tapers to ground level at whichever of its
 * edges face something shorter, so a lone tree becomes a rounded mass
 * standing IN the meadow while a run of adjacent trees keeps its shared
 * interior flat and only rounds off at the outside of the clump.
 *
 * This replaces the old per-cell dome, which put a peak in the middle of
 * every cell -- charming from above, and the source of the striped
 * shark-fins when the chase camera looked at a treeline edge-on.
 *
 * Walls, cliffs and fences are left alone: architecture should have
 * corners.
 */
static inline float height_at(const VoxTileGrid* grid, float x, float y) {
    float px = x + (float)grid->fine_x;
    float py = y + (float)grid->fine_y;
    int tx = (int)px >> 3;
    int ty = (int)py >> 3;
    if (tx < 0) tx = 0;
    if (tx >= VOX_TILES_W) tx = VOX_TILES_W - 1;
    if (ty < 0) ty = 0;
    if (ty >= VOX_TILES_H) ty = VOX_TILES_H - 1;
    float base = cell_base(grid, tx, ty);
    float object = (g_flatten_trees &&
                    (grid->treecell[ty][tx] || grid->structurecell[ty][tx]))
        ? 0.0f : VOX_UNITS[grid->height[ty][tx]];
    float h = base + object;
    if (object <= 0.0f || !grid->leafy[ty][tx]) return h;

    const float EDGE = g_tune.footprint;
    if (EDGE <= 0.01f) return h;   /* 0 = hard-edged cells */
    float u = px - (float)(tx * 8);
    float v = py - (float)(ty * 8);
    float k = 1.0f;
    if (cell_h(grid, tx - 1, ty) < h && u < EDGE)        k = fminf(k, u / EDGE);
    if (cell_h(grid, tx + 1, ty) < h && u > 8.0f - EDGE) k = fminf(k, (8.0f - u) / EDGE);
    if (cell_h(grid, tx, ty - 1) < h && v < EDGE)        k = fminf(k, v / EDGE);
    if (cell_h(grid, tx, ty + 1) < h && v > 8.0f - EDGE) k = fminf(k, (8.0f - v) / EDGE);
    if (k < 0.0f) k = 0.0f;
    k = k * k * (3.0f - 2.0f * k);     /* smoothstep, so the edge rolls */
    return base + object * k;
}

/* ------------------------------------------------------------------ */
/* sky                                                                 */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t zenith, horizon, cloud; } VoxSkyPalette;

/* 0xFFRRGGBB, matching the framebuffer. */
static const VoxSkyPalette VOX_SKIES[] = {
    [VOX_SKY_AGES_PRESENT] = {0xFF2C68B4u, 0xFF9CC8E0u, 0xFFDCE8F0u},
    [VOX_SKY_AGES_PAST]    = {0xFF9C6A4Au, 0xFFF0D8A8u, 0xFFF8ECC8u},
    [VOX_SKY_SPRING]       = {0xFF4880C0u, 0xFFC0D0E8u, 0xFFE4ECF4u},
    [VOX_SKY_SUMMER]       = {0xFF2878D0u, 0xFF8CC8E8u, 0xFFE0F0F8u},
    [VOX_SKY_AUTUMN]       = {0xFF90507Cu, 0xFFF0B888u, 0xFFF8D8B0u},
    [VOX_SKY_WINTER]       = {0xFF7890B0u, 0xFFC0C8D8u, 0xFFE8ECF0u},
    [VOX_SKY_SUBROSIA]     = {0xFF3C0820u, 0xFFA01830u, 0xFFD04050u},
};

static inline uint32_t lerp_color(uint32_t a, uint32_t b, int t /*0..256*/) {
    uint32_t rb = ((a & 0x00FF00FFu) * (256 - t) + (b & 0x00FF00FFu) * t) >> 8;
    uint32_t g  = ((a & 0x0000FF00u) * (256 - t) + (b & 0x0000FF00u) * t) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

/* Cheap smooth value noise for the clouds: two octaves of cosine bumps.
 * Coordinates in GB pixels (floats, so the sky stays smooth at scale). */
static inline int cloud_density(float x, float y, int t) {
    float fx = (x + (float)t) * 0.045f;
    float fy = y * 0.11f;
    float v = cosf(fx) * cosf(fy * 0.7f + 1.7f)
            + 0.5f * cosf(fx * 2.3f + 0.9f) * cosf(fy * 1.6f);
    int d = (int)((v - 0.55f) * 190.0f);
    if (d < 0) d = 0;
    if (d > 110) d = 110;
    return d;
}

static void vox_paint_sky(int kind, uint32_t* out, int S) {
    const VoxSkyPalette* p = &VOX_SKIES[kind];
    const int OW = GB_SCREEN_WIDTH * S;
    const int OH = GB_SCREEN_HEIGHT * S;

    /* Slow drift; wraps harmlessly. Subrosia's "clouds" are ember glow and
     * drift faster. */
    static int t = 0;
    t += (kind == VOX_SKY_SUBROSIA) ? 3 : 1;
    int drift = t >> 3;

    for (int Y = 0; Y < OH; Y++) {
        float gy = (float)Y / (float)S;
        /* Horizon low on the screen, where the diorama's far edge sits. */
        int g = (int)(gy * 256.0f / (float)(GB_SCREEN_HEIGHT + 24));
        uint32_t base = lerp_color(p->zenith, p->horizon, g);
        for (int X = 0; X < OW; X++) {
            int d = cloud_density((float)X / (float)S, gy, drift);
            out[Y * OW + X] = d ? lerp_color(base, p->cloud, d * 2) : base;
        }
    }
}

/* ------------------------------------------------------------------ */
/* chase camera                                                        */
/* ------------------------------------------------------------------ */

static inline int top_rim(int S) { return 2 * S; }

/* The chase camera samples the same footprint-shaped field the diorama
 * does; the cross filter below only removes sampling aliasing. */
static inline float height_raw(const VoxTileGrid* grid, float x, float y) {
    return height_at(grid, x, y);
}

/* Tent-filtered height for the perspective camera: 3x3 taps at 5px
 * spacing (weights 4/2/1) turn a clump of raised cells into one rounded
 * mass and a tree line into a smooth ridge, while a lone tree still
 * stands as its own bump. Cheap enough for the ~100k samples a frame of
 * raycasting takes, because the taps skip the dome maths entirely. */
static inline float chase_height_at(const VoxTileGrid* grid, float x, float y) {
    const float o = 3.0f;
    float c = height_raw(grid, x, y);
    float e = height_raw(grid, x - o, y) + height_raw(grid, x + o, y) +
              height_raw(grid, x, y - o) + height_raw(grid, x, y + o);
    return (c * 4.0f + e) * (1.0f / 8.0f);
}

static uint8_t s_ground_tx[VOX_TILES_H][VOX_TILES_W];
static uint8_t s_ground_ty[VOX_TILES_H][VOX_TILES_W];

/* A 3D tree replaces its overhead room-cell drawing. Leaving that drawing on
 * the floor projects branches and black outlines into long perspective
 * streaks beneath the new crown. For each tree tile, find the closest real
 * floor/grass tile and reuse the same intra-tile pixel coordinates. */
static void prepare_chase_ground(const VoxTileGrid* grid) {
    int first_world_tile = grid->hud_rows >> 3;
    for (int ty = 0; ty < VOX_TILES_H; ty++) {
        for (int tx = 0; tx < VOX_TILES_W; tx++) {
            int bx = tx, by = ty, best = 100000;
            if (grid->treecell[ty][tx] || grid->structurecell[ty][tx]) {
                for (int sy = first_world_tile; sy < VOX_TILES_H; sy++) {
                    for (int sx = 0; sx < VOX_TILES_W; sx++) {
                        if (grid->treecell[sy][sx] ||
                            grid->structurecell[sy][sx] ||
                            grid->leafy[sy][sx] ||
                            grid->height[sy][sx] < VOX_H_FLOOR ||
                            grid->height[sy][sx] > VOX_H_LOW ||
                            grid->elevation[sy][sx] != grid->elevation[ty][tx])
                            continue;
                        int dx = sx - tx, dy = sy - ty;
                        int score = dx * dx + dy * dy;
                        if (score < best) { best = score; bx = sx; by = sy; }
                    }
                }
            }
            s_ground_tx[ty][tx] = (uint8_t)bx;
            s_ground_ty[ty][tx] = (uint8_t)by;
        }
    }
}

static inline uint32_t chase_tex_at(const VoxTileGrid* grid, float x, float y) {
    int px = (int)(x + (float)grid->fine_x);
    int py = (int)(y + (float)grid->fine_y);
    if (px < 0) px = 0;
    if (px >= VOX_TEX_W) px = VOX_TEX_W - 1;
    if (py < 0) py = 0;
    if (py >= VOX_TEX_H) py = VOX_TEX_H - 1;
    int tx = px >> 3, ty = py >> 3;
    bool under_tree = grid->treecell[ty][tx] != 0;
    bool under_structure = grid->structurecell[ty][tx] != 0;
    if (under_tree || under_structure) {
        px = (int)s_ground_tx[ty][tx] * 8 + (px & 7);
        py = (int)s_ground_ty[ty][tx] * 8 + (py & 7);
    }
    uint32_t c = grid->tex[py * VOX_TEX_W + px];
    /* The source cell is hidden by a crown in 2D. Replacing it with bright
     * meadow art made the distant ground shine through every trunk gap as a
     * single horizontal bar. It is still real floor geometry, but wears the
     * same simple canopy shadow a dense treeline would cast. */
    return under_tree ? shade(c, 150)
                      : (under_structure ? shade(c, 178) : c);
}

/* One texel from the original raised tile, folded onto a vertical face.
 * Tall drops repeat complete 8px bands instead of stretching a row; this is
 * the same pixel vocabulary the flat map uses, now attached to real planar
 * geometry. Directional light is applied by the caller only after sampling. */
static uint32_t chase_cliff_texel(const VoxTileGrid* grid,
                                  int tile_x, int tile_y,
                                  float along, float vertical) {
    if (tile_x < 0) tile_x = 0;
    if (tile_x >= VOX_TILES_W) tile_x = VOX_TILES_W - 1;
    if (tile_y < 0) tile_y = 0;
    if (tile_y >= VOX_TILES_H) tile_y = VOX_TILES_H - 1;
    int u = ((int)floorf(along)) & 7;
    int v = ((int)floorf(vertical)) & 7;
    return grid->tex[(tile_y * 8 + v) * VOX_TEX_W + tile_x * 8 + u];
}

/* True where a camera-sized probe is over architectural solid terrain in the
 * live room. Positions beyond the live grid are left to the persistent-world renderer;
 * treating the nearest screen-edge tile as an infinite wall would prevent
 * the camera from following Link cleanly across room boundaries. */
static bool chase_camera_solid(const VoxTileGrid* grid, float x, float y) {
    float px = x + (float)grid->fine_x;
    float py = y + (float)grid->fine_y;
    if (px < 0.0f || py < 0.0f ||
        px >= (float)VOX_TEX_W || py >= (float)VOX_TEX_H)
        return false;
    int tx = (int)px >> 3;
    int ty = (int)py >> 3;
    /* Volumetric vegetation can cross the near plane harmlessly. Treating
     * its old solid cell as a wall shoved the camera into Link's back in
     * every forest clearing; architecture and cliffs still clamp it. */
    if (grid->structurecell[ty][tx]) return true;
    return !grid->treecell[ty][tx] && grid->height[ty][tx] >= VOX_H_MID;
}

float vox_chase_camera_back(const VoxTileGrid* grid, float lx, float ly,
                            float fx, float fy, float requested) {
    const float MIN_BACK = 12.0f;
    const float CLEARANCE = 6.0f;
    const float RADIUS = 3.5f;
    if (!grid || requested <= MIN_BACK) return requested;

    float len = sqrtf(fx * fx + fy * fy);
    if (len < 0.001f) return requested;
    fx /= len;
    fy /= len;
    const float rx = -fy, ry = fx;

    /* Walk from Link toward the requested camera position. The first wall
     * wins, keeping the camera on Link's side of it. Two shoulder probes
     * stop it clipping a corner while the centre line remains clear. */
    for (float d = MIN_BACK; d <= requested; d += 2.0f) {
        float x = lx - fx * d;
        float y = ly - fy * d;
        if (chase_camera_solid(grid, x, y) ||
            chase_camera_solid(grid, x + rx * RADIUS, y + ry * RADIUS) ||
            chase_camera_solid(grid, x - rx * RADIUS, y - ry * RADIUS)) {
            float safe = d - CLEARANCE;
            return safe > MIN_BACK ? safe : MIN_BACK;
        }
    }
    return requested;
}

/* Third person: a camera floating behind the player, looking the way they
 * face, raycasting the same heightfield the diorama extrudes -- the
 * classic "voxel space" scheme finally used for what it was invented
 * for. Planar (unnormalized) rays keep walls straight; a per-column
 * y-buffer gives front-to-back occlusion with no depth buffer. */
/* Per-pixel terrain depth for the last chase frame. Billboards test
 * against it, which is the whole of sprite occlusion: without it a
 * character behind a wall drew straight through the wall, because the
 * painter's order only sorts sprites against each other. */
static float s_zbuf[GB_FRAMEBUFFER_SIZE * VOX_MAX_SCALE * VOX_MAX_SCALE];

typedef struct {
    float x, y, dep;
    float vertical, along;
} ChaseFaceVert;

static bool chase_project_face_vertex(float wx, float wy, float z,
                                      float vertical, float along,
                                      float cx, float cy,
                                      float fxv, float fyv,
                                      float rxv, float ryv,
                                      float focal, float cz, int horizon,
                                      int OW, ChaseFaceVert* out) {
    float dx = wx - cx, dy = wy - cy;
    float dep = dx * fxv + dy * fyv;
    if (dep < 8.0f) return false;
    float lat = dx * rxv + dy * ryv;
    out->x = (float)OW * 0.5f + lat / dep * focal;
    out->y = (float)horizon + (cz - z) / dep * focal;
    out->dep = dep;
    out->vertical = vertical;
    out->along = along;
    return true;
}

static float chase_edge(float ax, float ay, float bx, float by,
                        float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

/* Rasterise one flat-colour world-space face with perspective-correct depth.
 * The colour is a source-art texel for front/back faces and a shaded copy of
 * that texel only where extrusion has exposed a new side. */
static void draw_chase_color_triangle(const ChaseFaceVert* a,
                                      const ChaseFaceVert* b,
                                      const ChaseFaceVert* c,
                                      uint32_t color, int face_mul,
                                      uint32_t fog, int OW, int OH,
                                      uint32_t* out) {
    float area = chase_edge(a->x, a->y, b->x, b->y, c->x, c->y);
    if (fabsf(area) < 0.001f) return;
    int minx = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    int maxx = (int)ceilf (fmaxf(a->x, fmaxf(b->x, c->x)));
    int miny = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    int maxy = (int)ceilf (fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < 0) minx = 0;
    if (maxx >= OW) maxx = OW - 1;
    if (miny < 0) miny = 0;
    if (maxy >= OH) maxy = OH - 1;
    float ia = 1.0f / a->dep, ib = 1.0f / b->dep, ic = 1.0f / c->dep;
    uint32_t lit = shade(color, face_mul);
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float wa = chase_edge(b->x, b->y, c->x, c->y, px, py) / area;
            float wb = chase_edge(c->x, c->y, a->x, a->y, px, py) / area;
            float wc = 1.0f - wa - wb;
            if (wa < -0.0001f || wb < -0.0001f || wc < -0.0001f) continue;
            float invd = wa * ia + wb * ib + wc * ic;
            if (invd <= 0.0f) continue;
            float dep = 1.0f / invd;
            int at = y * OW + x;
            if (s_zbuf[at] < dep - 0.35f) continue;
            int fog_mix = (int)((dep - g_tune.fog_start) * 256.0f /
                                (230.0f - g_tune.fog_start));
            if (fog_mix < 0) fog_mix = 0;
            if (fog_mix > (int)g_tune.fog_max)
                fog_mix = (int)g_tune.fog_max;
            out[at] = lerp_color(lit, fog, fog_mix);
            s_zbuf[at] = dep;
        }
    }
}

static void draw_chase_color_quad(float ax, float ay, float az,
                                  float bx, float by, float bz,
                                  float cx0, float cy0, float cz0,
                                  float dx, float dy, float dz,
                                  uint32_t color, int face_mul,
                                  float camx, float camy,
                                  float fxv, float fyv,
                                  float rxv, float ryv,
                                  float focal, float eye_z, int horizon,
                                  uint32_t fog, int OW, int OH,
                                  uint32_t* out) {
    ChaseFaceVert v[4];
    if (!chase_project_face_vertex(ax, ay, az, 0.0f, 0.0f,
                                   camx, camy, fxv, fyv, rxv, ryv,
                                   focal, eye_z, horizon, OW, &v[0]) ||
        !chase_project_face_vertex(bx, by, bz, 0.0f, 0.0f,
                                   camx, camy, fxv, fyv, rxv, ryv,
                                   focal, eye_z, horizon, OW, &v[1]) ||
        !chase_project_face_vertex(cx0, cy0, cz0, 0.0f, 0.0f,
                                   camx, camy, fxv, fyv, rxv, ryv,
                                   focal, eye_z, horizon, OW, &v[2]) ||
        !chase_project_face_vertex(dx, dy, dz, 0.0f, 0.0f,
                                   camx, camy, fxv, fyv, rxv, ryv,
                                   focal, eye_z, horizon, OW, &v[3]))
        return;
    draw_chase_color_triangle(&v[0], &v[1], &v[2], color, face_mul,
                              fog, OW, OH, out);
    draw_chase_color_triangle(&v[0], &v[2], &v[3], color, face_mul,
                              fog, OW, OH, out);
}

/* Impa's tree house is a two-row overhead drawing, but it describes one
 * object: a 48px canopy over a 48px doorway/trunk facade. Fold those exact
 * live pixels onto one shallow building. No replacement texture or palette
 * is invented; even the open-door tile continues to come from the cart. */
static void draw_voxel_structure(const VoxStructure* b, float ground,
                                 float camx, float camy,
                                 float fxv, float fyv,
                                 float rxv, float ryv,
                                 float focal, float eye_z, float hpx,
                                 int horizon, uint32_t fog,
                                 int OW, int OH, uint32_t* out) {
    const float west = (float)b->sx;
    const float east = west + (float)VOX_STRUCTURE_W;
    const float north = (float)b->sy;
    const float south = north + 16.0f;
    const float base_z = ground * hpx;
    const float wall_top = base_z + 16.0f;
    enum { ROOF_THICK = 4 };
    const float roof_top = wall_top + (float)ROOF_THICK;
    const float centre_x = (west + east) * 0.5f;
    const float centre_y = (north + south) * 0.5f;
    const bool see_south = camy >= centre_y;
    const bool see_north = camy <= centre_y;
    const bool see_west = camx <= centre_x;
    const bool see_east = camx >= centre_x;

    /* Doorway/trunk facade. Source y is top-down on the flat map; on the
     * wall it becomes top-to-bottom height, preserving every 1px course. */
    if (see_south || see_north) {
        float face_y = see_south ? south : north;
        for (int py = 0; py < VOX_STRUCTURE_H; py++) {
            float z1 = wall_top - (float)py;
            float z0 = z1 - 1.0f;
            for (int px = 0; px < VOX_STRUCTURE_W; px++) {
                float x0 = west + (float)px, x1 = x0 + 1.0f;
                int src_x = see_south ? px : VOX_STRUCTURE_W - 1 - px;
                int src_at = py * VOX_STRUCTURE_W + src_x;
                if (!b->front_solid[src_at]) continue;
                uint32_t c = b->front[src_at];
                if (see_south) {
                    draw_chase_color_quad(x0, face_y, z0, x1, face_y, z0,
                                          x1, face_y, z1, x0, face_y, z1,
                                          c, 256, camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                } else {
                    draw_chase_color_quad(x1, face_y, z0, x0, face_y, z0,
                                          x0, face_y, z1, x1, face_y, z1,
                                          c, 256, camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                }
            }
        }
    }

    /* The 16px-deep body gets edge texels from the same facade, making the
     * house genuinely volumetric when approached from either side. */
    if (see_west || see_east) {
        float face_x = see_west ? west : east;
        for (int py = 0; py < VOX_STRUCTURE_H; py++) {
            float z1 = wall_top - (float)py;
            float z0 = z1 - 1.0f;
            for (int d = 0; d < 16; d++) {
                float y0 = north + (float)d, y1 = y0 + 1.0f;
                int src_x = see_west ? 15 - d
                                     : VOX_STRUCTURE_W - 16 + d;
                int src_at = py * VOX_STRUCTURE_W + src_x;
                if (!b->front_solid[src_at]) continue;
                uint32_t c = b->front[src_at];
                if (see_west) {
                    draw_chase_color_quad(face_x, y1, z0, face_x, y0, z0,
                                          face_x, y0, z1, face_x, y1, z1,
                                          c, 256, camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                } else {
                    draw_chase_color_quad(face_x, y0, z0, face_x, y1, z0,
                                          face_x, y1, z1, face_x, y0, z1,
                                          c, 256, camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                }
            }
        }
    }

    /* Complete authored canopy strip as the roof. */
    for (int py = 0; py < VOX_STRUCTURE_H; py++) {
        float y0 = north + (float)py, y1 = y0 + 1.0f;
        for (int px = 0; px < VOX_STRUCTURE_W; px++) {
            float x0 = west + (float)px, x1 = x0 + 1.0f;
            int src_at = py * VOX_STRUCTURE_W + px;
            if (!b->roof_solid[src_at]) continue;
            uint32_t c = b->roof[src_at];
            draw_chase_color_quad(x0, y0, roof_top, x1, y0, roof_top,
                                  x1, y1, roof_top, x0, y1, roof_top,
                                  c, 256, camx, camy, fxv, fyv, rxv, ryv,
                                  focal, eye_z, horizon, fog, OW, OH, out);
        }
    }

    /* A shallow canopy skirt joins the roof to the facade. It samples the
     * matching roof edge instead of stretching one invented colour. */
    for (int v = 0; v < ROOF_THICK; v++) {
        float z0 = wall_top + (float)v, z1 = z0 + 1.0f;
        int src_y = VOX_STRUCTURE_H - 1 -
                    (v * VOX_STRUCTURE_H / ROOF_THICK);
        if (see_south || see_north) {
            float face_y = see_south ? south : north;
            for (int px = 0; px < VOX_STRUCTURE_W; px++) {
                float x0 = west + (float)px, x1 = x0 + 1.0f;
                int src_x = see_south ? px : VOX_STRUCTURE_W - 1 - px;
                int roof_y = see_south ? src_y
                                       : VOX_STRUCTURE_H - 1 - src_y;
                int src_at = roof_y * VOX_STRUCTURE_W + src_x;
                if (!b->roof_solid[src_at]) continue;
                uint32_t c = b->roof[src_at];
                if (see_south) {
                    draw_chase_color_quad(x0, face_y, z0, x1, face_y, z0,
                                          x1, face_y, z1, x0, face_y, z1,
                                          c, 256, camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                } else {
                    draw_chase_color_quad(x1, face_y, z0, x0, face_y, z0,
                                          x0, face_y, z1, x1, face_y, z1,
                                          c, 256, camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                }
            }
        }
        if (see_west || see_east) {
            float face_x = see_west ? west : east;
            for (int d = 0; d < 16; d++) {
                float y0 = north + (float)d, y1 = y0 + 1.0f;
                int src_x = see_west ? 15 - d
                                     : VOX_STRUCTURE_W - 16 + d;
                int src_at = src_y * VOX_STRUCTURE_W + src_x;
                if (!b->roof_solid[src_at]) continue;
                uint32_t c = b->roof[src_at];
                if (see_west) {
                    draw_chase_color_quad(face_x, y1, z0, face_x, y0, z0,
                                          face_x, y0, z1, face_x, y1, z1,
                                          c, 256, camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                } else {
                    draw_chase_color_quad(face_x, y0, z0, face_x, y1, z0,
                                          face_x, y1, z1, face_x, y0, z1,
                                          c, 256, camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                }
            }
        }
    }
}


/* Turn the original 16x16 room-cell drawing into fixed world geometry.
 * Full trees now have two literal pieces, like the Gen 1 voxel treatment:
 * a trunk tile below and a canopy tile above. The canopy top is the original
 * source cell unchanged. Its four vertical edges are direct projections of
 * the corresponding source half, and the trunk samples the source cell's
 * lower centre. There is no rounded shell, generated palette, or billboard.
 * Tufts keep a shallow relief. */
static void draw_voxel_tree(const VoxTree* t, int object_sx, int object_sy,
                            float ground,
                            float camx, float camy,
                            float fxv, float fyv, float rxv, float ryv,
                            float focal, float eye_z, float hpx,
                            int horizon, uint32_t fog,
                            int OW, int OH, uint32_t* out) {
    const bool big = t->hcls >= VOX_H_HIGH;
    const float centre_y = (float)object_sy + 8.0f;
    const float base_z = ground * hpx;

    if (big) {
        const float centre_x = (float)object_sx + 8.0f;
        const bool see_south = camy >= centre_y;
        const bool see_north = camy <= centre_y;
        const bool see_west = camx <= centre_x;
        const bool see_east = camx >= centre_x;
        enum { TRUNK_W = 6, TRUNK_H = 16, CANOPY_H = 12 };
        const int trunk_left = object_sx + (16 - TRUNK_W) / 2;
        const float trunk_north = centre_y - (float)TRUNK_W * 0.5f;
        const float trunk_south = centre_y + (float)TRUNK_W * 0.5f;
        const float canopy_bottom = base_z + (float)TRUNK_H;
        const float canopy_top = canopy_bottom + (float)CANOPY_H;

        /* Bottom tile: a square trunk cut straight from the lower centre of
         * the authored room cell. Repeating source rows is nearest-neighbour
         * scaling, so no new colours or painted bark enter the result. */
        for (int tz = 0; tz < TRUNK_H; tz++) {
            int src_y = 15 - tz * 7 / (TRUNK_H - 1);
            float z0 = base_z + (float)tz, z1 = z0 + 1.0f;
            for (int u = 0; u < TRUNK_W; u++) {
                int src_x = 5 + u;
                uint32_t c = t->tex[src_y * 16 + src_x];
                float x0 = (float)(trunk_left + u), x1 = x0 + 1.0f;
                if (see_south)
                    draw_chase_color_quad(x0, trunk_south, z0,
                                          x1, trunk_south, z0,
                                          x1, trunk_south, z1,
                                          x0, trunk_south, z1,
                                          c, cube_face_shade(246, u, tz),
                                          camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                if (see_north)
                    draw_chase_color_quad(x1, trunk_north, z0,
                                          x0, trunk_north, z0,
                                          x0, trunk_north, z1,
                                          x1, trunk_north, z1,
                                          c, cube_face_shade(214, u, tz),
                                          camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                float y0 = trunk_north + (float)u, y1 = y0 + 1.0f;
                if (see_west)
                    draw_chase_color_quad((float)trunk_left, y0, z0,
                                          (float)trunk_left, y1, z0,
                                          (float)trunk_left, y1, z1,
                                          (float)trunk_left, y0, z1,
                                          c, cube_face_shade(228, u, tz),
                                          camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
                if (see_east)
                    draw_chase_color_quad((float)(trunk_left + TRUNK_W), y1, z0,
                                          (float)(trunk_left + TRUNK_W), y0, z0,
                                          (float)(trunk_left + TRUNK_W), y0, z1,
                                          (float)(trunk_left + TRUNK_W), y1, z1,
                                          c, cube_face_shade(228, u, tz),
                                          camx, camy, fxv, fyv,
                                          rxv, ryv, focal, eye_z, horizon,
                                          fog, OW, OH, out);
            }
        }

        /* Top tile: the complete 16x16 room-cell art, horizontal and flat.
         * Neighbouring cells therefore rebuild the cart's connected canopy
         * exactly instead of approximating it with one rounded primitive. */
        for (int py = 0; py < 16; py++) {
            for (int px = 0; px < 16; px++) {
                int at = py * 16 + px;
                if (!t->solid[at]) continue;
                float x0 = (float)object_sx + (float)px;
                float x1 = x0 + 1.0f;
                float y0 = (float)object_sy + (float)py;
                float y1 = y0 + 1.0f;
                draw_chase_color_quad(x0, y0, canopy_top,
                                      x1, y0, canopy_top,
                                      x1, y1, canopy_top,
                                      x0, y1, canopy_top,
                                      t->tex[at],
                                      cube_face_shade(264, px, py),
                                      camx, camy, fxv, fyv,
                                      rxv, ryv, focal, eye_z, horizon,
                                      fog, OW, OH, out);
            }
        }

        /* Canopy sides: unfold the matching half of the same source tile.
         * The south face uses rows 8..15 (the lighter lower canopy the user
         * can see in the original), north uses 0..7, and east/west use their
         * corresponding columns. Shared forest edges emit no internal wall. */
        if (see_south && !(t->joins & VOX_TREE_JOIN_S)) {
            for (int tz = 0; tz < CANOPY_H; tz++) {
                int src_y = 15 - tz * 7 / (CANOPY_H - 1);
                float z0 = canopy_bottom + (float)tz, z1 = z0 + 1.0f;
                for (int u = 0; u < 16; u++) {
                    int at = src_y * 16 + u;
                    if (!t->solid[at]) continue;
                    float x0 = (float)object_sx + (float)u;
                    draw_chase_color_quad(x0, (float)object_sy + 16.0f, z0,
                                          x0 + 1.0f, (float)object_sy + 16.0f, z0,
                                          x0 + 1.0f, (float)object_sy + 16.0f, z1,
                                          x0, (float)object_sy + 16.0f, z1,
                                          t->tex[at],
                                          cube_face_shade(242, u, tz),
                                          camx, camy,
                                          fxv, fyv, rxv, ryv, focal, eye_z,
                                          horizon, fog, OW, OH, out);
                }
            }
        }
        if (see_north && !(t->joins & VOX_TREE_JOIN_N)) {
            for (int tz = 0; tz < CANOPY_H; tz++) {
                int src_y = 7 - tz * 7 / (CANOPY_H - 1);
                float z0 = canopy_bottom + (float)tz, z1 = z0 + 1.0f;
                for (int u = 0; u < 16; u++) {
                    int at = src_y * 16 + u;
                    if (!t->solid[at]) continue;
                    float x0 = (float)object_sx + (float)u;
                    draw_chase_color_quad(x0 + 1.0f, (float)object_sy, z0,
                                          x0, (float)object_sy, z0,
                                          x0, (float)object_sy, z1,
                                          x0 + 1.0f, (float)object_sy, z1,
                                          t->tex[at],
                                          cube_face_shade(212, u, tz),
                                          camx, camy,
                                          fxv, fyv, rxv, ryv, focal, eye_z,
                                          horizon, fog, OW, OH, out);
                }
            }
        }
        if (see_west && !(t->joins & VOX_TREE_JOIN_W)) {
            for (int tz = 0; tz < CANOPY_H; tz++) {
                int src_x = 7 - tz * 7 / (CANOPY_H - 1);
                float z0 = canopy_bottom + (float)tz, z1 = z0 + 1.0f;
                for (int u = 0; u < 16; u++) {
                    int at = u * 16 + src_x;
                    if (!t->solid[at]) continue;
                    float y0 = (float)object_sy + (float)u;
                    draw_chase_color_quad((float)object_sx, y0, z0,
                                          (float)object_sx, y0 + 1.0f, z0,
                                          (float)object_sx, y0 + 1.0f, z1,
                                          (float)object_sx, y0, z1,
                                          t->tex[at],
                                          cube_face_shade(226, u, tz),
                                          camx, camy,
                                          fxv, fyv, rxv, ryv, focal, eye_z,
                                          horizon, fog, OW, OH, out);
                }
            }
        }
        if (see_east && !(t->joins & VOX_TREE_JOIN_E)) {
            for (int tz = 0; tz < CANOPY_H; tz++) {
                int src_x = 8 + tz * 7 / (CANOPY_H - 1);
                float z0 = canopy_bottom + (float)tz, z1 = z0 + 1.0f;
                for (int u = 0; u < 16; u++) {
                    int at = u * 16 + src_x;
                    if (!t->solid[at]) continue;
                    float y0 = (float)object_sy + (float)u;
                    draw_chase_color_quad((float)object_sx + 16.0f, y0 + 1.0f, z0,
                                          (float)object_sx + 16.0f, y0, z0,
                                          (float)object_sx + 16.0f, y0, z1,
                                          (float)object_sx + 16.0f, y0 + 1.0f, z1,
                                          t->tex[at],
                                          cube_face_shade(226, u, tz),
                                          camx, camy,
                                          fxv, fyv, rxv, ryv, focal, eye_z,
                                          horizon, fog, OW, OH, out);
                }
            }
        }
        return;
    }

    const float zscale = 0.68f;
    const float half_depth = 3.0f;
    const float south = centre_y + half_depth;
    const float north = centre_y - half_depth;

    for (int py = 0; py < 16; py++) {
        for (int px = 0; px < 16; px++) {
            int at = py * 16 + px;
            if (!t->solid[at]) continue;
            float x0 = (float)object_sx + (float)px;
            float x1 = x0 + 1.0f;
            float z0 = base_z + (float)(15 - py) * zscale;
            float z1 = z0 + zscale;
            uint32_t c = t->tex[at];

            /* Original artwork, untouched except for distance fog. */
            draw_chase_color_quad(x0, south, z0, x1, south, z0,
                                  x1, south, z1, x0, south, z1,
                                  c, cube_face_shade(256, px, py),
                                  camx, camy, fxv, fyv, rxv, ryv,
                                  focal, eye_z, horizon, fog, OW, OH, out);
            draw_chase_color_quad(x1, north, z0, x0, north, z0,
                                  x0, north, z1, x1, north, z1,
                                  c, cube_face_shade(224, px, py),
                                  camx, camy, fxv, fyv, rxv, ryv,
                                  focal, eye_z, horizon, fog, OW, OH, out);

            bool left  = px == 0  || !t->solid[at - 1];
            bool right = px == 15 || !t->solid[at + 1];
            bool above = py == 0  || !t->solid[at - 16];
            bool below = py == 15 || !t->solid[at + 16];
            if (left)
                draw_chase_color_quad(x0, north, z0, x0, south, z0,
                                      x0, south, z1, x0, north, z1,
                                      c, 218, camx, camy, fxv, fyv, rxv, ryv,
                                      focal, eye_z, horizon, fog, OW, OH, out);
            if (right)
                draw_chase_color_quad(x1, south, z0, x1, north, z0,
                                      x1, north, z1, x1, south, z1,
                                      c, 202, camx, camy, fxv, fyv, rxv, ryv,
                                      focal, eye_z, horizon, fog, OW, OH, out);
            if (above)
                draw_chase_color_quad(x0, north, z1, x1, north, z1,
                                      x1, south, z1, x0, south, z1,
                                      c, 278, camx, camy, fxv, fyv, rxv, ryv,
                                      focal, eye_z, horizon, fog, OW, OH, out);
            if (below && py != 15)
                draw_chase_color_quad(x0, south, z0, x1, south, z0,
                                      x1, north, z0, x0, north, z0,
                                      c, 176, camx, camy, fxv, fyv, rxv, ryv,
                                      focal, eye_z, horizon, fog, OW, OH, out);
        }
    }
}

static void draw_chase_face_triangle(const ChaseFaceVert* a,
                                     const ChaseFaceVert* b,
                                     const ChaseFaceVert* c,
                                     const VoxTileGrid* grid,
                                     int tx, int ty, int face_mul,
                                     uint32_t fog, int S,
                                     int world_top, int OW, int OH,
                                     uint32_t* out) {
    float area = chase_edge(a->x, a->y, b->x, b->y, c->x, c->y);
    if (fabsf(area) < 0.001f) return;
    float minxf = fminf(a->x, fminf(b->x, c->x));
    float maxxf = fmaxf(a->x, fmaxf(b->x, c->x));
    float minyf = fminf(a->y, fminf(b->y, c->y));
    float maxyf = fmaxf(a->y, fmaxf(b->y, c->y));
    int minx = (int)floorf(minxf), maxx = (int)ceilf(maxxf);
    int miny = (int)floorf(minyf), maxy = (int)ceilf(maxyf);
    if (minx < 0) minx = 0;
    if (maxx >= OW) maxx = OW - 1;
    if (miny < world_top * S) miny = world_top * S;
    if (maxy >= OH) maxy = OH - 1;

    float ia = 1.0f / a->dep, ib = 1.0f / b->dep, ic = 1.0f / c->dep;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float wa = chase_edge(b->x, b->y, c->x, c->y, px, py) / area;
            float wb = chase_edge(c->x, c->y, a->x, a->y, px, py) / area;
            float wc = 1.0f - wa - wb;
            if (wa < -0.0001f || wb < -0.0001f || wc < -0.0001f) continue;
            float invd = wa * ia + wb * ib + wc * ic;
            if (invd <= 0.0f) continue;
            float dep = 1.0f / invd;
            int at = y * OW + x;
            if (s_zbuf[at] < dep - 2.5f) continue;
            float vertical = (wa * a->vertical * ia +
                              wb * b->vertical * ib +
                              wc * c->vertical * ic) / invd;
            float along = (wa * a->along * ia + wb * b->along * ib +
                           wc * c->along * ic) / invd;
            uint32_t color = shade(chase_cliff_texel(grid, tx, ty,
                                                     along, vertical),
                                   face_mul);
            int fog_mix = (int)((dep - g_tune.fog_start) * 256.0f /
                                (230.0f - g_tune.fog_start));
            if (fog_mix < 0) fog_mix = 0;
            if (fog_mix > (int)g_tune.fog_max)
                fog_mix = (int)g_tune.fog_max;
            out[at] = lerp_color(color, fog, fog_mix);
            s_zbuf[at] = dep;
        }
    }
}

/* Put one true projected quad on every visible authoritative height edge.
 * The raycast remains responsible for top surfaces and distant continuity;
 * these quads make cliff/wall silhouettes planar and preserve the room map's
 * right-angle corners instead of bending a face independently per column. */
static void draw_chase_cliff_faces(const VoxTileGrid* grid,
                                   float cx, float cy,
                                   float fxv, float fyv,
                                   float rxv, float ryv,
                                   float focal, float cz, float hpx,
                                   int horizon, uint32_t fog, int S,
                                   int OW, int OH, uint32_t* out) {
    const float* units = VOX_UNITS;
    int first_ty = grid->hud_rows >> 3;
    for (int ty = first_ty; ty < VOX_TILES_H; ty++) {
        for (int tx = 0; tx < VOX_TILES_W; tx++) {
            if (grid->treecell[ty][tx] ||
                (grid->height[ty][tx] < VOX_H_MID &&
                 grid->elevation[ty][tx] == 0))
                continue;
            float high = cell_base(grid, tx, ty)
                       + units[grid->height[ty][tx]];
            float x0 = (float)(tx * 8) - (float)grid->fine_x;
            float y0 = (float)(ty * 8) - (float)grid->fine_y;
            for (int side = 0; side < 4; side++) {
                int nx = tx + (side == 1) - (side == 0);
                int ny = ty + (side == 3) - (side == 2);
                if (nx < 0 || nx >= VOX_TILES_W ||
                    ny < first_ty || ny >= VOX_TILES_H)
                    continue;
                float low = cell_base(grid, nx, ny);
                if (!grid->treecell[ny][nx])
                    low += units[grid->height[ny][nx]];
                if (high <= low + 0.75f) continue;

                float ax, ay, bx, by, mx, my;
                int face_mul;
                if (side == 0) {       /* west */
                    ax = bx = x0; ay = y0; by = y0 + 8.0f;
                    mx = -1.0f; my = 0.0f; face_mul = 238;
                } else if (side == 1) {/* east */
                    ax = bx = x0 + 8.0f; ay = y0 + 8.0f; by = y0;
                    mx = 1.0f; my = 0.0f; face_mul = 222;
                } else if (side == 2) {/* north */
                    ay = by = y0; ax = x0 + 8.0f; bx = x0;
                    mx = 0.0f; my = -1.0f; face_mul = 248;
                } else {              /* south */
                    ay = by = y0 + 8.0f; ax = x0; bx = x0 + 8.0f;
                    mx = 0.0f; my = 1.0f; face_mul = 232;
                }
                float ex = (ax + bx) * 0.5f, ey = (ay + by) * 0.5f;
                if ((cx - ex) * mx + (cy - ey) * my <= 0.0f) continue;

                ChaseFaceVert v[4];
                float drop = (high - low) * hpx;
                if (!chase_project_face_vertex(ax, ay, high * hpx, 0.0f,
                                               ax + ay, cx, cy, fxv, fyv,
                                               rxv, ryv, focal, cz, horizon,
                                               OW, &v[0]) ||
                    !chase_project_face_vertex(bx, by, high * hpx, 0.0f,
                                               bx + by, cx, cy, fxv, fyv,
                                               rxv, ryv, focal, cz, horizon,
                                               OW, &v[1]) ||
                    !chase_project_face_vertex(bx, by, low * hpx, drop,
                                               bx + by, cx, cy, fxv, fyv,
                                               rxv, ryv, focal, cz, horizon,
                                               OW, &v[2]) ||
                    !chase_project_face_vertex(ax, ay, low * hpx, drop,
                                               ax + ay, cx, cy, fxv, fyv,
                                               rxv, ryv, focal, cz, horizon,
                                               OW, &v[3]))
                    continue;
                draw_chase_face_triangle(&v[0], &v[1], &v[2], grid,
                                         tx, ty, face_mul, fog, S,
                                         grid->hud_rows, OW, OH, out);
                draw_chase_face_triangle(&v[0], &v[2], &v[3], grid,
                                         tx, ty, face_mul, fog, S,
                                         grid->hud_rows, OW, OH, out);
            }
        }
    }
}

/* One frame of camera aim: where Link is, and where the camera looks.
 * Split out of render_chase so the yaw rules can be driven directly by
 * tools/chasecam_test.c -- how a camera feels is not something a
 * screenshot can check, but where it points is. */
void vox_chase_step(const VoxTileGrid* grid, float* out_lx, float* out_ly) {
    /* Hold the last known anchor through the frames where the game state
     * is unreadable (room transitions): re-targeting a default centre made
     * the camera swim away and back on every screen change. */
    static float lx = 80.0f, ly = 104.0f;
    if (grid->link_known) {
        lx = (float)grid->link_sx;
        ly = (float)grid->link_feet_sy;
        /* A fresh chase camera starts where its name promises: directly
         * behind Link. Free orbit still owns the heading after this first
         * trustworthy gameplay frame. */
        if (!g_chase_heading_live) {
            g_chase_yaw = CHASE_DIR_YAW[grid->link_dir & 3];
            g_chase_heading_live = true;
            g_manual_hold = 0;
            g_recentring = false;
        }
    }

    /* Yaw: the camera orbits Link, and the stick owns it.
     *
     * Two earlier attempts both fought the player. Following his 4-way
     * facing frame for frame was chaos -- a tap to read a sign whipped the
     * world a quarter turn. Following only while he WALKED was calmer but
     * still took the camera back every time you moved, so a look-around
     * never survived a step.
     *
     * So the camera no longer takes itself anywhere. The right stick (or
     * Q/E) swings it and it stays put -- Link runs around underneath it,
     * which is what a free orbit should feel like. Recentring is asked
     * for, not assumed: click the right stick (or press C) and it eases
     * round behind him; hold it and it keeps following until you let go.
     * chase_follow in the tuning block brings the old walk-follow back for
     * anyone who preferred it, and defaults to 0. */
#ifdef GB_HAS_SDL2
    {
        float stick = 0.0f;
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        if (keys) {
            if (keys[SDL_SCANCODE_Q]) stick -= 1.0f;
            if (keys[SDL_SCANCODE_E]) stick += 1.0f;
        }
        SDL_GameController* pad = SDL_GameControllerFromPlayerIndex(0);
        if (pad) {
            Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
            if (ax > 6000 || ax < -6000) stick += (float)ax / 32767.0f;
        }
        if (stick != 0.0f) voxel_chase_turn(stick);
        /* Recentre: right stick click, or C. Either one held keeps the
         * camera behind him; a tap starts an ease that finishes itself. */
        bool want_recentre = keys && keys[SDL_SCANCODE_C];
        if (pad && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
            want_recentre = true;
        voxel_chase_recenter(want_recentre);
    }
#endif
    const float turn = g_turn_request;
    g_turn_request = 0.0f;
    {
        /* Walking, not just facing: Link's own position has to have moved,
         * and a room transition teleports it, so only small steps count.
         * Only the optional walk-follow uses this. */
        static float prev_lx = 0.0f, prev_ly = 0.0f;
        static bool  pos_live = false;
        /* Frames left before the camera is allowed to drift again, and how
         * long it has been drifting. The first is why the old auto-swing
         * felt like a fight: it resumed the instant the stick went still,
         * so every adjustment was undone before you let go. The second
         * eases the drift in, so it starts as a lean rather than a jerk. */
        static int   drift_age = 0;
        float dx = lx - prev_lx, dy = ly - prev_ly;
        bool walking = pos_live && (dx * dx + dy * dy) > 0.02f &&
                       (dx * dx + dy * dy) < 64.0f;
        prev_lx = lx; prev_ly = ly; pos_live = true;

        /* 0 up, 1 right, 2 down, 3 left -- screen yaw has +y downward,
         * so up is -pi/2 and down is +pi/2. */
        const float behind = CHASE_DIR_YAW[grid->link_dir & 3];

        if (turn != 0.0f) {
            g_chase_yaw += turn * 0.055f;
            /* About half a second at 60fps. Long enough to line a shot up
             * and let go without the camera immediately taking it back. */
            g_manual_hold = 32;
            drift_age = 0;
            g_recentring = false;     /* the stick always wins */
        } else if (g_manual_hold > 0) {
            g_manual_hold--;
        }

        /* Shortest way round, so facing left from up turns a quarter turn
         * rather than three quarters the other way. */
        float delta = behind - g_chase_yaw;
        while (delta >  3.1415927f) delta -= 6.2831853f;
        while (delta < -3.1415927f) delta += 6.2831853f;

        if (g_recentring) {
            float rate = g_tune.chase_recenter;
            if (rate <= 0.0f) rate = 0.16f;
            if (rate > 1.0f) rate = 1.0f;
            g_chase_yaw += delta * rate;
            /* A tap is done once it has arrived; a hold keeps going until
             * the button comes up (g_recentre_held clears it). */
            if (!g_recentre_held && delta < 0.02f && delta > -0.02f) {
                g_chase_yaw = behind;
                g_recentring = false;
            }
        } else if (g_tune.chase_follow > 0.0f && walking && g_manual_hold == 0) {
            /* Trail him. Only while he is actually walking -- turning on
             * the spot to talk to someone should not move the camera --
             * and eased in over about a third of a second, so a corner
             * taken at speed reads as the view swinging round to follow
             * rather than snapping to the new heading. */
            float rate = g_tune.chase_follow;
            if (rate > 1.0f) rate = 1.0f;
            if (drift_age < 20) drift_age++;
            rate *= (float)drift_age / 20.0f;
            /* Close enough is close enough: without this the last fraction
             * of a degree keeps the camera imperceptibly creeping. */
            if (delta > 0.015f || delta < -0.015f) g_chase_yaw += delta * rate;
        } else {
            drift_age = 0;
        }
        /* Keep the angle in (-pi, pi] rather than letting a session's worth
         * of turns wind it into the thousands, where float steps coarsen. */
        while (g_chase_yaw >  3.1415927f) g_chase_yaw -= 6.2831853f;
        while (g_chase_yaw <= -3.1415927f) g_chase_yaw += 6.2831853f;
    }
    if (out_lx) *out_lx = lx;
    if (out_ly) *out_ly = ly;
}

static void render_chase(GBContext* ctx, const VoxTileGrid* grid,
                         const VoxSpriteList* sprites, int S, uint32_t* out) {
    const int OW = GB_SCREEN_WIDTH * S;
    const int OH = GB_SCREEN_HEIGHT * S;
    const int world_top = grid->hud_rows;
    const float HPX = g_tune.chase_hpx;
    const float FAR = 230.0f;

    for (int i = 0; i < OW * OH; i++) s_zbuf[i] = 1e9f;
    g_flatten_trees = true;
    prepare_chase_ground(grid);

    /* Sky first; the ground overwrites what it owns. */
    if (grid->sky != VOX_SKY_NONE) {
        vox_paint_sky(grid->sky, out, S);
    } else {
        for (int Y = 0; Y < OH; Y++) {
            int l = 26 + (Y * 20) / OH;
            uint32_t c = 0xFF000000u
                         | ((uint32_t)(l - 8 > 0 ? l - 8 : 0) << 16)
                         | ((uint32_t)l << 8) | (uint32_t)(l + 6);
            for (int X = 0; X < OW; X++) out[Y * OW + X] = c;
        }
    }

    float lx, ly;
    vox_chase_step(grid, &lx, &ly);

    /* Facing north (g_chase_yaw -pi/2) the forward vector is (0,-1) -- up the
     * screen -- and the viewer's right hand points east, (+1,0). That is
     * (-fy, fx). An earlier pass flipped this to (fy,-fx), which mirrored
     * the whole view: the world looked plausible but left and right were
     * swapped, so walking east moved you screen-left. */
    const float fxv = cosf(g_chase_yaw), fyv = sinf(g_chase_yaw);
    const float rxv = -fyv, ryv = fxv;      /* screen-space right vector */

    const float CAMH = g_tune.chase_height; /* and above their ground */
    /* Ease Link's whole-pixel anchor, then place the camera at a separately
     * smoothed distance. Obstacle clamps apply immediately; moving back out
     * to the requested distance is gentle. Smoothing the old target x/y
     * directly allowed it to spend several frames travelling through the
     * very wall the new target had avoided. */
    static float slx_f = 0.0f, sly_f = 0.0f, back_f = 0.0f;
    static bool cam_live = false;
    if (g_chase_pose_reset) {
        cam_live = false;
        g_chase_pose_reset = false;
    }
    if (!cam_live) {
        slx_f = lx;
        sly_f = ly;
        back_f = vox_chase_camera_back(grid, lx, ly, fxv, fyv,
                                       g_tune.chase_back);
        cam_live = true;
    }
    slx_f += (lx - slx_f) * 0.35f;
    sly_f += (ly - sly_f) * 0.35f;
    float allowed = vox_chase_camera_back(grid, slx_f, sly_f, fxv, fyv,
                                          g_tune.chase_back);
    if (allowed < back_f) back_f = allowed;
    else back_f += (allowed - back_f) * 0.12f;
    float cx = slx_f - fxv * back_f;
    float cy = sly_f - fyv * back_f;
    float cg = chase_height_at(grid, cx, cy);
    if (cg < 0.0f) cg = 0.0f;
    const float cz = cg * HPX + CAMH;

    const float focal = (float)OW * g_tune.chase_fov;
    const int horizon = (int)((float)OH * 0.36f);
    const uint32_t fog = (grid->sky != VOX_SKY_NONE)
        ? VOX_SKIES[grid->sky].horizon : 0xFF2C2620u;

    /* A treeline on the horizon: where the ground plane runs out into
     * fog, a painted silhouette of distant forest sits under the sky
     * instead of a bare seam. Its colour is the fog's own, darkened --
     * the same palette the environment is already lit by -- and its
     * ridge undulates so it reads as woods, not a ruler line. */
    if (grid->sky != VOX_SKY_NONE) {
        /* Forest green, hazed toward the sky's own horizon colour so the
         * woods sit IN the atmosphere rather than pasted over it. */
        uint32_t wood = lerp_color(0xFF1E3D20u, fog, 88);
        uint32_t wood2 = lerp_color(0xFF2E5629u, fog, 150);
        for (int X = 0; X < OW; X++) {
            float fx2 = (float)X / (float)S;
            int hgt = (int)((6.5f + 2.8f * sinf(fx2 * 0.093f)
                             + 1.9f * sinf(fx2 * 0.031f + 1.7f)
                             + 1.1f * sinf(fx2 * 0.21f + 4.2f)) * (float)S);
            int y0 = horizon - hgt;
            if (y0 < 0) y0 = 0;
            for (int Y = y0; Y < horizon + 3 * S && Y < OH; Y++) {
                /* Two ranks: a lighter far band behind the dark ridge. */
                out[Y * OW + X] = (Y < y0 + 2 * S) ? wood2 : wood;
            }
        }
    }

    for (int X = 0; X < OW; X++) {
        float ndc = ((float)X - (float)OW * 0.5f) / focal;
        float rx = fxv + rxv * ndc;
        float ry = fyv + ryv * ndc;
        int ybuf = OH;
        float ph = 0.0f;
        for (float d = 8.0f; d < FAR; d += (d < 70.0f ? 0.6f : 1.4f)) {
            float wx2 = cx + rx * d;
            float wy2 = cy + ry * d;
            /* Past the room's edge the heightfield has nothing to say, but
             * a sky-coloured hole in the floor is worse than a continued
             * ground plane: clamp the sample to the room border so the
             * world runs out into fog instead of falling away. */
            bool inside = wx2 >= 0.0f && wx2 < (float)GB_SCREEN_WIDTH &&
                          wy2 >= (float)world_top &&
                          wy2 < (float)GB_SCREEN_HEIGHT;
            float sx2 = wx2 < 0.0f ? 0.0f
                      : (wx2 > (float)(GB_SCREEN_WIDTH - 1)
                         ? (float)(GB_SCREEN_WIDTH - 1) : wx2);
            float sy2 = wy2 < (float)world_top ? (float)world_top
                      : (wy2 > (float)(GB_SCREEN_HEIGHT - 1)
                         ? (float)(GB_SCREEN_HEIGHT - 1) : wy2);
            int tex_col = (int)(sx2 + (float)grid->fine_x);
            if (tex_col < 0) tex_col = 0;
            if (tex_col >= VOX_TEX_W) tex_col = VOX_TEX_W - 1;
            /* Past the room's edge, the persistent world answers first: a
             * neighbouring room seen earlier this session continues the
             * terrain for real. Only where nobody has walked yet does the
             * border-clamp fade below take over. */
            float h;
            bool remembered = false;
            if (!inside) remembered = vox_world_height(wx2, wy2, &h);
            if (!remembered) h = chase_height_at(grid, sx2, sy2);
            if (!inside && !remembered) {
                /* Outside the room the border cell repeats, so a treeline
                 * at the edge keeps going instead of ending at a cliff of
                 * flat ground -- then settles to the fog plane over ~70px
                 * so it reads as "the world continues" rather than "an
                 * infinite wall". */
                float outd = 0.0f;
                if (wx2 < 0.0f) outd = -wx2;
                else if (wx2 > (float)(GB_SCREEN_WIDTH - 1))
                    outd = wx2 - (float)(GB_SCREEN_WIDTH - 1);
                if (wy2 < (float)world_top) {
                    float t2 = (float)world_top - wy2;
                    if (t2 > outd) outd = t2;
                } else if (wy2 > (float)(GB_SCREEN_HEIGHT - 1)) {
                    float t2 = wy2 - (float)(GB_SCREEN_HEIGHT - 1);
                    if (t2 > outd) outd = t2;
                }
                float k = 1.0f - outd * (1.0f / 70.0f);
                if (k < 0.0f) k = 0.0f;
                h *= k;
            }
            int sy = horizon + (int)((cz - h * HPX) * focal / d);
            if (sy < ybuf) {
                uint32_t c;
                if (!(remembered && vox_world_tex(wx2, wy2, &c)))
                    c = chase_tex_at(grid, sx2, sy2);
                if (h < 0.0f) {
                    c = shade(c, 190);
                    c = (c & 0xFFFFFF00u) | 0x00000050u;
                } else if (inside || remembered) {
                    /* The cube treatment, chase edition. Sub-texel edge
                     * bevels alias into a dot grid at ray-step
                     * granularity (seen live on a big window), so the
                     * perspective ground shades whole blocks instead --
                     * a per-texel checker that cannot moire -- and lets
                     * it fade before the fog takes over. */
                    if (g_tune.bevel > 0.01f && d < g_tune.fog_start) {
                        int k = (int)(g_tune.bevel * 24.0f
                                      * (1.0f - d / g_tune.fog_start));
                        int bx = (int)(remembered ? wx2 : sx2);
                        int by = (int)(remembered ? wy2 : sy2);
                        c = shade(c, 256 + (((bx + by) & 1) ? -k : k / 2));
                    }
                }
                if (!inside && !remembered) {
                    /* Nobody has walked there: the border cell repeats,
                     * which used to smear its columns to the horizon as
                     * visible streaks. Sink it into the fog with
                     * distance instead, so unexplored ground reads as
                     * haze the treeline sits on. */
                    float outd2 = 0.0f;
                    if (wx2 < 0.0f) outd2 = -wx2;
                    else if (wx2 > (float)(GB_SCREEN_WIDTH - 1))
                        outd2 = wx2 - (float)(GB_SCREEN_WIDTH - 1);
                    if (wy2 < (float)world_top) {
                        float t3 = (float)world_top - wy2;
                        if (t3 > outd2) outd2 = t3;
                    } else if (wy2 > (float)(GB_SCREEN_HEIGHT - 1)) {
                        float t3 = wy2 - (float)(GB_SCREEN_HEIGHT - 1);
                        if (t3 > outd2) outd2 = t3;
                    }
                    int t4 = 96 + (int)(outd2 * 5.0f);
                    if (t4 > 256) t4 = 256;
                    c = lerp_color(c, fog, t4);
                }
                int t = (int)((d - g_tune.fog_start) * 256.0f /
                              (FAR - g_tune.fog_start));
                if (t < 0) t = 0;
                if (t > (int)g_tune.fog_max) t = (int)g_tune.fog_max;
                c = lerp_color(c, fog, t);
                /* A step up in height means this span is mostly the riser's
                 * front face: darken it below a thin lit rim, so cliffs and
                 * tree lines read as forms instead of vertical streaks. */
                bool face = h > ph + 0.25f;
                int rim = top_rim(S);
                int top = sy < 0 ? 0 : sy;
                if (face && ybuf - top > rim + 1) {
                    /* A riser's front face folds the original 8px tile rows
                     * downward in complete bands. Planar edge quads overwrite
                     * this column fallback nearby; this keeps distant faces
                     * faithful too. */
                    int tile_top = ((int)(sy2 + (float)grid->fine_y)) & ~7;
                    if (tile_top < 0) tile_top = 0;
                    if (tile_top > VOX_TEX_H - 8) tile_top = VOX_TEX_H - 8;
                    int t2 = (int)((d - g_tune.fog_start) * 256.0f /
                                   (FAR - g_tune.fog_start));
                    if (t2 < 0) t2 = 0;
                    if (t2 > (int)g_tune.fog_max) t2 = (int)g_tune.fog_max;
                    for (int yy = top; yy < ybuf; yy++) {
                        uint32_t wc;
                        int art = ((yy - top) / S) & 7;
                        if (!(remembered &&
                              vox_world_face(wx2, wy2, art, &wc, NULL)))
                            wc = grid->tex[(tile_top + art) * VOX_TEX_W
                                           + tex_col];
                        /* The source texel owns the drawing. This modest
                         * orientation shade is the only invented colour on
                         * a riser; no masonry, trunk or seam pattern replaces
                         * what the cart actually drew. */
                        /* Same block-not-edge rule for riser faces: one
                         * checker cell per texel column and art row. */
                        if (g_tune.bevel > 0.01f && d < g_tune.fog_start) {
                            int k = (int)(g_tune.bevel * 24.0f
                                          * (1.0f - d / g_tune.fog_start));
                            int bx = (int)(remembered ? wx2 : sx2);
                            wc = shade(wc, 256 + (((bx + art) & 1)
                                                  ? -k : k / 2));
                        }
                        wc = lerp_color(shade(wc, yy < top + rim ? 246 : 226),
                                        fog, t2);
                        out[yy * OW + X] = wc;
                        s_zbuf[yy * OW + X] = d;
                    }
                } else {
                    for (int yy = top; yy < ybuf; yy++) {
                        out[yy * OW + X] = c;
                        s_zbuf[yy * OW + X] = d;
                    }
                }
                ybuf = top;
                if (ybuf <= 0) break;
            }
            ph = h;
        }
    }

    draw_chase_cliff_faces(grid, cx, cy, fxv, fyv, rxv, ryv,
                           focal, cz, HPX, horizon, fog, S, OW, OH, out);

    /* Compound facades own the same depth buffer as terrain, so Link/NPCs
     * naturally pass in front of the doorway and disappear behind the
     * house without a room-specific sprite-order rule. */
    for (int i = 0; i < grid->structure_count; i++) {
        const VoxStructure* b = &grid->structures[i];
        float ground = height_raw(grid,
                                  (float)b->sx + VOX_STRUCTURE_W * 0.5f,
                                  (float)b->sy + 8.0f);
        if (ground < 0.0f) ground = 0.0f;
        draw_voxel_structure(b, ground, cx, cy, fxv, fyv, rxv, ryv,
                             focal, cz, HPX, horizon, fog, OW, OH, out);
    }

    /* Sprites: adjacent 8px OAM entries at the same y are one character,
     * and drawing the halves as independent billboards let them drift into
     * twins with depth. Group each horizontal run, then draw runs farthest
     * first, scaled by depth, standing on their ground. */
    typedef struct {
        int members[6];      /* sprite indices, left to right */
        int n;
        int x, y, sh;
        float dep, lat, g;
        int elev;            /* world px above its ground (held items) */
        int parent;          /* lower OAM run that owns this metasprite */
        bool link_run;       /* Link's own row cannot parent a nearby NPC */
    } ChaseRun;
    ChaseRun runs[VOX_MAX_SPRITES];
    bool used[VOX_MAX_SPRITES] = {false};
    int nr = 0;
    for (int i = 0; i < sprites->count; i++) {
        if (used[i]) continue;
        const VoxSprite* s = &sprites->entries[i];
        /* Find the leftmost member of this run. */
        int lx0 = s->x;
        for (bool moved = true; moved;) {
            moved = false;
            for (int j = 0; j < sprites->count; j++) {
                if (!used[j] && sprites->entries[j].y == s->y &&
                    sprites->entries[j].x == lx0 - 8) {
                    lx0 -= 8;
                    moved = true;
                }
            }
        }
        ChaseRun* r = &runs[nr];
        r->n = 0;
        r->x = lx0;
        r->y = s->y;
        r->sh = s->tall ? 16 : 8;
        r->parent = -1;
        r->link_run = false;
        for (int step = 0; step < 6; step++) {
            int want = lx0 + step * 8;
            int found = -1;
            for (int j = 0; j < sprites->count; j++) {
                if (!used[j] && sprites->entries[j].y == s->y &&
                    sprites->entries[j].x == want) { found = j; break; }
            }
            if (found < 0) break;
            used[found] = true;
            r->members[r->n++] = found;
        }
        if (r->n == 0) continue;

        float pxc = (float)r->x + (float)(r->n * 8) * 0.5f;
        float pyc = (float)(r->y + r->sh);
        r->dep = (pxc - cx) * fxv + (pyc - cy) * fyv;
        r->lat = (pxc - cx) * rxv + (pyc - cy) * ryv;
        if (r->dep < 24.0f || r->dep > FAR) continue;
        r->g = chase_height_at(grid, pxc, pyc);
        if (r->g < 0.0f) r->g = 0.0f;
        r->elev = 0;
        if (grid->link_known) {
            float dxl = pxc - (float)grid->link_sx;
            if (dxl < 0.0f) dxl = -dxl;
            int dyf = (int)pyc - grid->link_feet_sy;
            if (dyf < 0) dyf = -dyf;
            r->link_run = dxl <= 5.0f && dyf <= 2;
        }
        /* A run hanging directly over Link's head is the item he is
         * holding up. Its 2D position is ABOVE him on screen, which the
         * ground-anchoring below would read as "16px further north" --
         * planting the sword in the dirt behind him. Anchor it at Link's
         * own ground instead, elevated by exactly the pixels the game
         * drew it above his feet, and pull it a hair nearer so it sorts
         * in front of him. */
        if (grid->link_known && grid->link_item_get) {
            int bot = r->y + r->sh;
            int dxl = (int)pxc - grid->link_sx;
            if (dxl < 0) dxl = -dxl;
            int above = grid->link_feet_sy - bot;
            if (dxl <= 12 && above >= 6 && above <= 32) {
                float lyf = (float)grid->link_feet_sy;
                r->dep = (pxc - cx) * fxv + (lyf - cy) * fyv - 1.0f;
                r->lat = (pxc - cx) * rxv + (lyf - cy) * ryv;
                if (r->dep < 24.0f || r->dep > FAR) continue;
                r->g = chase_height_at(grid, (float)grid->link_sx, lyf);
                if (r->g < 0.0f) r->g = 0.0f;
                r->elev = above;
            }
        }
        nr++;
    }

    /* Oracles composes tall characters as multiple horizontal OAM runs:
     * Link usually has one 16px row, while larger NPCs (Impa, Gorons, etc.)
     * stack a head row above a torso row. Projecting each row from its own
     * bottom made the head appear metres behind/beside the body whenever the
     * camera faced north or south. Attach a narrower/equal row whose bottom
     * is within 16px above a lower row to that lower row's world anchor.
     * A real held item is deliberately excluded; its item-get state uses the
     * elevation path above. */
    for (int i = 0; i < nr; i++) {
        if (runs[i].elev || runs[i].n <= 0) continue;
        float ic = (float)runs[i].x + (float)(runs[i].n * 8) * 0.5f;
        int ibot = runs[i].y + runs[i].sh;
        int best = -1, best_gap = 999;
        for (int j = 0; j < nr; j++) {
            if (i == j || runs[j].elev || runs[j].link_run ||
                runs[j].n < runs[i].n)
                continue;
            float jc = (float)runs[j].x + (float)(runs[j].n * 8) * 0.5f;
            float dc = ic - jc;
            if (dc < 0.0f) dc = -dc;
            if (dc > 4.0f) continue;
            int gap = ibot - runs[j].y;
            if (gap < 0 || gap > 5) continue;
            if (gap < best_gap) { best = j; best_gap = gap; }
        }
        if (best >= 0) runs[i].parent = best;
    }
    for (int pass = 0; pass < nr; pass++) {
        bool changed = false;
        for (int i = 0; i < nr; i++) {
            int p = runs[i].parent;
            if (p < 0) continue;
            while (runs[p].parent >= 0) p = runs[p].parent;
            if (runs[i].parent != p) { runs[i].parent = p; changed = true; }
            const ChaseRun* root = &runs[p];
            float root_c = (float)root->x + (float)(root->n * 8) * 0.5f;
            float root_y = (float)(root->y + root->sh);
            runs[i].dep = (root_c - cx) * fxv + (root_y - cy) * fyv;
            runs[i].lat = (root_c - cx) * rxv + (root_y - cy) * ryv;
            runs[i].g = root->g;
            runs[i].elev = (root->y + root->sh) -
                           (runs[i].y + runs[i].sh);
        }
        if (!changed) break;
    }
    if (getenv("VOX_DUMP_RUNS")) {
        fprintf(stderr,
                "[VOXEL] chase sprites cam=(%.1f,%.1f) f=(%.2f,%.2f) "
                "link=(%d,%d)\n",
                cx, cy, fxv, fyv, grid->link_sx, grid->link_feet_sy);
        for (int i = 0; i < nr; i++) {
            const ChaseRun* r = &runs[i];
            fprintf(stderr,
                    "  run %02d n=%d pos=(%3d,%3d) dep=%6.1f lat=%6.1f "
                    "scale=%5.2f elev=%d parent=%d members=",
                    i, r->n, r->x, r->y, r->dep, r->lat,
                    focal / r->dep, r->elev, r->parent);
            for (int j = 0; j < r->n; j++)
                fprintf(stderr, "%s%d", j ? "," : "", r->members[j]);
            fputc('\n', stderr);
        }
    }
    for (int a = 1; a < nr; a++) {
        ChaseRun tmp = runs[a];
        int b = a - 1;
        while (b >= 0 && runs[b].dep < tmp.dep) {
            runs[b + 1] = runs[b];
            b--;
        }
        runs[b + 1] = tmp;
    }

    /* Vegetation, depth-sorted into the same painter's pass as sprites, so a
     * character walks behind one trunk and in front of the next tree. */
    /* Candidates: the live room's trees, then the remembered neighbours' --
     * a forest keeps its far ranks across the room border. */
    enum { VOX_MAX_BILLBOARDS = VOX_MAX_TREES + 192 };
    static VoxWorldTree cand[VOX_MAX_BILLBOARDS];
    int nc = 0;
    for (int i = 0; i < grid->tree_count; i++) {
        cand[nc].t = &grid->trees[i];
        cand[nc].sx = grid->trees[i].sx;
        cand[nc].sy = grid->trees[i].sy;
        nc++;
    }
    const int live_trees = nc;
    nc += vox_world_neighbor_trees(cand + nc, VOX_MAX_BILLBOARDS - nc);

    int nt = 0;
    static float tdep[VOX_MAX_BILLBOARDS], tg[VOX_MAX_BILLBOARDS];
    static int tsx[VOX_MAX_BILLBOARDS], tsy[VOX_MAX_BILLBOARDS];
    static const VoxTree* tptr[VOX_MAX_BILLBOARDS];
    for (int i = 0; i < nc; i++) {
        const VoxTree* t = cand[i].t;
        float pxc = (float)cand[i].sx + 8.0f;
        float pyc = (float)cand[i].sy + 16.0f;
        float dep = (pxc - cx) * fxv + (pyc - cy) * fyv;
        if (dep < 16.0f || dep > FAR) continue;
        tdep[nt] = dep;
        float gh;
        if (i < live_trees) {
            gh = chase_height_at(grid, pxc, pyc);
        } else if (!vox_world_height(pxc, pyc, &gh)) {
            gh = 0.0f;
        }
        tg[nt] = gh < 0.0f ? 0.0f : gh;
        tsx[nt] = cand[i].sx;
        tsy[nt] = cand[i].sy;
        tptr[nt] = t;
        nt++;
    }
    for (int a = 1; a < nt; a++) {
        float d0 = tdep[a], g0 = tg[a];
        int x0 = tsx[a], y0 = tsy[a];
        const VoxTree* p0 = tptr[a];
        int b = a - 1;
        while (b >= 0 && tdep[b] < d0) {
            tdep[b + 1] = tdep[b]; tg[b + 1] = tg[b];
            tsx[b + 1] = tsx[b]; tsy[b + 1] = tsy[b];
            tptr[b + 1] = tptr[b];
            b--;
        }
        tdep[b + 1] = d0; tg[b + 1] = g0;
        tsx[b + 1] = x0; tsy[b + 1] = y0;
        tptr[b + 1] = p0;
    }

    int ri = 0, ti = 0;
    while (ri < nr || ti < nt) {
        if (ti < nt && (ri >= nr || tdep[ti] >= runs[ri].dep)) {
            const VoxTree* t = tptr[ti];
            float ground = tg[ti];
            int object_sx = tsx[ti], object_sy = tsy[ti];
            ti++;
            draw_voxel_tree(t, object_sx, object_sy, ground,
                            cx, cy, fxv, fyv, rxv, ryv,
                            focal, cz, HPX, horizon, fog, OW, OH, out);
            continue;
        }
        const ChaseRun* r = &runs[ri++];
        float scale = focal / r->dep;
        int wgb = r->n * 8;
        int cxp = (int)((float)OW * 0.5f + r->lat / r->dep * focal);
        int byp = horizon +
            (int)((cz - (r->g * HPX + (float)r->elev)) * focal / r->dep);
        int w2 = (int)((float)wgb * scale / (float)S + 0.5f) * S;
        int h2 = (int)((float)r->sh * scale / (float)S + 0.5f) * S;
        if (w2 < 2 || h2 < 2) continue;
        int left = cxp - w2 / 2;
        for (int m = 0; m < r->n; m++) {
            const VoxSprite* s = &sprites->entries[r->members[m]];
            for (int row = 0; row < r->sh; row++) {
                uint32_t px8[8];
                vox_decode_sprite_row(ctx, s, row, px8);
                int y0 = byp - h2 + row * h2 / r->sh;
                int y1 = byp - h2 + (row + 1) * h2 / r->sh;
                for (int yy = y0; yy < y1; yy++) {
                    if (yy < 0 || yy >= OH) continue;
                    for (int c2 = 0; c2 < 8; c2++) {
                        if (!px8[c2]) continue;
                        int gcol = m * 8 + c2;
                        int x0 = left + gcol * w2 / wgb;
                        int x1 = left + (gcol + 1) * w2 / wgb;
                        for (int xx = x0; xx < x1; xx++) {
                            if (xx < 0 || xx >= OW) continue;
                            /* Terrain nearer than the sprite hides it.
                             * The 3px slack keeps the ground a sprite
                             * STANDS on -- which sits at essentially the
                             * same depth -- from eating their feet. */
                            if (s_zbuf[yy * OW + xx] < r->dep - 3.0f)
                                continue;
                            out[yy * OW + xx] = px8[c2];
                        }
                    }
                }
            }
        }
    }

    /* The diorama-photo finish: focus on the band Link stands in, melt
     * the sky and the near foreground a little. The HUD and any dialog
     * are composited after this and stay crisp. */
    vox_tilt_shift(out, OW, OH, S, 0, (int)((float)OH * 0.62f));
}

/* ------------------------------------------------------------------ */
/* the diorama                                                         */
/* ------------------------------------------------------------------ */

void vox_render(GBContext* ctx, const VoxTileGrid* grid,
                const VoxSpriteList* sprites, const uint32_t* fb,
                int mode, int scale, uint32_t* out) {
    const VoxCam cam = VOX_CAMS[mode];
    if (scale < 1) scale = 1;
    if (scale > VOX_MAX_SCALE) scale = VOX_MAX_SCALE;
    const int S = scale;
    const float fS = (float)S;
    const int OW = GB_SCREEN_WIDTH * S;
    const int OH = GB_SCREEN_HEIGHT * S;

    /* A full-screen menu owns the display: pass the frame through (the
     * live hook returns NULL before ever calling here; this path serves
     * direct callers like vox_shot). */
    if (grid->flat) {
        for (int Y = 0; Y < OH; Y++) {
            const uint32_t* src = fb + (Y / S) * GB_SCREEN_WIDTH;
            for (int X = 0; X < OW; X++) out[Y * OW + X] = src[X / S];
        }
        return;
    }

    /* The status bar is a band at the top; the world is everything under it
     * and stays flat inside its own band at the end. */
    const int world_top = grid->hud_rows;
    const int world_h = GB_SCREEN_HEIGHT - world_top;
    if (world_h <= 0) {
        for (int Y = 0; Y < OH; Y++) {
            const uint32_t* src = fb + (Y / S) * GB_SCREEN_WIDTH;
            for (int X = 0; X < OW; X++) out[Y * OW + X] = src[X / S];
        }
        return;
    }

    if (mode == VOXEL_MODE_CHASE) {
        render_chase(ctx, grid, sprites, S, out);
        g_flatten_trees = false;   /* diorama modes keep extruded trees */
        goto compose_overlays;
    }

    /* Squashing the world frees vertical room; spend it lifting the diorama
     * so tall geometry has somewhere to go without clipping off the top.
     * A prop on a plateau can add another course above the shelf. */
    float max_height = VOX_UNITS[VOX_H_HIGH];
    for (int ty = 0; ty < VOX_TILES_H; ty++) {
        for (int tx = 0; tx < VOX_TILES_W; tx++) {
            float h = cell_base(grid, tx, ty)
                    + VOX_UNITS[grid->height[ty][tx]];
            if (h > max_height) max_height = h;
        }
    }
    const float headroom = max_height * cam.lift * g_tune.tilt_scale;
    float y_off = (float)world_top + ((float)world_h * (1.0f - cam.squash)) * 0.5f;
    y_off += headroom * 0.55f;

    /* Backdrop. Outdoors on the Oracles carts this is a sky that follows
     * the game's own state; anywhere else it stays the quiet dark fade. */
    if (grid->sky != VOX_SKY_NONE) {
        vox_paint_sky(grid->sky, out, S);
    } else {
        for (int Y = 0; Y < OH; Y++) {
            int l = 26 + (Y / S) * 20 / GB_SCREEN_HEIGHT;
            uint32_t c = 0xFF000000u
                         | ((uint32_t)(l - 8 > 0 ? l - 8 : 0) << 16)
                         | ((uint32_t)l << 8) | (uint32_t)(l + 6);
            for (int X = 0; X < OW; X++) out[Y * OW + X] = c;
        }
    }

    /* Water animates: a slow ripple in the shading. */
    static int water_t = 0;
    water_t++;

    /* When real heights return after a transition's flat slide, grow the
     * extrusion in over a few frames -- snapping from flat to full relief
     * in one frame reads as a glitch, growing in reads as intent. */
    bool extruded = false;
    for (int ty = 0; ty < VOX_TILES_H && !extruded; ty++) {
        for (int tx = 0; tx < VOX_TILES_W; tx++) {
            if (grid->elevation[ty][tx] ||
                VOX_UNITS[grid->height[ty][tx]] != 0.0f) {
                extruded = true;
                break;
            }
        }
    }
    static float grow = 1.0f;
    static bool was_extruded = false;
    if (extruded && !was_extruded) grow = 0.0f;
    was_extruded = extruded;
    grow += (1.0f - grow) * 0.18f;
    if (grow > 0.999f) grow = 1.0f;
    const float lift = cam.lift * grow * g_tune.tilt_scale;

    /* Link's ground height, eased over a few frames: point-sampling the
     * cell under his feet made his billboard teleport a full step the
     * instant he crossed onto raised or lowered ground. */
    static float link_ground_ease = 0.0f;
    static bool link_ease_live = false;
    if (grid->link_known) {
        int fy = grid->link_feet_sy;
        if (fy < world_top) fy = world_top;
        if (fy >= GB_SCREEN_HEIGHT) fy = GB_SCREEN_HEIGHT - 1;
        float now = height_at(grid, (float)grid->link_sx, (float)fy);
        if (now < 0.0f) now = 0.0f;
        if (!link_ease_live) {
            link_ground_ease = now;
            link_ease_live = true;
        }
        link_ground_ease += (now - link_ground_ease) * 0.25f;
    } else {
        link_ease_live = false;
    }

    /* One pass over output rows, far to near, terrain and sprites together
     * in painter's order. Per-output-column memory of the previous row's
     * projection drives the front-wall fills. */
    static int prev_sy[GB_SCREEN_WIDTH * VOX_MAX_SCALE];
    for (int X = 0; X < OW; X++) prev_sy[X] = -1;

    for (int J = world_top * S; J < (GB_SCREEN_HEIGHT + 16) * S; J++) {
        const float wy = (float)J / fS;
        if (J < OH) {
            const int tex_row = (int)(wy + (float)grid->fine_y);
            for (int X = 0; X < OW; X++) {
                const float wx = (float)X / fS;
                float h = height_at(grid, wx, wy);
                int sy = (int)((y_off + (wy - (float)world_top) * cam.squash
                                - h * lift) * fS + 0.5f);
                const int tex_col = (int)(wx + (float)grid->fine_x);
                uint32_t tex = grid->tex[tex_row * VOX_TEX_W + tex_col];
                if (h >= 0.0f) tex = bevel_px(tex, wx, wy);

                if (h < 0.0f) {
                    /* Water: tint toward deep blue so sunk cells read as
                     * liquid, with a slow moving shimmer on top. */
                    int rip = (int)(12.0f * sinf(wx * 0.42f + wy * 0.27f
                                                 + (float)water_t * 0.09f));
                    tex = shade(tex, 190 + rip);
                    tex = (tex & 0xFFFFFF00u) | 0x00000050u;
                }

                if (prev_sy[X] >= 0 && sy > prev_sy[X] + 1) {
                    /* Height dropped toward the viewer: the far cell's front
                     * wall is exposed. Texture it by tiling that cell's own
                     * 8px artwork down the face, darkening with a little
                     * falloff so faces read as faces. */
                    int span = sy - prev_sy[X] - 1;
                    int src_py = (int)(wy - 1.0f + (float)grid->fine_y);
                    /* Never source wall texture from above the world's
                     * first row -- the tile rows under the HUD band hold
                     * whatever the map wraps to there (HUD tiles). */
                    if (src_py < world_top) src_py = world_top;
                    int tile_top = src_py & ~7;
                    /* Foliage faces stretch their art once down the wall;
                     * tiling it made tall trees a busy stack of repeats.
                     * Masonry and rock keep the tiling -- stacked courses
                     * are exactly what a cliff face should look like. */
                    bool leafy_wall =
                        grid->leafy[tile_top >> 3][tex_col >> 3] != 0;
                    for (int fy = prev_sy[X] + 1; fy < sy; fy++) {
                        if (fy < world_top * S || fy >= OH) continue;
                        int d = fy - prev_sy[X];
                        int art_row;
                        if (leafy_wall) {
                            art_row = (span > 0) ? ((d - 1) * 8 / (span + 1)) : 0;
                            if (art_row > 7) art_row = 7;
                        } else {
                            art_row = (d / S) & 7;
                        }
                        uint32_t wall = grid->tex[(tile_top + art_row)
                                                      * VOX_TEX_W + tex_col];
                        int t = d * 52 / (span + 1);
                        out[fy * OW + X] = shade(wall, 186 - t);
                    }
                }

                if (sy >= world_top * S && sy < OH) {
                    /* Slight top-light on raised ground helps height pop. */
                    out[sy * OW + X] = (h > 0.0f) ? shade(tex, 272) : tex;
                }
                prev_sy[X] = sy;
            }
        }

        /* Sprites standing on this world row (once per GB row). */
        if (J % S != 0) continue;
        const int wy_i = J / S;
        for (int i = 0; i < sprites->count; i++) {
            const VoxSprite* s = &sprites->entries[i];
            int sh = s->tall ? 16 : 8;
            int feet = s->y + sh;          /* sprites stand on their bottom edge */

            /* Link mid-jump: the game draws his sprite higher, but his
             * shadow -- and therefore his billboard's anchor -- stays on
             * the ground he jumped from. Re-anchor his OAM sprites to the
             * ground row and lift the body instead. */
            int air = 0;
            if (grid->link_known && grid->link_jump > 0 &&
                s->x + 4 >= grid->link_sx - 10 && s->x + 4 <= grid->link_sx + 10 &&
                feet >= grid->link_feet_sy - grid->link_jump - 4 &&
                feet <= grid->link_feet_sy - grid->link_jump + 4) {
                air = grid->link_jump;
                feet = grid->link_feet_sy;
            }
            if (feet != wy_i) continue;
            if (feet < world_top) continue;   /* lives in the HUD band */

            int fy = feet >= GB_SCREEN_HEIGHT ? GB_SCREEN_HEIGHT - 1 : feet;
            /* Characters are drawn as runs of adjacent 8px OAM sprites.
             * Anchoring each half to the ground under its own feet split
             * them apart whenever a character straddled a height boundary,
             * so find the horizontal run this sprite belongs to and anchor
             * the whole run at its shared centre. */
            int run_l = s->x, run_r = s->x;
            for (bool moved = true; moved;) {
                moved = false;
                for (int j = 0; j < sprites->count; j++) {
                    const VoxSprite* o = &sprites->entries[j];
                    if (o->y != s->y) continue;
                    if (o->x == run_l - 8) { run_l = o->x; moved = true; }
                    if (o->x == run_r + 8) { run_r = o->x; moved = true; }
                }
            }
            float anchor_x = (float)(run_l + run_r + 8) * 0.5f;
            float ground = height_at(grid, anchor_x, (float)fy);
            if (ground < 0.0f) ground = 0.0f;   /* stand on the water surface */
            /* Link rides the eased ground instead of the raw cell sample,
             * so stepping across a height boundary ramps instead of pops. */
            if (link_ease_live &&
                s->x + 4 >= grid->link_sx - 10 && s->x + 4 <= grid->link_sx + 10 &&
                feet >= grid->link_feet_sy - 6 && feet <= grid->link_feet_sy + 6) {
                ground = link_ground_ease;
            }
            int base = (int)((y_off + (float)(fy - world_top) * cam.squash
                              - ground * lift) * fS + 0.5f);

            /* Airborne pixels rise like terrain does: 16 world px of jump
             * equals one HIGH block of extrusion. */
            int body_lift = (int)(((float)air * (VOX_UNITS[VOX_H_HIGH] / 16.0f)
                                   * lift + (float)air) * fS + 0.5f);

            for (int row = 0; row < sh; row++) {
                uint32_t px[8];
                vox_decode_sprite_row(ctx, s, row, px);
                int sy0 = base - body_lift - (sh - row) * S;
                for (int sub = 0; sub < S; sub++) {
                    int SY = sy0 + sub;
                    if (SY < world_top * S || SY >= OH) continue;
                    for (int c = 0; c < 8; c++) {
                        if (!px[c]) continue;
                        int sx = s->x + c;
                        if (sx < 0 || sx >= GB_SCREEN_WIDTH) continue;
                        uint32_t* dst = &out[SY * OW + sx * S];
                        for (int k = 0; k < S; k++) dst[k] = px[c];
                    }
                }
            }

            /* A soft contact shadow sells the billboard standing on the
             * ground instead of floating over it. */
            for (int sub = 0; sub < S; sub++) {
                int SY = base + sub;
                if (SY < world_top * S || SY >= OH) continue;
                for (int c = 1; c < 7; c++) {
                    int sx = s->x + c;
                    if (sx < 0 || sx >= GB_SCREEN_WIDTH) continue;
                    uint32_t* p = &out[SY * OW + sx * S];
                    for (int k = 0; k < S; k++) p[k] = shade(p[k], 170);
                }
            }
        }
    }

    /* The tilted diorama gets the same photo finish as the chase view:
     * sharp through the middle of the scene, melting toward the top and
     * bottom edges. HUD and dialog composite after, staying crisp. */
    vox_tilt_shift(out, OW, OH, S, world_top * S,
                   (world_top * S + OH) / 2);

compose_overlays:
    /* A dialog box floats flat over the frozen diorama, exactly as the
     * game drew it -- the world stays voxel, the words stay words. */
    if (grid->text_overlay && grid->box_w > 0) {
        /* In chase cam the box's 2D screen position means nothing (it was
         * landing in the sky over a 3D view), so pin it to the bottom of
         * the screen where a dialog belongs. The diorama modes keep it
         * exactly where the game drew it. */
        int dy = 0;
        if (mode == VOXEL_MODE_CHASE) {
            dy = (GB_SCREEN_HEIGHT - 4) - (grid->box_y + grid->box_h);
        }
        for (int y = grid->box_y; y < grid->box_y + grid->box_h; y++) {
            const uint32_t* src = fb + y * GB_SCREEN_WIDTH;
            int oy = y + dy;
            if (oy < 0 || oy >= GB_SCREEN_HEIGHT) continue;
            for (int sub = 0; sub < S; sub++) {
                uint32_t* dst = &out[(oy * S + sub) * OW];
                for (int x = grid->box_x; x < grid->box_x + grid->box_w; x++) {
                    for (int k = 0; k < S; k++) dst[x * S + k] = src[x];
                }
            }
        }
    }

    /* The status bar comes back exactly as the game drew it. */
    for (int Y = 0; Y < world_top * S; Y++) {
        const uint32_t* src = fb + (Y / S) * GB_SCREEN_WIDTH;
        for (int X = 0; X < OW; X++) out[Y * OW + X] = src[X / S];
    }
}

/* ------------------------------------------------------------------ */
/* frame hook                                                          */
/* ------------------------------------------------------------------ */

static void poll_toggle_key(void) {
#ifdef GB_HAS_SDL2
    /* The platform pumps SDL events every frame, so keyboard state is
     * already current by the time the frame hook runs. Edge-detect F3. */
    static bool was_down = false;
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    bool down = keys && keys[SDL_SCANCODE_F3];
    if (down && !was_down) {
        voxel_set_mode((g_mode + 1) % VOXEL_MODE_COUNT);
    }
    was_down = down;
#endif
}

/* Returning NULL leaves the guest frame completely untouched, which is what
 * happens whenever voxel mode is off -- the default -- and whenever the
 * cart is showing something that should stay flat (menus, dialog,
 * cutscenes). The game plays exactly as it always did unless you ask for
 * the diorama. */
static const uint32_t* voxel_frame_hook(GBContext* ctx, const uint32_t* fb,
                                        int* out_w, int* out_h) {
    static bool was_scripted = false;
    poll_toggle_key();
    if (g_mode == VOXEL_MODE_OFF || !ctx || !fb) return NULL;
    if (!vox_scrape(ctx, fb, &g_grid, &g_sprites)) return NULL;
    if (g_grid.flat) return NULL;
    int render_mode = vox_scene_render_mode(g_mode, g_grid.scripted_scene);
    if (was_scripted && !g_grid.scripted_scene && g_mode == VOXEL_MODE_CHASE) {
        /* Re-enter chase cleanly behind Link instead of resuming a stale pose
         * from before the script moved him or the room camera. */
        g_chase_heading_live = false;
        g_chase_pose_reset = true;
    }
    was_scripted = g_grid.scripted_scene;
    vox_render(ctx, &g_grid, &g_sprites, fb, render_mode, g_scale, g_out);
    *out_w = GB_SCREEN_WIDTH * g_scale;
    *out_h = GB_SCREEN_HEIGHT * g_scale;
    return g_out;
}

/* Camera-relative movement for the chase camera.
 *
 * The d-pad is world-fixed: right always walks east. That is invisible in
 * a top-down game, and wrong the moment the camera can turn -- east is
 * only screen-right while the camera looks north. This rotates the
 * pressed direction into the camera's frame, so "right" is always the
 * right of what you can see.
 *
 * Bits are active-low: Down, Up, Left, Right in bits 3..0.
 */
uint8_t vox_chase_remap_pressed(uint8_t pressed, float yaw) {
    pressed &= 0x0F;
    /* Screen-space direction the player asked for (y grows downward). */
    float dx = 0.0f, dy = 0.0f;
    if (pressed & 0x01) dx += 1.0f;   /* right */
    if (pressed & 0x02) dx -= 1.0f;   /* left  */
    if (pressed & 0x04) dy -= 1.0f;   /* up    */
    if (pressed & 0x08) dy += 1.0f;   /* down  */
    if (dx == 0.0f && dy == 0.0f) return 0;

    /* Rotate out of the camera's frame into the world. The camera's
     * forward is world "screen up", its right is world "screen right".
     * Since screen y grows downward, requested up is -dy along forward. */
    const float fx = cosf(yaw), fy = sinf(yaw);
    const float rx = -fy, ry = fx;
    const float wx = rx * dx - fx * dy;
    const float wy = ry * dx - fy * dy;

    /* Snap to the four directions the game actually accepts. Diagonals
     * survive because both axes can clear their threshold. */
    uint8_t out = 0;
    const float T = 0.38f;
    if (wx >  T) out |= 0x01;
    if (wx < -T) out |= 0x02;
    if (wy < -T) out |= 0x04;
    if (wy >  T) out |= 0x08;
    return out;
}

void voxel_remap_dpad(void) {
    if (g_mode != VOXEL_MODE_CHASE || g_grid.scripted_scene) return;

    const uint8_t pressed = (uint8_t)(~g_joypad_dpad & 0x0F);
    if (!pressed) return;
    const uint8_t out = vox_chase_remap_pressed(pressed, g_chase_yaw);
    if (!out) return;

    g_joypad_dpad = (uint8_t)((g_joypad_dpad | 0x0F) & ~out);
}

void voxel_terrain_status(int* live, int* scroll_mode, const char** reason) {
    vox_oracle_status(live, scroll_mode, reason);
}

void voxel_install(void) {
    voxel_tuning_load();
    const char* s = getenv("VOXEL_SCALE");
    if (s) {
        int v = atoi(s);
        if (v >= 1 && v <= VOX_MAX_SCALE) g_scale = v;
    }
    gb_platform_set_frame_hook_scaled(voxel_frame_hook);
    fprintf(stderr,
            "[VOXEL] available at %dx internal scale "
            "(off by default; F3 cycles OFF/15/30/45)\n", g_scale);
}
