/* Achievements. See epoch_achievements.h for the shape of the thing.
 *
 * Packs are text files under achievements/ named for the cart id:
 * tlozooa.txt itself plus any tlozooa.*.txt beside it, so a mod's pack is
 * one dropped file away. Every address in the shipped packs comes from
 * the oracles disassembly (include/wram.s); the format is documented in
 * achievements/README.md.
 *
 * Unlocks are remembered per cart in states/achievements-<cart>.txt --
 * one id per line, append-only, human-readable. Deleting the file locks
 * everything again, which is also the point: it is a save file you can
 * see.
 */
#include "epoch_achievements.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define ea_mkdir(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#define ea_mkdir(p) mkdir(p, 0755)
#endif

#define LOG(...) \
    do { fprintf(stderr, "[achievements] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ------------------------------------------------------------------ */
/* conditions                                                          */
/* ------------------------------------------------------------------ */

static uint32_t popcount8(uint8_t v) {
    uint32_t n = 0;
    while (v) { n += v & 1u; v >>= 1; }
    return n;
}

static uint32_t cond_read(const EaCond* c, const uint8_t* wram) {
    const uint16_t off = (uint16_t)(c->addr - 0xC000);
    switch (c->kind) {
    case EA_BYTE:   return wram[off];
    case EA_WORD:   return (uint32_t)wram[off] | ((uint32_t)wram[off + 1] << 8);
    case EA_BCD: {
        /* Two bytes of packed BCD, low byte first: 0x23 0x01 = 123. */
        uint32_t lo = wram[off], hi = wram[off + 1];
        return (hi >> 4) * 1000 + (hi & 0xF) * 100 + (lo >> 4) * 10 + (lo & 0xF);
    }
    case EA_BITS:   return popcount8(wram[off]);
    case EA_BITSET: {
        uint32_t n = 0;
        for (uint8_t i = 0; i < c->len; i++) n += popcount8(wram[off + i]);
        return n;
    }
    case EA_FLAG:   return (wram[off] >> c->len) & 1u;
    }
    return 0;
}

bool ea_cond_holds(const EaCond* c, const uint8_t* wram) {
    const uint32_t got = cond_read(c, wram);
    switch (c->op) {
    case EA_EQ: return got == c->value;
    case EA_NE: return got != c->value;
    case EA_GE: return got >= c->value;
    case EA_LE: return got <= c->value;
    case EA_GT: return got >  c->value;
    case EA_LT: return got <  c->value;
    }
    return false;
}

int ea_evaluate(EaSet* set, const uint8_t* wram, int* newly, int cap) {
    if (set->gate_addr  && wram[set->gate_addr  - 0xC000] == 0) return 0;
    if (set->gate_addr2 && wram[set->gate_addr2 - 0xC000] == 0) return 0;
    int n = 0;
    for (int i = 0; i < set->count; i++) {
        EaAchievement* a = &set->list[i];
        if (a->unlocked || a->n_conds == 0) continue;
        bool all = true;
        for (int k = 0; k < a->n_conds && all; k++)
            all = ea_cond_holds(&a->conds[k], wram);
        if (all) {
            a->unlocked = true;
            if (n < cap) newly[n] = i;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* pack parsing                                                        */
/* ------------------------------------------------------------------ */

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    return s;
}

static bool parse_op(const char* tok, EaOp* op) {
    if (!strcmp(tok, "=="))  { *op = EA_EQ; return true; }
    if (!strcmp(tok, "!="))  { *op = EA_NE; return true; }
    if (!strcmp(tok, ">="))  { *op = EA_GE; return true; }
    if (!strcmp(tok, "<="))  { *op = EA_LE; return true; }
    if (!strcmp(tok, ">"))   { *op = EA_GT; return true; }
    if (!strcmp(tok, "<"))   { *op = EA_LT; return true; }
    return false;
}

static bool parse_addr(const char* tok, uint16_t* addr) {
    unsigned v;
    if (sscanf(tok, "%x", &v) != 1) return false;
    if (v < 0xC000 || v > 0xDFFF) return false;   /* WRAM only */
    *addr = (uint16_t)v;
    return true;
}

/* "byte c6aa >= 40" / "bitset c69a 16 >= 12" / "flag c69a 5" */
static bool parse_cond(char* text, EaCond* c) {
    char kind[16], a1[16], a2[16], a3[16], a4[16];
    int n = sscanf(text, "%15s %15s %15s %15s %15s", kind, a1, a2, a3, a4);

    if (!strcmp(kind, "flag") && n == 3) {
        unsigned bit;
        if (!parse_addr(a1, &c->addr)) return false;
        if (sscanf(a2, "%u", &bit) != 1 || bit > 7) return false;
        c->kind = EA_FLAG; c->len = (uint8_t)bit;
        c->op = EA_EQ; c->value = 1;
        return true;
    }
    if (!strcmp(kind, "bitset") && n == 5) {
        unsigned len;
        if (!parse_addr(a1, &c->addr)) return false;
        if (sscanf(a2, "%u", &len) != 1 || len == 0 || len > 32) return false;
        if (!parse_op(a3, &c->op)) return false;
        if (sscanf(a4, "%u", &c->value) != 1) return false;
        c->kind = EA_BITSET; c->len = (uint8_t)len;
        return true;
    }
    if (n == 4) {
        if (!strcmp(kind, "byte"))      c->kind = EA_BYTE;
        else if (!strcmp(kind, "word")) c->kind = EA_WORD;
        else if (!strcmp(kind, "bcd"))  c->kind = EA_BCD;
        else if (!strcmp(kind, "bits")) c->kind = EA_BITS;
        else return false;
        if (!parse_addr(a1, &c->addr)) return false;
        if (!parse_op(a2, &c->op)) return false;
        if (sscanf(a3, "%u", &c->value) != 1) return false;
        c->len = 0;
        return true;
    }
    return false;
}

int ea_load_pack(EaSet* set, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    int added = 0;
    EaAchievement* cur = NULL;
    bool cur_bad = false;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        char* s = trim(line);
        if (!*s || *s == '#') continue;

        if (*s == '[') {
            char* close = strchr(s, ']');
            if (!close || close == s + 1) { cur = NULL; continue; }
            *close = 0;
            if (set->count >= EA_MAX_ACHIEVEMENTS) {
                LOG("%s: over %d achievements, ignoring the rest", path,
                    EA_MAX_ACHIEVEMENTS);
                break;
            }
            cur = &set->list[set->count++];
            memset(cur, 0, sizeof(*cur));
            snprintf(cur->id, sizeof(cur->id), "%s", s + 1);
            cur_bad = false;
            added++;
            continue;
        }
        if (!cur) continue;

        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = trim(s);
        char* val = trim(eq + 1);

        if (!strcmp(key, "title")) {
            snprintf(cur->title, sizeof(cur->title), "%s", val);
        } else if (!strcmp(key, "desc")) {
            snprintf(cur->desc, sizeof(cur->desc), "%s", val);
        } else if (!strcmp(key, "when")) {
            if (cur_bad) continue;
            if (cur->n_conds >= EA_MAX_CONDS) {
                LOG("%s: [%s] has over %d conditions; dropping it",
                    path, cur->id, EA_MAX_CONDS);
                cur_bad = true;
            } else if (!parse_cond(val, &cur->conds[cur->n_conds])) {
                /* Half an achievement would unlock on the wrong thing;
                 * a broken condition takes the whole entry with it. */
                LOG("%s: [%s] bad condition '%s'; dropping it",
                    path, cur->id, val);
                cur_bad = true;
            } else {
                cur->n_conds++;
            }
        } else {
            LOG("%s: [%s] unknown key '%s', skipping", path, cur->id, key);
        }
        if (cur_bad) cur->n_conds = 0;
    }
    fclose(f);
    return added;
}

/* ------------------------------------------------------------------ */
/* persistence                                                         */
/* ------------------------------------------------------------------ */

void ea_load_unlocked(EaSet* set, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char* id = trim(line);
        if (!*id) continue;
        for (int i = 0; i < set->count; i++)
            if (!strcmp(set->list[i].id, id)) set->list[i].unlocked = true;
    }
    fclose(f);
}

void ea_save_unlock(const char* path, const char* id) {
    FILE* f = fopen(path, "a");
    if (!f) { LOG("cannot write %s", path); return; }
    fprintf(f, "%s\n", id);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* the live set (loaded per cart by the runner glue below)             */
/* ------------------------------------------------------------------ */

static EaSet  g_set;
static char   g_state_path[128];
static char   g_cart[16];
static bool   g_ready = false;

/* ------------------------------------------------------------------ */
/* icons                                                               */
/* ------------------------------------------------------------------ */

bool ea_load_ppm(const char* path, EaIcon* out) {
    out->w = out->h = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char magic[3] = {0};
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) goto bad;
    {
        /* Header fields, with the #-comments editors like to leave. */
        int* fields[3] = {&w, &h, &maxv};
        for (int i = 0; i < 3; i++) {
            int ch;
            do {
                ch = fgetc(f);
                if (ch == '#') {
                    while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
                }
            } while (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
            if (ch == EOF) goto bad;
            ungetc(ch, f);
            if (fscanf(f, "%d", fields[i]) != 1) goto bad;
        }
    }
    if (fgetc(f) == EOF) goto bad;   /* the single whitespace after maxval */
    if (w < 1 || h < 1 || w > EA_ICON_DIM || h > EA_ICON_DIM || maxv != 255) {
        LOG("%s: want a P6 up to %dx%d with maxval 255, got %dx%d/%d",
            path, EA_ICON_DIM, EA_ICON_DIM, w, h, maxv);
        goto bad;
    }
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3];
        if (fread(rgb, 1, 3, f) != 3) goto bad;
        /* Magenta is the transparent key, same as the mod billboard art. */
        uint32_t a = (rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 255) ? 0 : 0xFF;
        out->px[i] = (a << 24) | ((uint32_t)rgb[0] << 16) |
                     ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
    }
    fclose(f);
    out->w = w;
    out->h = h;
    return true;
bad:
    fclose(f);
    out->w = out->h = 0;
    return false;
}

bool ea_load_pam(const char* path, EaIcon* out) {
    out->w = out->h = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char line[128];
    int w = 0, h = 0, depth = 0, maxv = 0;
    bool endhdr = false;
    if (!fgets(line, sizeof(line), f) || strcmp(line, "P7\n") != 0) goto bad;
    while (fgets(line, sizeof(line), f)) {
        if (strcmp(line, "ENDHDR\n") == 0 || strcmp(line, "ENDHDR\r\n") == 0) {
            endhdr = true;
            break;
        }
        if (sscanf(line, "WIDTH %d", &w) == 1) continue;
        if (sscanf(line, "HEIGHT %d", &h) == 1) continue;
        if (sscanf(line, "DEPTH %d", &depth) == 1) continue;
        if (sscanf(line, "MAXVAL %d", &maxv) == 1) continue;
    }
    if (!endhdr || w < 1 || h < 1 || w > EA_ICON_DIM || h > EA_ICON_DIM ||
        depth != 4 || maxv != 255) {
        LOG("%s: want a PAM RGBA up to %dx%d with maxval 255, got %dx%d/%d",
            path, EA_ICON_DIM, EA_ICON_DIM, w, h, maxv);
        goto bad;
    }
    for (int i = 0; i < w * h; i++) {
        uint8_t rgba[4];
        if (fread(rgba, 1, 4, f) != 4) goto bad;
        out->px[i] = ((uint32_t)rgba[3] << 24) |
                     ((uint32_t)rgba[0] << 16) |
                     ((uint32_t)rgba[1] << 8) | (uint32_t)rgba[2];
    }
    fclose(f);
    out->w = w;
    out->h = h;
    return true;
bad:
    fclose(f);
    out->w = out->h = 0;
    return false;
}

/* One cached icon per achievement, loaded on first ask. `tried` keeps a
 * missing file from being stat'ed every frame the toast is up. */
static EaIcon g_icons[EA_MAX_ACHIEVEMENTS];
static uint8_t g_icon_tried[EA_MAX_ACHIEVEMENTS];

const EaIcon* ea_icon_get(const char* id) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < g_set.count; i++) {
        if (strcmp(g_set.list[i].id, id) != 0) continue;
        if (!g_icon_tried[i]) {
            g_icon_tried[i] = 1;
            char path[256];
            snprintf(path, sizeof(path), "achievements/icons/%s/%s.pam",
                     g_cart, id);
            if (!ea_load_pam(path, &g_icons[i])) {
                /* Mods made for the original icon format still work. */
                snprintf(path, sizeof(path), "achievements/icons/%s/%s.ppm",
                         g_cart, id);
                ea_load_ppm(path, &g_icons[i]);
            }
        }
        return g_icons[i].w > 0 ? &g_icons[i] : NULL;
    }
    return NULL;
}

