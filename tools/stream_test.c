/* Checks the stream overlay's data feed (src/epoch_stream.c).
 *
 * The overlay is a browser source in OBS: if the player emits a broken
 * line, the whole overlay goes blank mid-stream and nobody notices until
 * chat says so. So the emitted text is checked here rather than trusted --
 * the numbers it carries, the gate that stops the file select driving it,
 * and the escaping that keeps a pack author's apostrophe from ending the
 * JavaScript string early.
 *
 * Build like vox_shot (it links the same libraries):
 *
 *   cc -O2 -o stream_test ../tools/stream_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 */
#include "epoch_stream.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

static uint8_t wram[0x2000];
static void w8(uint16_t a, uint8_t v)  { wram[a - 0xC000] = v; }
static void w16(uint16_t a, uint16_t v) {
    wram[a - 0xC000] = (uint8_t)(v & 0xFF);
    wram[a - 0xC000 + 1] = (uint8_t)(v >> 8);
}

/* An Ages file, mid-adventure and standing in a room. */
static void ages_in_play(void) {
    memset(wram, 0, sizeof(wram));
    w8(0xC6AB, 40);              /* wLinkMaxHealth: 10 hearts */
    w8(0xC6AA, 36);              /* wLinkHealth: 9             */
    w8(0xCD00, 1);               /* wScrollMode: in a room     */
    w8(0xC6BF, 0x0F);            /* four essences              */
    w16(0xC6AD, 137);            /* rupees                     */
    w8(0xC616, 0xFF);            /* eight rings in byte 0      */
    w8(0xC617, 0x03);            /* two more                   */
    w8(0xC61E, 0x12);            /* deaths, BCD low            */
    w8(0xC61F, 0x00);
    w16(0xC620, 640);            /* kills                      */
    w16(0xC627, 4210);           /* rupees collected ever      */
    w8(0xCC2D, 0x00);            /* group                      */
    w8(0xCC30, 0x8A);            /* room                       */
    w8(0xC612, 0);               /* not linked                 */
    /* wPlaytimeCounter: 2 hours at 60 fps = 432000 frames. */
    w8(0xC622, 432000 & 0xFF);
    w8(0xC623, (432000 >> 8) & 0xFF);
    w8(0xC624, (432000 >> 16) & 0xFF);
    w8(0xC625, (432000 >> 24) & 0xFF);
}

