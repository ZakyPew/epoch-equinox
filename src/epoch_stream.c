/* See epoch_stream.h.
 *
 * Every address here is the same one the achievement packs use, taken
 * from the oracles disassembly's include/wram.s. The two carts keep the
 * save block at slightly different offsets, which is why the table has
 * two columns: comments in the disassembly read "$c6aa/$c6a2", Ages
 * first, Seasons second.
 */
#include "epoch_stream.h"

#include "epoch_achievements.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define es_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define es_mkdir(p) mkdir(p, 0755)
#endif

/* Per-cart WRAM addresses. Shared ones sit outside the struct. */
typedef struct {
    const char* id;
    const char* title;
    uint16_t link_health, link_max_health;
    uint16_t rupees;          /* wNumRupees, 2 bytes */
    uint16_t essences;        /* wEssencesObtained, a bitmask */
    uint16_t group, room;     /* wActiveGroup / wActiveRoom */
} CartMap;

static const CartMap CARTS[] = {
    { "tlozooa", "Oracle of Ages",
      0xC6AA, 0xC6AB, 0xC6AD, 0xC6BF, 0xCC2D, 0xCC30 },
    { "tlozoos", "Oracle of Seasons",
      0xC6A2, 0xC6A3, 0xC6A5, 0xC6BB, 0xCC49, 0xCC4C },
};

/* Same in both games. */
#define A_RINGS        0xC616   /* wRingsObtained, 8-byte bitset */
#define A_DEATHS       0xC61E   /* wDeathCounter, 2 bytes BCD    */
#define A_KILLS        0xC620   /* wTotalEnemiesKilled, word     */
#define A_PLAYTIME     0xC622   /* wPlaytimeCounter, 4 bytes     */
#define A_RUPEES_TOTAL 0xC627   /* wTotalRupeesCollected, word   */
#define A_LINKED       0xC612   /* wFileIsLinkedGame             */
#define A_SCROLL       0xCD00   /* wScrollMode                   */

static uint8_t rd(const uint8_t* wram, uint16_t addr) {
    return wram[addr - 0xC000];
}
static int rd16(const uint8_t* wram, uint16_t addr) {
    return (int)rd(wram, addr) | ((int)rd(wram, addr + 1) << 8);
}
static int popcount8(uint8_t v) {
    int n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

bool epoch_stream_read(EpochStreamState* out, const uint8_t* wram,
                       const char* game_id) {
    const CartMap* cart = NULL;
    for (size_t i = 0; i < sizeof(CARTS) / sizeof(CARTS[0]); i++)
        if (game_id && strcmp(CARTS[i].id, game_id) == 0) cart = &CARTS[i];
    if (!cart) return false;

    /* The same gate the achievements use: a file's data is in WRAM AND we
     * are standing in a room. Without the second half the file-select
     * preview would drive the overlay. */
    if (rd(wram, cart->link_max_health) == 0) return false;
    if (rd(wram, A_SCROLL) == 0) return false;

    snprintf(out->cart, sizeof(out->cart), "%s", cart->id);
    snprintf(out->title, sizeof(out->title), "%s", cart->title);
    out->group = rd(wram, cart->group);
    out->room  = rd(wram, cart->room);
    out->essences = popcount8(rd(wram, cart->essences));
    out->hearts = rd(wram, cart->link_health);
    out->max_hearts = rd(wram, cart->link_max_health);
    out->rupees = rd16(wram, cart->rupees);

    out->rings = 0;
    for (int i = 0; i < 8; i++) out->rings += popcount8(rd(wram, A_RINGS + i));

    {   /* Two bytes of packed BCD, low byte first: 0x23 0x01 = 123. */
        int lo = rd(wram, A_DEATHS), hi = rd(wram, A_DEATHS + 1);
        out->deaths = (hi >> 4) * 1000 + (hi & 0xF) * 100 +
                      (lo >> 4) * 10 + (lo & 0xF);
    }
    out->kills = rd16(wram, A_KILLS);
    out->rupees_total = rd16(wram, A_RUPEES_TOTAL);
    out->linked = rd(wram, A_LINKED) != 0;

    {   /* A frame counter; the games run at ~60 Hz. */
        uint32_t frames = (uint32_t)rd(wram, A_PLAYTIME)
                        | ((uint32_t)rd(wram, A_PLAYTIME + 1) << 8)
                        | ((uint32_t)rd(wram, A_PLAYTIME + 2) << 16)
                        | ((uint32_t)rd(wram, A_PLAYTIME + 3) << 24);
        out->play_seconds = (int)(frames / 60);
    }

    epoch_achievements_progress(&out->unlocked, &out->total);
    {
        const char *id = NULL, *title = NULL, *desc = NULL;
        epoch_achievements_last_unlock(&id, &title, &desc, &out->unlock_serial);
        snprintf(out->last_id, sizeof(out->last_id), "%s", id ? id : "");
        snprintf(out->last_title, sizeof(out->last_title), "%s", title ? title : "");
        snprintf(out->last_desc, sizeof(out->last_desc), "%s", desc ? desc : "");
    }
    return true;
}

/* Text going into a JS string literal. Only quotes and backslashes can
 * break out, and the strings here come from pack files a player edits,
 * so escape rather than trust. Control characters are dropped. */
static void js_escape(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') out[o++] = '\\';
        out[o++] = (char)c;
    }
    out[o < cap ? o : cap - 1] = 0;
}