const EaSet* epoch_achievements_set(void)  { return &g_set; }
const char*  epoch_achievements_cart(void) { return g_cart; }

/* ------------------------------------------------------------------ */
/* toast queue                                                         */
/* ------------------------------------------------------------------ */

#define TOAST_QUEUE 8

static EaToast g_toasts[TOAST_QUEUE];
static int g_toast_head = 0, g_toast_count = 0;

void ea_toast_push(const char* id, const char* title, const char* desc) {
    if (g_toast_count >= TOAST_QUEUE) return;   /* queue full: drop, shrug */
    EaToast* t = &g_toasts[(g_toast_head + g_toast_count) % TOAST_QUEUE];
    snprintf(t->id, sizeof(t->id), "%s", id ? id : "");
    snprintf(t->title, sizeof(t->title), "%s", title);
    snprintf(t->desc, sizeof(t->desc), "%s", desc);
    t->age = 0.0f;
    g_toast_count++;
}

const EaToast* ea_toast_current(void) {
    return g_toast_count > 0 ? &g_toasts[g_toast_head] : NULL;
}

/* Slide in, sit, slide out; the drawer turns age into position. */
#define TOAST_LIFETIME 5.2f

void ea_toast_advance(float dt) {
    if (g_toast_count == 0) return;
    EaToast* t = &g_toasts[g_toast_head];
    t->age += dt;
    if (t->age >= TOAST_LIFETIME) {
        g_toast_head = (g_toast_head + 1) % TOAST_QUEUE;
        g_toast_count--;
    }
}

