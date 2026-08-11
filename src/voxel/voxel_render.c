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
    .chase_height = 32.0f,                  \
    .chase_fov    = 0.62f,                  \
    .chase_hpx    = 3.2f,                   \
    .chase_follow = 0.0f,                   \
    .chase_recenter = 0.16f,                \
    .cliff_unify = 1.0f,                    \
    .fog_start    = 70.0f,                  \
    .fog_max      = 150.0f,                 \
}

static VoxelTuning g_tune = VOX_TUNE_INIT;
static const VoxelTuning VOX_TUNE_DEFAULTS = VOX_TUNE_INIT;

/* VOX_UNITS was an array; keep the spelling so the call sites read the
 * same, but source the numbers from the live block. */
#define VOX_UNITS (g_tune.units)

VoxelTuning* voxel_tuning(void) { return &g_tune; }

float voxel_chase_yaw(void) { return g_chase_yaw; }

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
    }
    fclose(f);
    fprintf(stderr, "[VOXEL] tuning loaded from %s\n", TUNE_PATH);
}

int voxel_get_mode(void) { return g_mode; }

void voxel_set_mode(int mode) {
    if (mode < 0 || mode >= VOXEL_MODE_COUNT) mode = VOXEL_MODE_OFF;
    if (mode != g_mode) {
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

/* While the chase camera renders, tree cells drop out of the heightfield:
 * their trunks and canopies are drawn as billboards instead, so the ground
 * plane runs beneath them. The diorama never sets this and keeps its
 * extruded domes. */
static bool g_flatten_trees = false;

/* Height class of one tile, clamped to the grid. */
static inline float cell_h(const VoxTileGrid* grid, int tx, int ty) {
    if (tx < 0) tx = 0;
    if (tx >= VOX_TILES_W) tx = VOX_TILES_W - 1;
    if (ty < 0) ty = 0;
    if (ty >= VOX_TILES_H) ty = VOX_TILES_H - 1;
    if (g_flatten_trees && grid->treecell[ty][tx])
        return VOX_UNITS[VOX_H_FLOOR];
    return VOX_UNITS[grid->height[ty][tx]];
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
    if (g_flatten_trees && grid->treecell[ty][tx])
        return VOX_UNITS[VOX_H_FLOOR];
    float h = VOX_UNITS[grid->height[ty][tx]];
    if (h <= 0.0f || !grid->leafy[ty][tx]) return h;

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
    return h * k;
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

static inline uint32_t chase_tex_at(const VoxTileGrid* grid, float x, float y) {
    int px = (int)(x + (float)grid->fine_x);
    int py = (int)(y + (float)grid->fine_y);
    if (px < 0) px = 0;
    if (px >= VOX_TEX_W) px = VOX_TEX_W - 1;
    if (py < 0) py = 0;
    if (py >= VOX_TEX_H) py = VOX_TEX_H - 1;
    return grid->tex[py * VOX_TEX_W + px];
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
    float turn = 0.0f;
#ifdef GB_HAS_SDL2
    {
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        if (keys) {
            if (keys[SDL_SCANCODE_Q]) turn -= 1.0f;
            if (keys[SDL_SCANCODE_E]) turn += 1.0f;
        }
        SDL_GameController* pad = SDL_GameControllerFromPlayerIndex(0);
        if (pad) {
            Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
            if (ax > 6000 || ax < -6000) turn += (float)ax / 32767.0f;
        }
        /* Recentre: right stick click, or C. Either one held keeps the
         * camera behind him; a tap starts an ease that finishes itself. */
        bool want_recentre = keys && keys[SDL_SCANCODE_C];
        if (pad && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
            want_recentre = true;
        voxel_chase_recenter(want_recentre);
    }
#endif
    {
        /* Walking, not just facing: Link's own position has to have moved,
         * and a room transition teleports it, so only small steps count.
         * Only the optional walk-follow uses this. */
        static float prev_lx = 0.0f, prev_ly = 0.0f;
        static bool  pos_live = false;
        static int   manual_hold = 0;
        float dx = lx - prev_lx, dy = ly - prev_ly;
        bool walking = pos_live && (dx * dx + dy * dy) > 0.02f &&
                       (dx * dx + dy * dy) < 64.0f;
        prev_lx = lx; prev_ly = ly; pos_live = true;

        /* 0 up, 1 right, 2 down, 3 left -- screen yaw has +y downward,
         * so up is -pi/2 and down is +pi/2. */
        static const float DIR_YAW[4] = {
            -1.5707963f, 0.0f, 1.5707963f, 3.1415927f
        };
        const float behind = DIR_YAW[grid->link_dir & 3];

        if (turn != 0.0f) {
            g_chase_yaw += turn * 0.055f;
            manual_hold = 1;
            g_recentring = false;     /* the stick always wins */
        } else if (walking) {
            manual_hold = 0;
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
        } else if (g_tune.chase_follow > 0.0f && walking && !manual_hold) {
            float rate = g_tune.chase_follow;
            if (rate > 1.0f) rate = 1.0f;
            g_chase_yaw += delta * rate;
        }
        /* Keep the angle in (-pi, pi] rather than letting a session's worth
         * of turns wind it into the thousands, where float steps coarsen. */
        while (g_chase_yaw >  3.1415927f) g_chase_yaw -= 6.2831853f;
        while (g_chase_yaw <= -3.1415927f) g_chase_yaw += 6.2831853f;
    }

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

    const float BACK = g_tune.chase_back;   /* camera behind the player */
    const float CAMH = g_tune.chase_height; /* and above their ground */
    /* Ease the camera's position so Link's whole-pixel steps (and room
     * transitions) glide instead of ticking. */
    static float scx_f = 0.0f, scy_f = 0.0f;
    static bool cam_live = false;
    float tx = lx - fxv * BACK;
    float ty = ly - fyv * BACK;
    if (!cam_live) { scx_f = tx; scy_f = ty; cam_live = true; }
    scx_f += (tx - scx_f) * 0.35f;
    scy_f += (ty - scy_f) * 0.35f;
    float cx = scx_f, cy = scy_f;
    float cg = chase_height_at(grid, cx, cy);
    if (cg < 0.0f) cg = 0.0f;
    const float cz = cg * HPX + CAMH;

    const float focal = (float)OW * g_tune.chase_fov;
    const int horizon = (int)((float)OH * 0.36f);
    const uint32_t fog = (grid->sky != VOX_SKY_NONE)
        ? VOX_SKIES[grid->sky].horizon : 0xFF2C2620u;

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
                const uint32_t ground_raw = c;
                if (h < 0.0f) {
                    c = shade(c, 190);
                    c = (c & 0xFFFFFF00u) | 0x00000050u;
                }
                if (!inside && !remembered) c = lerp_color(c, fog, 96);
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
                    /* A riser's front face. Tile the cell's own 8px artwork
                     * down it at native chunkiness -- the diorama already
                     * does this and its cliffs read as rock courses, where
                     * stretching the art once down the face (the old way)
                     * smeared it into vertical taffy. */
                    int tile_top = ((int)(sy2 + (float)grid->fine_y)) & ~7;
                    if (tile_top < 0) tile_top = 0;
                    if (tile_top > VOX_TEX_H - 8) tile_top = VOX_TEX_H - 8;
                    int span = ybuf - top;
                    int t2 = (int)((d - g_tune.fog_start) * 256.0f /
                                   (FAR - g_tune.fog_start));
                    if (t2 < 0) t2 = 0;
                    if (t2 > (int)g_tune.fog_max) t2 = (int)g_tune.fog_max;
                    /* A tree's face is not cliff all the way down: below
                     * the canopy it falls into bark shadow, which is what
                     * separates "tree" from "green wall" at eye level. The
                     * bark keeps the cell's own colour cast so autumn and
                     * winter palettes still land. */
                    bool leafy_face =
                        grid->leafy[tile_top >> 3][tex_col >> 3] != 0;
                    if (remembered) {
                        uint32_t dummy;
                        vox_world_face(wx2, wy2, 0, &dummy, &leafy_face);
                    }
                    int trunk_y = (leafy_face && span >= 8 * S)
                        ? top + (span * 3) / 5 : ybuf;
                    uint32_t bark = lerp_color(shade(ground_raw, 70),
                                               0xFF3A2A20u, 120);
                    bark = lerp_color(bark, fog, t2);
                    for (int yy = top; yy < ybuf; yy++) {
                        uint32_t wc;
                        if (yy < top + rim) {
                            wc = c;
                        } else if (yy >= trunk_y) {
                            wc = bark;
                        } else {
                            int art = ((yy - top) / S) & 7;
                            if (!(remembered &&
                                  vox_world_face(wx2, wy2, art, &wc, NULL)))
                                wc = grid->tex[(tile_top + art) * VOX_TEX_W
                                               + tex_col];
                            wc = lerp_color(shade(wc, 168), fog, t2);
                        }
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
        /* A run hanging directly over Link's head is the item he is
         * holding up. Its 2D position is ABOVE him on screen, which the
         * ground-anchoring below would read as "16px further north" --
         * planting the sword in the dirt behind him. Anchor it at Link's
         * own ground instead, elevated by exactly the pixels the game
         * drew it above his feet, and pull it a hair nearer so it sorts
         * in front of him. */
        if (grid->link_known) {
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
    for (int a = 1; a < nr; a++) {
        ChaseRun tmp = runs[a];
        int b = a - 1;
        while (b >= 0 && runs[b].dep < tmp.dep) {
            runs[b + 1] = runs[b];
            b--;
        }
        runs[b + 1] = tmp;
    }

    /* Billboard trees, depth-sorted into the same painter's pass as the
     * sprites, so a character walks behind one trunk and in front of the
     * next tree down the row. */
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
    static float tdep[VOX_MAX_BILLBOARDS], tlat[VOX_MAX_BILLBOARDS],
                 tg[VOX_MAX_BILLBOARDS];
    static const VoxTree* tptr[VOX_MAX_BILLBOARDS];
    for (int i = 0; i < nc; i++) {
        const VoxTree* t = cand[i].t;
        float pxc = (float)cand[i].sx + 8.0f;
        float pyc = (float)cand[i].sy + 16.0f;
        float dep = (pxc - cx) * fxv + (pyc - cy) * fyv;
        if (dep < 16.0f || dep > FAR) continue;
        tdep[nt] = dep;
        tlat[nt] = (pxc - cx) * rxv + (pyc - cy) * ryv;
        float gh;
        if (i < live_trees) {
            gh = chase_height_at(grid, pxc, pyc);
        } else if (!vox_world_height(pxc, pyc, &gh)) {
            gh = 0.0f;
        }
        tg[nt] = gh < 0.0f ? 0.0f : gh;
        tptr[nt] = t;
        nt++;
    }
    for (int a = 1; a < nt; a++) {
        float d0 = tdep[a], l0 = tlat[a], g0 = tg[a];
        const VoxTree* p0 = tptr[a];
        int b = a - 1;
        while (b >= 0 && tdep[b] < d0) {
            tdep[b + 1] = tdep[b]; tlat[b + 1] = tlat[b];
            tg[b + 1] = tg[b];     tptr[b + 1] = tptr[b];
            b--;
        }
        tdep[b + 1] = d0; tlat[b + 1] = l0; tg[b + 1] = g0; tptr[b + 1] = p0;
    }

    int ri = 0, ti = 0;
    while (ri < nr || ti < nt) {
        if (ti < nt && (ri >= nr || tdep[ti] >= runs[ri].dep)) {
            /* One tree: a bark trunk and an elliptical canopy shaded in
             * the three tones sampled from its own art -- lit crown, mid
             * body, shadowed underside -- with a checker dither at the
             * band seams so it stays pixel-art and not vector clip-art. */
            const VoxTree* t = tptr[ti];
            float dep = tdep[ti];
            float sc = focal / dep;
            int cxp = (int)((float)OW * 0.5f + tlat[ti] / dep * focal);
            int byp = horizon + (int)((cz - tg[ti] * HPX) * focal / dep);
            int t2 = (int)((dep - g_tune.fog_start) * 256.0f /
                           (FAR - g_tune.fog_start));
            if (t2 < 0) t2 = 0;
            if (t2 > (int)g_tune.fog_max) t2 = (int)g_tune.fog_max;
            /* Proportions by kind: trees get a trunk and a tall canopy;
             * destructibles (bushes, cuttable grass) are trunkless tufts
             * hugging the ground -- a tuft with a trunk read as a small
             * tree, and nobody swings a sword at a tree. */
            bool big = t->hcls >= VOX_H_HIGH;
            int th2 = big ? (int)(9.0f * sc) : 0;
            int tw2 = (int)((big ? 6.0f : 5.0f) * sc);
            int chh = (int)((big ? 22.0f : 10.0f) * sc);
            int cww = (int)((big ? 24.0f : 16.0f) * sc);
            ti++;
            if (chh < 3 || cww < 3) continue;
            if (tw2 < 2) tw2 = 2;
            uint32_t bark = lerp_color(t->bark, fog, t2);
            uint32_t lit  = lerp_color(t->lit, fog, t2);
            uint32_t mid  = lerp_color(t->mid, fog, t2);
            uint32_t dk   = lerp_color(shade(t->dark, 200), fog, t2);
            for (int yy = byp - th2; yy < byp; yy++) {
                if (yy < 0 || yy >= OH) continue;
                for (int k2 = 0; k2 < tw2; k2++) {
                    int xx = cxp - tw2 / 2 + k2;
                    if (xx < 0 || xx >= OW) continue;
                    if (s_zbuf[yy * OW + xx] < dep - 3.0f) continue;
                    out[yy * OW + xx] = (k2 == 0 || k2 == tw2 - 1)
                        ? shade(bark, 150) : bark;
                }
            }
            /* The canopy is the tree's own 16x16 tile art stood upright,
             * masked to a rounded-square silhouette (superellipse) so the
             * block's corners -- neighbouring ground or the next tree's
             * leaves -- don't float. The art carries the shading; a dark
             * rim at the silhouette edge gives it body. */
            (void)lit; (void)mid; (void)dk;
            int cyc = byp - th2 - chh / 2;
            for (int yy = cyc - chh / 2; yy <= cyc + chh / 2; yy++) {
                if (yy < 0 || yy >= OH) continue;
                float v = (float)(yy - cyc) / ((float)chh * 0.5f);
                int ay = (int)((v * 0.5f + 0.5f) * 15.99f);
                for (int xx = cxp - cww / 2; xx <= cxp + cww / 2; xx++) {
                    if (xx < 0 || xx >= OW) continue;
                    float u = (float)(xx - cxp) / ((float)cww * 0.5f);
                    float r2 = u * u * u * u + v * v * v * v;
                    if (r2 > 1.0f) continue;
                    if (s_zbuf[yy * OW + xx] < dep - 3.0f) continue;
                    int ax = (int)((u * 0.5f + 0.5f) * 15.99f);
                    uint32_t c2 = t->tex[ay * 16 + ax];
                    if (r2 > 0.62f) c2 = shade(c2, 148);   /* dark rim */
                    out[yy * OW + xx] = lerp_color(c2, fog, t2);
                }
            }
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
     * so tall geometry has somewhere to go without clipping off the top. */
    const float headroom = VOX_UNITS[VOX_H_HIGH] * cam.lift * g_tune.tilt_scale;
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
            if (VOX_UNITS[grid->height[ty][tx]] != 0.0f) { extruded = true; break; }
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
    poll_toggle_key();
    if (g_mode == VOXEL_MODE_OFF || !ctx || !fb) return NULL;
    if (!vox_scrape(ctx, fb, &g_grid, &g_sprites)) return NULL;
    if (g_grid.flat) return NULL;
    vox_render(ctx, &g_grid, &g_sprites, fb, g_mode, g_scale, g_out);
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
void voxel_remap_dpad(void) {
    if (g_mode != VOXEL_MODE_CHASE) return;

    const uint8_t pressed = (uint8_t)(~g_joypad_dpad & 0x0F);
    if (!pressed) return;

    /* Screen-space direction the player asked for (y grows downward). */
    float dx = 0.0f, dy = 0.0f;
    if (pressed & 0x01) dx += 1.0f;   /* right */
    if (pressed & 0x02) dx -= 1.0f;   /* left  */
    if (pressed & 0x04) dy -= 1.0f;   /* up    */
    if (pressed & 0x08) dy += 1.0f;   /* down  */
    if (dx == 0.0f && dy == 0.0f) return;

    /* Rotate out of the camera's frame into the world. The camera's
     * forward is world "screen up", its right is world "screen right". */
    const float fx = cosf(g_chase_yaw), fy = sinf(g_chase_yaw);
    const float rx = -fy, ry = fx;
    const float wx = rx * dx + fx * dy;
    const float wy = ry * dx + fy * dy;

    /* Snap to the four directions the game actually accepts. Diagonals
     * survive because both axes can clear their threshold. */
    uint8_t out = 0;
    const float T = 0.38f;
    if (wx >  T) out |= 0x01;
    if (wx < -T) out |= 0x02;
    if (wy < -T) out |= 0x04;
    if (wy >  T) out |= 0x08;
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
