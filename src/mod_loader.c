/* Mod discovery + ROM patching. See mod_loader.h for the design rationale. */
#include "mod_loader.h"

#include "gb_asset_loader.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define GB_MKDIR(p) _mkdir(p)
#else
#define GB_MKDIR(p) mkdir((p), 0755)
#endif

#define LOG(...)                                \
    do {                                        \
        fprintf(stderr, "[MOD] ");              \
        fprintf(stderr, __VA_ARGS__);           \
        fprintf(stderr, "\n");                  \
    } while (0)

static GBModInfo g_mods[GB_MOD_MAX_ENTRIES];
static int       g_mod_count = 0;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static bool file_exists(const char* path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0;
}

static uint8_t* read_whole_file(const char* path, size_t* out_len) {
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t* buf = (uint8_t*)malloc((size_t)len ? (size_t)len : 1);
    if (!buf) { fclose(f); return NULL; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

static bool write_whole_file(const char* path, const uint8_t* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool copy_file(const char* src, const char* dst) {
    size_t len = 0;
    uint8_t* buf = read_whole_file(src, &len);
    if (!buf) return false;
    bool ok = write_whole_file(dst, buf, len);
    free(buf);
    return ok;
}

/* Case-insensitive extension test. */
static bool has_ext(const char* path, const char* ext) {
    size_t pl = strlen(path), el = strlen(ext);
    if (pl < el) return false;
    const char* tail = path + (pl - el);
    for (size_t i = 0; i < el; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i])) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* minimal JSON field reader                                           */
/*                                                                     */
/* Deliberately not a full parser: manifests are flat objects of        */
/* strings, numbers, bools and string arrays. Anything more exotic is   */
/* ignored rather than rejected, so a manifest written for a future     */
/* loader still loads here.                                            */
/* ------------------------------------------------------------------ */

/* Find the value text following "key" at the top level. Returns a pointer
 * into @p json just past the colon, or NULL. */
static const char* json_find(const char* json, const char* key) {
    size_t klen = strlen(key);
    for (const char* p = json; (p = strchr(p, '"')) != NULL; p++) {
        const char* start = p + 1;
        const char* end = strchr(start, '"');
        if (!end) return NULL;
        bool match = ((size_t)(end - start) == klen) &&
                     (strncmp(start, key, klen) == 0);
        p = end;
        if (!match) continue;
        const char* q = end + 1;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != ':') continue;
        q++;
        while (*q && isspace((unsigned char)*q)) q++;
        return q;
    }
    return NULL;
}

/* Copy a JSON string value into @p out. Handles \" and \\ escapes. */
static bool json_get_string(const char* json, const char* key, char* out, size_t out_sz) {
    const char* v = json_find(json, key);
    if (!v || *v != '"') return false;
    v++;
    size_t o = 0;
    while (*v && *v != '"') {
        char c = *v++;
        if (c == '\\' && *v) {
            char esc = *v++;
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default:  c = esc;  break;
            }
        }
        if (o + 1 < out_sz) out[o++] = c;
    }
    if (o < out_sz) out[o] = '\0'; else out[out_sz - 1] = '\0';
    return true;
}

static bool json_get_bool(const char* json, const char* key, bool defval) {
    const char* v = json_find(json, key);
    if (!v) return defval;
    if (strncmp(v, "true", 4) == 0) return true;
    if (strncmp(v, "false", 5) == 0) return false;
    return defval;
}

static int json_get_int(const char* json, const char* key, int defval) {
    const char* v = json_find(json, key);
    if (!v) return defval;
    if (*v != '-' && !isdigit((unsigned char)*v)) return defval;
    return (int)strtol(v, NULL, 10);
}

/* True if the array at @p key contains @p needle. If the key is absent,
 * returns @p absent_result — a manifest with no "games" list is treated as
 * applying to every cart. */