/* ------------------------------------------------------------------ */
/* runner glue                                                         */
/* ------------------------------------------------------------------ */

static void load_for_cart(const char* game_id) {
    memset(&g_set, 0, sizeof(g_set));
    memset(g_icon_tried, 0, sizeof(g_icon_tried));   /* new cart, new icons */
    snprintf(g_cart, sizeof(g_cart), "%s", game_id);
    snprintf(g_state_path, sizeof(g_state_path),
             "states/achievements-%s.txt", game_id);

    /* wLinkMaxHealth: zero until a file's data is in WRAM -- but the
     * file select loads it for the preview card too, so wScrollMode
     * (zero outside an actual room, same address both carts) closes
     * the file-browsing hole. Both must be nonzero. */
    const bool ages = (strcmp(game_id, "tlozooa") == 0);
    g_set.gate_addr  = ages ? 0xC6AB : 0xC6A3;
    g_set.gate_addr2 = 0xCD00;   /* wScrollMode */

    char path[160];
    snprintf(path, sizeof(path), "achievements/%s.txt", game_id);
    int base = ea_load_pack(&g_set, path);
    if (base < 0) {
        LOG("no pack at %s; achievements off for this cart", path);
        return;
    }

    /* Extra packs: achievements/<cart>.<anything>.txt, so a mod (or a
     * player) adds achievements by dropping one file in. */
#ifdef _WIN32
    {
        struct _finddata_t fd;
        char pattern[160];
        snprintf(pattern, sizeof(pattern), "achievements/%s.*.txt", game_id);
        intptr_t h = _findfirst(pattern, &fd);
        if (h != -1) {
            do {
                char sub[288];
                snprintf(sub, sizeof(sub), "achievements/%s", fd.name);
                int n = ea_load_pack(&g_set, sub);
                if (n > 0) LOG("pack %s: %d", sub, n);
            } while (_findnext(h, &fd) == 0);
            _findclose(h);
        }
    }
#else
    {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s.", game_id);
        const size_t plen = strlen(prefix);
        DIR* d = opendir("achievements");
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                const size_t nlen = strlen(e->d_name);
                if (nlen <= plen + 4) continue;
                if (strncmp(e->d_name, prefix, plen) != 0) continue;
                if (strcmp(e->d_name + nlen - 4, ".txt") != 0) continue;
                char sub[288];
                snprintf(sub, sizeof(sub), "achievements/%s", e->d_name);
                int n = ea_load_pack(&g_set, sub);
                if (n > 0) LOG("pack %s: %d", sub, n);
            }
            closedir(d);
        }
    }