int main(void) {
    EpochStreamState s;
    memset(&s, 0, sizeof(s));

    /* -- the gate ----------------------------------------------------- */
    memset(wram, 0, sizeof(wram));
    CHECK(!epoch_stream_read(&s, wram, "tlozooa"),
          "a blank WRAM (title screen) is refused");

    w8(0xC6AB, 40);              /* file data loaded... */
    CHECK(!epoch_stream_read(&s, wram, "tlozooa"),
          "file select (health set, scroll 0) is refused");

    w8(0xCD00, 1);
    CHECK(epoch_stream_read(&s, wram, "tlozooa"),
          "in a room, with a file, is accepted");

    CHECK(!epoch_stream_read(&s, wram, "pokemon-red"),
          "an unknown cart is refused rather than misread");

    /* -- the numbers -------------------------------------------------- */
    ages_in_play();
    CHECK(epoch_stream_read(&s, wram, "tlozooa"), "ages file reads");
    CHECK(strcmp(s.cart, "tlozooa") == 0, "cart id");
    CHECK(strcmp(s.title, "Oracle of Ages") == 0, "cart title");
    CHECK(s.essences == 4, "essences counted from the bitmask");
    CHECK(s.hearts == 36 && s.max_hearts == 40, "hearts");
    CHECK(s.rings == 10, "rings counted across the bitset");
    CHECK(s.deaths == 12, "deaths decoded from BCD");
    CHECK(s.kills == 640, "kills");
    CHECK(s.rupees == 137, "rupees");
    CHECK(s.rupees_total == 4210, "rupees collected ever");
    CHECK(s.play_seconds == 7200, "playtime in seconds");
    CHECK(s.group == 0 && s.room == 0x8A, "room");
    CHECK(!s.linked, "not a linked file");

    /* Seasons keeps the same block at different offsets. */
    memset(wram, 0, sizeof(wram));
    w8(0xC6A3, 52);              /* wLinkMaxHealth (Seasons) */
    w8(0xCD00, 1);
    w8(0xC6BB, 0xFF);            /* eight essences */
    w8(0xC612, 1);               /* linked game */
    CHECK(epoch_stream_read(&s, wram, "tlozoos"), "seasons file reads");
    CHECK(s.essences == 8 && s.max_hearts == 52,
          "seasons uses its own addresses");
    CHECK(s.linked, "linked flag");
    CHECK(strcmp(s.title, "Oracle of Seasons") == 0, "seasons title");

    /* -- the emitted line --------------------------------------------- */
    ages_in_play();
    epoch_stream_read(&s, wram, "tlozooa");
    char buf[1024];
    int n = epoch_stream_format(&s, buf, sizeof(buf));
    CHECK(n > 0, "formats");
    CHECK(strncmp(buf, "EPOCH({", 7) == 0, "calls EPOCH(...)");
    CHECK(strstr(buf, "});\n") != NULL, "closes the call");
    CHECK(strstr(buf, "essences:4") != NULL, "carries essences");
    CHECK(strstr(buf, "rings:10") != NULL, "carries rings");
    CHECK(strstr(buf, "room:\"0-8A\"") != NULL, "carries the room label");
    CHECK(strstr(buf, "linked:false") != NULL, "carries a real boolean");
    /* The heartbeat. Without it the overlay cannot tell a running player
     * from the file one left behind when it exited -- live.js stays on
     * disk either way, so re-reading it always succeeds. */
    CHECK(strstr(buf, "tick:") != NULL, "carries a heartbeat");
    {
        char a[1024], b[1024];
        s.tick = 7;
        epoch_stream_format(&s, a, sizeof(a));
        s.tick = 8;
        epoch_stream_format(&s, b, sizeof(b));
        CHECK(strstr(a, "tick:7") && strstr(b, "tick:8"),
              "the heartbeat is what changes between writes");
        CHECK(strcmp(a, b) != 0,
              "two writes of identical game state still differ");
        s.tick = 0;
    }

    /* A tiny buffer must be refused, not truncated into broken JS. */
    char small[40];
    CHECK(epoch_stream_format(&s, small, sizeof(small)) == -1,
          "a buffer too small is refused, never half a line");

    /* -- the item tracker and the run timer ---------------------------- *
     * Both read the save block, and a wrong address invents an item or a
     * time on someone's stream. These expectations come from the two real
     * saves in tests/saves: a late-game Ages file has the harp and no rod
     * of seasons, a linked Seasons file has the rod and no cane. */
    ages_in_play();
    /* wObtainedTreasureFlags, Ages at $c69a. Byte 0 covers ids 0-7. */
    w8(0xC69A, 0x7E);            /* shield,punch,bombs,somaria,sword,boomerang */
    w8(0xC69B, 0xC4);            /* switch hook, flute, seed shooter           */
    w8(0xC69C, 0xE2);            /* harp, shovel, bracelet, feather            */
    w8(0xC6B2, 2);               /* wSwordLevel                                */
    w8(0xC6AF, 2);               /* wShieldLevel                               */
    w8(0xC6B4, 2);               /* wSeedSatchelLevel                          */
    w8(0xC6B8, 2);               /* wBraceletLevel (Ages only)                 */
    w8(0xC6B0, 36); w8(0xC6B1, 48);   /* bombs / max                           */
    w8(0xC69E, 0x0F);            /* four of the five seed types, ids $20-$24   */
    CHECK(epoch_stream_read(&s, wram, "tlozooa"), "ages items read");
    CHECK(s.sword == 2 && s.shield == 2, "sword and shield tiers");
    CHECK(s.satchel == 2 && s.bracelet == 2, "satchel and bracelet tiers");
    CHECK(s.bombs == 36 && s.max_bombs == 48, "bombs held and capacity");
    CHECK(s.seeds == 4, "seed types counted from the treasure bits");
    CHECK(s.treasures[0] == 0x7E, "treasure flags come through whole");
    /* The bit layout is what the tracker indexes by, so pin it: id 17 is
     * the harp and id 7 the rod of seasons, which Ages does not have. */
    CHECK((s.treasures[17 >> 3] & (1 << (17 & 7))) != 0, "ages has the harp");
    CHECK((s.treasures[7 >> 3] & (1 << (7 & 7))) == 0,
          "ages does not have the rod of seasons");
    CHECK(s.play_frames == 432000, "playtime kept as frames for the timer");
    CHECK(s.play_seconds == 7200, "and still as seconds");

    /* Seasons keeps all of it somewhere else. A wrong column would read
     * the bracelet tier out of a byte that means something else. */
    memset(wram, 0, sizeof(wram));
    w8(0xC6A3, 64); w8(0xCD00, 1);
    w8(0xC6AC, 3);               /* wSwordLevel (Seasons) */
    w8(0xC6A9, 3);               /* wShieldLevel          */
    w8(0xC6AE, 3);               /* wSeedSatchelLevel     */
    w8(0xC6B8, 2);               /* Ages' bracelet address: must be ignored */
    w8(0xC692, 0xEE);            /* flags incl. the rod of seasons */
    CHECK(epoch_stream_read(&s, wram, "tlozoos"), "seasons items read");
    CHECK(s.sword == 3 && s.shield == 3 && s.satchel == 3,
          "seasons tiers use the seasons column");
    CHECK(s.bracelet == 0,
          "seasons has no bracelet address, so no bracelet is invented");
    CHECK((s.treasures[7 >> 3] & (1 << (7 & 7))) != 0,
          "seasons has the rod of seasons");

    {   /* The emitted line has to carry all of it. */
        char line[1024];
        CHECK(epoch_stream_format(&s, line, sizeof(line)) > 0,
              "formats with items");
        CHECK(strstr(line, "items:\"EE") != NULL,
              "treasure bits are emitted as hex");
        CHECK(strstr(line, "sword:3") && strstr(line, "satchel:3"),
              "tiers are emitted");
        CHECK(strstr(line, "frames:") != NULL, "frames are emitted");
    }

    /* -- escaping ------------------------------------------------------ */
    /* Pack titles are edited by players; a quote or a backslash in one
     * must not end the JavaScript string early. */
    snprintf(s.last_title, sizeof(s.last_title), "%s", "The \"Best\" \\ Ring");
    snprintf(s.last_desc, sizeof(s.last_desc), "%s", "line\nbreak");
    n = epoch_stream_format(&s, buf, sizeof(buf));
    CHECK(n > 0, "formats with hostile text");
    CHECK(strstr(buf, "\\\"Best\\\"") != NULL, "quotes are escaped");
    CHECK(strstr(buf, "\\\\ Ring") != NULL, "backslashes are escaped");
    CHECK(strstr(buf, "linebreak") != NULL, "control characters are dropped");
    /* The line must stay one line, or the overlay's parser sees garbage. */
    {
        int newlines = 0;
        for (int i = 0; i < n; i++) if (buf[i] == '\n') newlines++;
        CHECK(newlines == 1, "exactly one newline, at the end");
    }

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