static bool json_array_contains(const char* json, const char* key,
                                const char* needle, bool absent_result) {
    const char* v = json_find(json, key);
    if (!v) return absent_result;
    if (*v != '[') return absent_result;
    const char* end = strchr(v, ']');
    if (!end) return absent_result;
    size_t nlen = strlen(needle);
    for (const char* p = v; p < end && (p = strchr(p, '"')) != NULL && p < end; p++) {
        const char* s = p + 1;
        const char* e = strchr(s, '"');
        if (!e || e > end) break;
        if ((size_t)(e - s) == nlen && strncmp(s, needle, nlen) == 0) return true;
        p = e;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* IPS                                                                 */
/* ------------------------------------------------------------------ */

/* Applies in place over a fixed-size ROM image. Records past the end of
 * the image are skipped with a warning rather than growing it: a GB cart
 * image is a fixed power-of-two and a longer one would not boot. */
static bool ips_apply(const uint8_t* patch, size_t plen, uint8_t* rom, size_t rom_len) {
    if (plen < 8 || memcmp(patch, "PATCH", 5) != 0) {
        LOG("not an IPS patch (bad magic)");
        return false;
    }
    size_t p = 5;
    unsigned applied = 0, skipped = 0;
    for (;;) {
        if (p + 3 > plen) { LOG("IPS truncated (no EOF marker)"); return false; }
        if (memcmp(patch + p, "EOF", 3) == 0) { p += 3; break; }
        uint32_t off = ((uint32_t)patch[p] << 16) | ((uint32_t)patch[p + 1] << 8) | patch[p + 2];
        p += 3;
        if (p + 2 > plen) { LOG("IPS truncated (record header)"); return false; }
        uint16_t size = (uint16_t)((patch[p] << 8) | patch[p + 1]);
        p += 2;

        if (size == 0) { /* RLE run */
            if (p + 3 > plen) { LOG("IPS truncated (RLE header)"); return false; }
            uint16_t rle = (uint16_t)((patch[p] << 8) | patch[p + 1]);
            uint8_t value = patch[p + 2];
            p += 3;
            if ((size_t)off + rle > rom_len) { skipped++; continue; }
            memset(rom + off, value, rle);
        } else {
            if (p + size > plen) { LOG("IPS truncated (record data)"); return false; }
            if ((size_t)off + size > rom_len) { skipped++; p += size; continue; }
            memcpy(rom + off, patch + p, size);
            p += size;
        }
        applied++;
    }
    if (skipped) LOG("IPS: %u records skipped (past end of %zu-byte ROM)", skipped, rom_len);
    LOG("IPS: %u records applied", applied);
    return true;
}

/* ------------------------------------------------------------------ */
/* BPS                                                                 */
/* ------------------------------------------------------------------ */

static uint32_t crc32_buf(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* BPS variable-length number. Advances *pos; sets *ok false on overrun. */
static uint64_t bps_varint(const uint8_t* p, size_t len, size_t* pos, bool* ok) {
    uint64_t data = 0, shift = 1;
    for (;;) {
        if (*pos >= len) { *ok = false; return 0; }
        uint8_t x = p[(*pos)++];
        data += (uint64_t)(x & 0x7F) * shift;
        if (x & 0x80) break;
        shift <<= 7;
        data += shift;
    }
    return data;
}

/* Decodes into a freshly allocated target buffer. The caller enforces the
 * cart's fixed image size afterwards. */
static uint8_t* bps_apply(const uint8_t* patch, size_t plen,
                          const uint8_t* src, size_t src_len,
                          size_t* out_len) {
    *out_len = 0;
    if (plen < 4 + 12 || memcmp(patch, "BPS1", 4) != 0) {
        LOG("not a BPS patch (bad magic)");
        return NULL;
    }
    /* Trailing 12 bytes are source/target/patch CRC32s. */
    size_t body_end = plen - 12;
    uint32_t want_src_crc = rd32le(patch + body_end);
    uint32_t want_tgt_crc = rd32le(patch + body_end + 4);
    uint32_t want_patch_crc = rd32le(patch + body_end + 8);

    uint32_t got_patch_crc = crc32_buf(patch, body_end + 8);
    if (got_patch_crc != want_patch_crc) {
        LOG("BPS patch is corrupt (patch CRC %08x, expected %08x)", got_patch_crc, want_patch_crc);
        return NULL;
    }

    size_t pos = 4;
    bool ok = true;
    uint64_t decl_src = bps_varint(patch, body_end, &pos, &ok);
    uint64_t decl_tgt = bps_varint(patch, body_end, &pos, &ok);
    uint64_t meta_len = bps_varint(patch, body_end, &pos, &ok);
    if (!ok || pos + meta_len > body_end) { LOG("BPS header truncated"); return NULL; }
    pos += (size_t)meta_len;

    if (decl_src != src_len) {
        LOG("BPS expects a %llu-byte source, this ROM is %zu bytes",
            (unsigned long long)decl_src, src_len);
        return NULL;
    }
    uint32_t got_src_crc = crc32_buf(src, src_len);
    if (got_src_crc != want_src_crc) {
        LOG("BPS base ROM mismatch (source CRC %08x, patch expects %08x) — "
            "this patch was built for a different revision", got_src_crc, want_src_crc);
        return NULL;
    }

    uint8_t* tgt = (uint8_t*)calloc((size_t)decl_tgt ? (size_t)decl_tgt : 1, 1);
    if (!tgt) return NULL;

    size_t out = 0;
    int64_t src_rel = 0, tgt_rel = 0;
    while (pos < body_end) {
        uint64_t data = bps_varint(patch, body_end, &pos, &ok);
        if (!ok) { LOG("BPS action stream truncated"); free(tgt); return NULL; }
        uint64_t cmd = data & 3u;
        uint64_t length = (data >> 2) + 1u;
        if (out + length > decl_tgt) { LOG("BPS overruns target size"); free(tgt); return NULL; }

        switch (cmd) {
            case 0: /* SourceRead */
                if (out + length > src_len) { LOG("BPS SourceRead past source"); free(tgt); return NULL; }
                memcpy(tgt + out, src + out, (size_t)length);
                out += length;
                break;
            case 1: /* TargetRead */
                if (pos + length > body_end) { LOG("BPS TargetRead past patch"); free(tgt); return NULL; }
                memcpy(tgt + out, patch + pos, (size_t)length);
                pos += (size_t)length;
                out += length;
                break;
            case 2: { /* SourceCopy */
                uint64_t raw = bps_varint(patch, body_end, &pos, &ok);
                if (!ok) { free(tgt); return NULL; }
                int64_t delta = (int64_t)(raw >> 1);
                if (raw & 1) delta = -delta;
                src_rel += delta;
                if (src_rel < 0 || (uint64_t)src_rel + length > src_len) {
                    LOG("BPS SourceCopy out of range"); free(tgt); return NULL;
                }
                for (uint64_t i = 0; i < length; i++) tgt[out++] = src[src_rel++];
                break;
            }
            case 3: { /* TargetCopy */
                uint64_t raw = bps_varint(patch, body_end, &pos, &ok);
                if (!ok) { free(tgt); return NULL; }
                int64_t delta = (int64_t)(raw >> 1);
                if (raw & 1) delta = -delta;
                tgt_rel += delta;
                if (tgt_rel < 0 || (uint64_t)tgt_rel >= decl_tgt) {
                    LOG("BPS TargetCopy out of range"); free(tgt); return NULL;
                }
                /* Byte-at-a-time on purpose: TargetCopy is allowed to read
                 * bytes this same action is still writing (that is how BPS
                 * encodes runs), so memcpy would be wrong here. */
                for (uint64_t i = 0; i < length; i++) tgt[out++] = tgt[tgt_rel++];
                break;
            }
            default: break;
        }
    }

    if (out != decl_tgt) {
        LOG("BPS produced %zu bytes, header declared %llu", out, (unsigned long long)decl_tgt);
        free(tgt);
        return NULL;
    }
    uint32_t got_tgt_crc = crc32_buf(tgt, out);
    if (got_tgt_crc != want_tgt_crc) {
        LOG("BPS output CRC %08x != expected %08x — patch did not apply cleanly",
            got_tgt_crc, want_tgt_crc);
        free(tgt);
        return NULL;
    }
    LOG("BPS: applied cleanly (%zu bytes, CRC %08x)", out, got_tgt_crc);
    *out_len = out;
    return tgt;
}

/* ------------------------------------------------------------------ */
/* overlays                                                            */
/* ------------------------------------------------------------------ */

/* Raw byte splices: each file in the overlay dir is named for the hex ROM
 * offset it lands at, e.g. `overlay/0x3f200.bin` or `overlay/3f200.bin`.
 * Useful for hand-edited graphics without generating a whole patch. */
static int overlay_apply(const char* dir, uint8_t* rom, size_t rom_len) {
    DIR* d = opendir(dir);
    if (!d) return 0;
    int applied = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!has_ext(ent->d_name, ".bin")) continue;

        const char* nm = ent->d_name;
        if (nm[0] == '0' && (nm[1] == 'x' || nm[1] == 'X')) nm += 2;
        char* endp = NULL;
        unsigned long off = strtoul(nm, &endp, 16);
        if (endp == nm) {
            LOG("overlay: '%s' is not a hex offset, skipping", ent->d_name);
            continue;
        }

        char path[GB_MOD_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        size_t len = 0;
        uint8_t* buf = read_whole_file(path, &len);
        if (!buf) continue;
        if (off + len > rom_len) {
            LOG("overlay: '%s' (0x%lx +%zu) runs past the ROM, skipping", ent->d_name, off, len);
            free(buf);
            continue;
        }
        memcpy(rom + off, buf, len);
        free(buf);
        LOG("overlay: %s -> 0x%06lx (%zu bytes)", ent->d_name, off, len);
        applied++;
    }
    closedir(d);
    return applied;
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */
/* ------------------------------------------------------------------ */

/* mods/state.json is the launcher's toggle store: a flat map of mod id ->
 * bool. It overrides the manifest's own "enabled" default so the launcher
 * never has to rewrite files the user owns. Missing file = use manifests. */
static char* read_mod_state(void) {
    size_t len = 0;
    uint8_t* raw = read_whole_file("mods/state.json", &len);
    if (!raw) return NULL;
    char* json = (char*)realloc(raw, len + 1);
    if (!json) { free(raw); return NULL; }
    json[len] = '\0';
    return json;
}

static int mod_priority_cmp(const void* a, const void* b) {
    const GBModInfo* x = (const GBModInfo*)a;
    const GBModInfo* y = (const GBModInfo*)b;
    if (x->priority != y->priority) return x->priority - y->priority;
    return strcmp(x->id, y->id);
}

int gb_mods_scan(const char* game_id) {
    g_mod_count = 0;
    if (!game_id || !*game_id) return 0;

    DIR* d = opendir("mods");
    if (!d) return -1;

    char* state = read_mod_state();
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && g_mod_count < GB_MOD_MAX_ENTRIES) {
        if (ent->d_name[0] == '.') continue;

        char dir[GB_MOD_PATH_MAX];
        char manifest_path[GB_MOD_PATH_MAX];
        int n = snprintf(dir, sizeof(dir), "mods/%s", ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(dir)) continue;

        struct stat st;
        if (stat(dir, &st) != 0 || !(st.st_mode & S_IFDIR)) continue;

        n = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", dir);
        if (n < 0 || (size_t)n >= sizeof(manifest_path)) continue;

        size_t jlen = 0;
        uint8_t* jraw = read_whole_file(manifest_path, &jlen);
        if (!jraw) continue;
        char* json = (char*)realloc(jraw, jlen + 1);
        if (!json) { free(jraw); continue; }
        json[jlen] = '\0';

        /* A manifest with no "games" key applies to every cart. */
        if (!json_array_contains(json, "games", game_id, true)) {
            free(json);
            continue;
        }

        GBModInfo* m = &g_mods[g_mod_count];
        memset(m, 0, sizeof(*m));
        snprintf(m->dir, sizeof(m->dir), "%s", dir);
        if (!json_get_string(json, "id", m->id, sizeof(m->id))) {
            snprintf(m->id, sizeof(m->id), "%s", ent->d_name);
        }
        if (!json_get_string(json, "name", m->name, sizeof(m->name))) {
            snprintf(m->name, sizeof(m->name), "%s", m->id);
        }
        if (!json_get_string(json, "version", m->version, sizeof(m->version))) {
            snprintf(m->version, sizeof(m->version), "0.0.0");
        }
        m->priority = json_get_int(json, "priority", 100);
        m->enabled  = json_get_bool(json, "enabled", true);
        m->applies  = true;
        /* The launcher's toggle wins over the manifest default when it has
         * an opinion about this mod. */
        if (state) m->enabled = json_get_bool(state, m->id, m->enabled);

        char rel[GB_MOD_PATH_MAX];
        if (json_get_string(json, "patch", rel, sizeof(rel)) && rel[0]) {
            snprintf(m->patch_file, sizeof(m->patch_file), "%s/%s", dir, rel);
            if (has_ext(rel, ".bps")) {
                m->patch_kind = GB_MOD_PATCH_BPS;
            } else if (has_ext(rel, ".ips")) {
                m->patch_kind = GB_MOD_PATCH_IPS;
            } else {
                LOG("%s: patch '%s' is neither .ips nor .bps, ignoring", m->id, rel);
                m->patch_file[0] = '\0';
            }
            if (m->patch_file[0] && !file_exists(m->patch_file)) {
                LOG("%s: patch file '%s' is missing", m->id, m->patch_file);
                m->patch_file[0] = '\0';
                m->patch_kind = GB_MOD_PATCH_NONE;
            }
        }
        if (json_get_string(json, "overlay", rel, sizeof(rel)) && rel[0]) {
            snprintf(m->overlay_dir, sizeof(m->overlay_dir), "%s/%s", dir, rel);
            if (!file_exists(m->overlay_dir)) m->overlay_dir[0] = '\0';
        }

        free(json);
        g_mod_count++;
    }
    closedir(d);
    free(state);

    qsort(g_mods, (size_t)g_mod_count, sizeof(g_mods[0]), mod_priority_cmp);
    return g_mod_count;
}

int gb_mods_count(void) { return g_mod_count; }

const GBModInfo* gb_mods_get(int index) {
    if (index < 0 || index >= g_mod_count) return NULL;
    return &g_mods[index];
}

void gb_mods_set_enabled(int index, bool enabled) {
    if (index < 0 || index >= g_mod_count) return;
    g_mods[index].enabled = enabled;
}

/* ------------------------------------------------------------------ */
/* staging + applying                                                  */
/* ------------------------------------------------------------------ */

static void rom_paths(const char* game_id, char* live, size_t live_sz,
                      char* orig, size_t orig_sz) {
    snprintf(live, live_sz, "assets/%s/rom.bin", game_id);
    snprintf(orig, orig_sz, "assets/%s/rom.bin.orig", game_id);
}

bool gb_mods_stage_assets(const char* game_id,
                          const char* rom_path,
                          const uint8_t expected_sha1[20],
                          uint32_t rom_size,
                          const void* manifest,
                          uint32_t manifest_count) {
    char live[GB_MOD_PATH_MAX], orig[GB_MOD_PATH_MAX];
    rom_paths(game_id, live, sizeof(live), orig, sizeof(orig));

    /* Already staged, and a snapshot exists to patch from. */
    if (file_exists(orig)) return true;

    /* Not staged yet: run the runtime's own extractor into a scratch
     * buffer so assets/<id>/ is written exactly as the cart would write
     * it, SHA-1 check and all. */
    if (!file_exists(live)) {
        uint8_t* scratch = (uint8_t*)calloc(rom_size, 1);
        if (!scratch) return false;

        GBGameAssets staging;
        memset(&staging, 0, sizeof(staging));
        staging.game_id = game_id;
        staging.rom_filename = rom_path;
        staging.rom_data = scratch;
        staging.rom_size = rom_size;
        memcpy((void*)staging.expected_sha1, expected_sha1, 20);
        staging.manifest = (const GBAssetEntry*)manifest;
        staging.manifest_count = manifest_count;

        bool ok = gb_load_assets(&staging);
        free(scratch);
        if (!ok) {
            LOG("%s: could not stage assets from '%s'", game_id, rom_path);
            return false;
        }
    }

    if (!file_exists(live)) {
        LOG("%s: expected %s after staging but it is missing", game_id, live);
        return false;
    }
    if (!copy_file(live, orig)) {
        LOG("%s: could not write pristine snapshot %s", game_id, orig);
        return false;
    }
    LOG("%s: pristine ROM snapshot saved", game_id);
    return true;
}

bool gb_mods_restore_stock(const char* game_id) {
    char live[GB_MOD_PATH_MAX], orig[GB_MOD_PATH_MAX];
    rom_paths(game_id, live, sizeof(live), orig, sizeof(orig));
    if (!file_exists(orig)) return false;
    return copy_file(orig, live);
}

bool gb_mods_apply(const char* game_id, uint32_t rom_size) {
    char live[GB_MOD_PATH_MAX], orig[GB_MOD_PATH_MAX];
    rom_paths(game_id, live, sizeof(live), orig, sizeof(orig));

    if (!file_exists(orig)) {
        LOG("%s: no pristine snapshot; stage assets first", game_id);
        return false;
    }

    /* Always rebuild from the snapshot so disabling a mod actually undoes
     * it, and so two runs with the same mod set are byte-identical. */
    size_t rom_len = 0;
    uint8_t* rom = read_whole_file(orig, &rom_len);
    if (!rom) return false;
    if (rom_len != rom_size) {
        LOG("%s: snapshot is %zu bytes, expected %u", game_id, rom_len, rom_size);
        free(rom);
        return false;
    }

    int enabled = 0, failed = 0;
    for (int i = 0; i < g_mod_count; i++) {
        GBModInfo* m = &g_mods[i];
        if (!m->enabled) continue;
        enabled++;
        bool mod_ok = true;

        if (m->patch_kind == GB_MOD_PATCH_IPS) {
            size_t plen = 0;
            uint8_t* p = read_whole_file(m->patch_file, &plen);
            if (!p) { LOG("%s: cannot read %s", m->id, m->patch_file); failed++; continue; }
            if (!ips_apply(p, plen, rom, rom_len)) {
                LOG("%s: IPS failed", m->id);
                failed++;
                mod_ok = false;
            }
            free(p);
        } else if (m->patch_kind == GB_MOD_PATCH_BPS) {
            size_t plen = 0;
            uint8_t* p = read_whole_file(m->patch_file, &plen);
            if (!p) { LOG("%s: cannot read %s", m->id, m->patch_file); failed++; continue; }
            size_t tlen = 0;
            uint8_t* out = bps_apply(p, plen, rom, rom_len, &tlen);
            free(p);
            if (!out) {
                LOG("%s: BPS failed", m->id);
                failed++;
                mod_ok = false;
            } else if (tlen != rom_len) {
                /* A BPS that resizes the cart cannot boot on a statically
                 * recompiled binary: the generated code is bound to the
                 * original bank layout. */
                LOG("%s: BPS output is %zu bytes but this cart is fixed at %zu - skipping",
                    m->id, tlen, rom_len);
                free(out);
                failed++;
                mod_ok = false;
            } else {
                memcpy(rom, out, rom_len);
                free(out);
            }
        }

        if (m->overlay_dir[0]) {
            int n = overlay_apply(m->overlay_dir, rom, rom_len);
            if (n > 0) LOG("%s: %d overlay file(s) applied", m->id, n);
        }
        if (mod_ok) {
            LOG("applied '%s' %s", m->name, m->version);
        } else {
            LOG("skipped '%s' %s - its patch did not apply", m->name, m->version);
        }
    }

    bool ok = write_whole_file(live, rom, rom_len);
    free(rom);
    if (!ok) {
        LOG("%s: could not write %s", game_id, live);
        return false;
    }

    if (enabled == 0) {
        LOG("%s: no mods enabled — stock ROM restored", game_id);
    } else {
        LOG("%s: %d mod(s) enabled, %d failed", game_id, enabled, failed);
    }
    return failed == 0;
}