int epoch_stream_format(const EpochStreamState* s, char* buf, size_t cap) {
    char title[160], desc[240], id[120], cart_title[80];
    js_escape(s->last_title, title, sizeof(title));
    js_escape(s->last_desc, desc, sizeof(desc));
    js_escape(s->last_id, id, sizeof(id));
    js_escape(s->title, cart_title, sizeof(cart_title));

    int n = snprintf(buf, cap,
        "EPOCH({cart:\"%s\",title:\"%s\",room:\"%X-%02X\",essences:%d,"
        "hearts:%d,maxHearts:%d,rings:%d,deaths:%d,kills:%d,rupees:%d,"
        "rupeesTotal:%d,seconds:%d,linked:%s,unlocked:%d,total:%d,"
        "lastId:\"%s\",lastTitle:\"%s\",lastDesc:\"%s\",serial:%u,"
        "tick:%u});\n",
        s->cart, cart_title, s->group & 0xFF, s->room & 0xFF, s->essences,
        s->hearts, s->max_hearts, s->rings, s->deaths, s->kills, s->rupees,
        s->rupees_total, s->play_seconds, s->linked ? "true" : "false",
        s->unlocked, s->total, id, title, desc,
        (unsigned)s->unlock_serial, (unsigned)s->tick);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

void epoch_stream_tick(GBContext* ctx, const char* game_id) {
    if (!ctx || !ctx->wram || !game_id) return;

    static EpochStreamState state;
    static uint32_t last_serial = 0;
    static int countdown = 0;
    static bool warned = false;

    const bool fresh = epoch_stream_read(&state, ctx->wram, game_id);
    if (!fresh && state.cart[0] == 0) return;   /* nothing worth writing yet */

    /* Once a second is plenty for a viewer, but an unlock should land the
     * moment it happens -- that is the thing people are watching for. */
    const bool unlocked_now = state.unlock_serial != last_serial;
    if (countdown-- > 0 && !unlocked_now) return;
    countdown = 60;
    last_serial = state.unlock_serial;

    /* The heartbeat. The player leaves live.js on disk when it exits --
     * deleting it would only help a clean shutdown anyway -- so "is the
     * file there" says nothing about whether anyone is playing. This
     * does: it moves on every write, and stops dead when the player
     * does, which is how the overlay knows to take the panel down
     * instead of leaving an hour-old heart count on screen. */
    state.tick++;

    char line[1024];
    if (epoch_stream_format(&state, line, sizeof(line)) < 0) return;

    es_mkdir("stream");
    /* Write beside the target and rename over it: OBS polls this file
     * several times a second, and half a line reaching it would throw a
     * syntax error into the overlay. */
    FILE* f = fopen("stream/live.js.tmp", "w");
    if (!f) {
        if (!warned) {
            fprintf(stderr, "[stream] cannot write stream/live.js.tmp; "
                            "overlay stats disabled\n");
            warned = true;
        }
        return;
    }
    fputs(line, f);
    fclose(f);
    remove("stream/live.js");
    if (rename("stream/live.js.tmp", "stream/live.js") != 0 && !warned) {
        fprintf(stderr, "[stream] cannot replace stream/live.js\n");
        warned = true;
    }
}
