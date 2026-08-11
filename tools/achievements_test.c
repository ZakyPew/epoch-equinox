/* Exercises the achievements engine without a cart: parses a pack from a
 * scratch file, drives a synthetic WRAM buffer through the milestones the
 * shipped packs watch, and checks unlocks fire once, persist, and respect
 * the title-screen gate.
 *
 * Build like vox_shot (it links the same libraries):
 *
 *   cc -O2 -o achievements_test ../tools/achievements_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 *
 * Also validates the packs shipped in achievements/ parse clean when run
 * from the repo root's build directory. Exits nonzero on any failure.
 */
#include "epoch_achievements.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) { printf("ok   %s\n", name); }                     \
        else { printf("FAIL %s\n", name); failures++; }              \
    } while (0)

static uint8_t wram[0x2000];
static void w8(uint16_t addr, uint8_t v)  { wram[addr - 0xC000] = v; }
static void w16(uint16_t addr, uint16_t v) {
    wram[addr - 0xC000] = (uint8_t)(v & 0xFF);
    wram[addr - 0xC000 + 1] = (uint8_t)(v >> 8);
}

static const char* PACK =
    "# scratch pack\n"
    "[essences]\n"
    "title = Essences\n"
    "desc = four essences\n"
    "when = bits c6bf >= 4\n"
    "\n"
    "[hearts]\n"
    "title = Hearts\n"
    "when = byte c6ab >= 40\n"
    "\n"
    "[deathless]\n"
    "title = Deathless\n"
    "when = bits c6bf >= 2\n"
    "when = bcd c61e == 0\n"
    "\n"
    "[kills]\n"
    "title = Kills\n"
    "when = word c620 >= 1000\n"
    "\n"
    "[flippers]\n"
    "title = Flippers\n"
    "when = flag c69f 6\n"
    "\n"
    "[rings]\n"
    "title = Rings\n"
    "when = bitset c616 8 >= 3\n"
    "\n"
    "[broken]\n"
    "title = Never loads\n"
    "when = byte 8000 >= 1\n"          /* outside WRAM: dropped */
    "\n"
    "[halfbroken]\n"
    "title = Also never loads\n"
    "when = byte c6ab >= 1\n"
    "when = frobnicate c6ab >= 1\n";   /* bad kind: drops the entry */

static const char* pack_path  = "achv_test_pack.txt";
static const char* state_path = "achv_test_state.txt";