#endif

    ea_mkdir("states");
    ea_load_unlocked(&g_set, g_state_path);

    int unlocked = 0;
    for (int i = 0; i < g_set.count; i++) unlocked += g_set.list[i].unlocked;
    LOG("%s: %d achievements, %d already unlocked", game_id, g_set.count, unlocked);
}

void epoch_achievements_tick(GBContext* ctx, const char* game_id) {
    if (!ctx || !ctx->wram || !game_id) return;
    if (!g_ready || strcmp(g_cart, game_id) != 0) {
        load_for_cart(game_id);
        g_ready = true;
        /* EPOCH_TOAST_TEST=1: pop a sample toast immediately, so pack
         * authors (and screenshots) can see the card without earning
         * anything. */
        if (getenv("EPOCH_TOAST_TEST"))
            ea_toast_push("all-essences", "Master of Ages",
                          "Hold all eight Essences of Time");
    }
    if (g_set.count == 0) return;

    int newly[8];
    int n = ea_evaluate(&g_set, ctx->wram, newly, 8);
    for (int i = 0; i < n && i < 8; i++) {
        const EaAchievement* a = &g_set.list[newly[i]];
        ea_save_unlock(g_state_path, a->id);
        ea_toast_push(a->id, a->title[0] ? a->title : a->id, a->desc);
        LOG("unlocked: %s", a->id);
    }
}

void epoch_achievements_progress(int* unlocked, int* total) {
    int u = 0;
    for (int i = 0; i < g_set.count; i++) u += g_set.list[i].unlocked;
    if (unlocked) *unlocked = u;
    if (total)    *total = g_set.count;
}