int main(void) {
    /* -- parse ------------------------------------------------------ */
    FILE* f = fopen(pack_path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", pack_path); return 1; }
    fputs(PACK, f);
    fclose(f);

    static EaSet set;
    memset(&set, 0, sizeof(set));
    set.gate_addr  = 0xC6AB;   /* wLinkMaxHealth, as the runner sets it */
    set.gate_addr2 = 0xCD00;   /* wScrollMode */

    int added = ea_load_pack(&set, pack_path);
    CHECK(added == 8, "eight entries parsed (two of them broken)");
    int watchable = 0;
    for (int i = 0; i < set.count; i++) watchable += set.list[i].n_conds > 0;
    CHECK(watchable == 6, "the two broken entries dropped their conditions");

    /* -- the gate --------------------------------------------------- */
    int newly[16];
    memset(wram, 0, sizeof(wram));
    w8(0xC6BF, 0xFF);                       /* all essences... */
    int n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 0, "title screen (gate byte 0) unlocks nothing");

    /* File select: the preview card loads the file's data into WRAM,
     * but scroll mode stays 0. Browsing must never unlock. */
    w8(0xC6AB, 40);
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 0, "file select (health set, scroll 0) unlocks nothing");
    w8(0xC6BF, 0x00);
    w8(0xC6AB, 0x00);

    /* -- unlocks fire once, at the right thresholds ----------------- */
    w8(0xC6AB, 12);                          /* 3 hearts... */
    w8(0xCD00, 1);                           /* ...and standing in a room */
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 0, "fresh file unlocks nothing");

    w8(0xC6BF, 0x07);                        /* three essences */
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 1 && !strcmp(set.list[newly[0]].id, "deathless"),
          "three essences + no deaths = deathless only");

    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 0, "an unlocked achievement does not fire again");

    w8(0xC6BF, 0x0F);                        /* four essences */
    w8(0xC6AB, 40);                          /* ten hearts */
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 2, "four essences + ten hearts fire together");

    /* -- word, flag, bitset, bcd ------------------------------------ */
    w16(0xC620, 999);
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 0, "999 kills is not 1000");
    w16(0xC620, 1000);
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 1 && !strcmp(set.list[newly[0]].id, "kills"), "1000 kills is");

    w8(0xC69F, 0x40);                        /* bit 6: flippers */
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 1 && !strcmp(set.list[newly[0]].id, "flippers"),
          "treasure flag bit 6 reads as the flippers");

    w8(0xC616, 0x03);                        /* two rings... */
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 0, "two rings across the bitset is not three");
    w8(0xC61D, 0x80);                        /* ...and one in the last byte */
    n = ea_evaluate(&set, wram, newly, 16);
    CHECK(n == 1 && !strcmp(set.list[newly[0]].id, "rings"),
          "bitset counts across all eight bytes");

    /* BCD: a death counter of 0x0123 means 123 deaths, not 291. */
    {
        EaCond c = { EA_BCD, 0xC61E, 0, EA_EQ, 123 };
        w8(0xC61E, 0x23);
        w8(0xC61F, 0x01);
        CHECK(ea_cond_holds(&c, wram), "bcd 23 01 reads as 123");
    }

    /* -- persistence ------------------------------------------------ */
    remove(state_path);
    ea_save_unlock(state_path, "essences");
    ea_save_unlock(state_path, "kills");

    static EaSet set2;
    memset(&set2, 0, sizeof(set2));
    ea_load_pack(&set2, pack_path);
    ea_load_unlocked(&set2, state_path);
    int pre = 0;
    for (int i = 0; i < set2.count; i++) pre += set2.list[i].unlocked;
    CHECK(pre == 2, "state file restores exactly its two ids");

    memset(wram, 0, sizeof(wram));
    w8(0xC6AB, 12);
    w8(0xCD00, 1);
    w8(0xC6BF, 0xFF);
    w16(0xC620, 5000);
    set2.gate_addr  = 0xC6AB;
    set2.gate_addr2 = 0xCD00;
    n = ea_evaluate(&set2, wram, newly, 16);
    int refired = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(set2.list[newly[i]].id, "essences")) refired = 1;
        if (!strcmp(set2.list[newly[i]].id, "kills")) refired = 1;
    }
    CHECK(!refired, "restored unlocks never re-toast");

    /* -- toast queue ------------------------------------------------ */
    CHECK(ea_toast_current() == NULL, "no toast before any unlock");
    ea_toast_push("first", "First", "one");
    ea_toast_push("second", "Second", "two");
    CHECK(ea_toast_current() && !strcmp(ea_toast_current()->title, "First"),
          "toasts show in order");
    ea_toast_advance(10.0f);                 /* well past one lifetime */
    CHECK(ea_toast_current() && !strcmp(ea_toast_current()->title, "Second"),
          "a finished toast hands over to the next");
    ea_toast_advance(10.0f);
    CHECK(ea_toast_current() == NULL, "the queue drains");

    /* -- icons ------------------------------------------------------ */
    {
        static EaIcon icon;
        const char* icon_path = "achv_test_icon.ppm";
        FILE* pf = fopen(icon_path, "wb");
        /* 3x2, with a comment in the header and one magenta pixel. */
        fprintf(pf, "P6\n# a comment\n3 2\n255\n");
        const uint8_t px[18] = {
            255, 0, 0,   0, 255, 0,   0, 0, 255,
            255, 0, 255, 9, 9, 9,     255, 255, 255,
        };
        fwrite(px, 1, sizeof(px), pf);
        fclose(pf);

        CHECK(ea_load_ppm(icon_path, &icon), "ppm with comments loads");
        CHECK(icon.w == 3 && icon.h == 2, "ppm size read");
        CHECK(icon.px[0] == 0xFFFF0000u, "red pixel decoded");
        CHECK((icon.px[3] >> 24) == 0, "magenta reads as transparent");
        CHECK(icon.px[5] == 0xFFFFFFFFu, "white pixel decoded");

        /* Oversize is refused rather than truncated. */
        pf = fopen(icon_path, "wb");
        fprintf(pf, "P6\n%d %d\n255\n", EA_ICON_DIM + 1, 1);
        for (int i = 0; i < (EA_ICON_DIM + 1) * 3; i++) fputc(0, pf);
        fclose(pf);
        CHECK(!ea_load_ppm(icon_path, &icon), "oversize ppm refused");
        remove(icon_path);
    }

    /* -- shipped packs parse clean, from the repo root -------------- */
    {
        static EaSet ship;
        memset(&ship, 0, sizeof(ship));
        int a = ea_load_pack(&ship, "../achievements/tlozooa.txt");
        int have_a = a;
        int bad = 0;
        for (int i = 0; i < ship.count; i++) bad += ship.list[i].n_conds == 0;
        int s = ea_load_pack(&ship, "../achievements/tlozoos.txt");
        for (int i = 0; i < ship.count; i++) bad += ship.list[i].n_conds == 0;
        if (a < 0 || s < 0) {
            printf("note: shipped packs not beside this binary; skipped\n");
        } else {
            CHECK(have_a >= 15 && s >= 15, "shipped packs load fully");
            CHECK(bad == 0, "no shipped entry lost its conditions");
        }
    }

    remove(pack_path);
    remove(state_path);

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
