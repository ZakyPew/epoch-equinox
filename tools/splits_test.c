/* Exercises the auto-splitter without a cart, a window, or LiveSplit.
 *
 * A split that fires late is a nuisance; a split that fires when it
 * should not is a ruined run, and the two ways that happens are loading
 * a save (every condition it already satisfies looks "new") and the file
 * select screen (a save block sits in WRAM just to draw the preview).
 * Both are checked here, along with ordering and the recorded times.
 *
 * Build like the other probes (it links the same libraries):
 *
 *   cc -O2 -o splits_test ../tools/splits_test.c -I ../src \
 *      -I _deps/gb_recompiled-src/runtime/include \
 *      $(sdl2-config --cflags) libepoch_support.a _gbrt_build/libgbrt.a \
 *      $(sdl2-config --libs) -lGL -lcurl -lm -lstdc++
 */
#include "epoch_splits.h"

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
static void w8(uint16_t a, uint8_t v) { wram[a - 0xC000] = v; }

/* A three-segment route: an item, then two essence counts. */
static const char* PACK =
    "[harp]\n"
    "title = Harp of Ages\n"
    "when = flag c69c 1\n"
    "\n"
    "[essence-1]\n"
    "title = Eternal Spirit\n"
    "when = bits c6bf >= 1\n"
    "\n"
    "[essence-2]\n"
    "title = Ancient Wood\n"
    "when = bits c6bf >= 2\n";

static const char* write_pack(const char* body) {
    static char path[] = "splits_test_pack.txt";
    FILE* f = fopen(path, "w");
    if (!f) { printf("FAIL cannot write %s\n", path); exit(1); }
    fputs(body, f);
    fclose(f);
    return path;
}

/* A file loaded and standing in a room, so the gate is open. */
static void in_play(void) {
    memset(wram, 0, sizeof(wram));
    w8(0xC6AB, 40);        /* wLinkMaxHealth */
    w8(0xCD00, 1);         /* wScrollMode    */
}

int main(void) {
    const char* path = write_pack(PACK);
    EsRun run;

    CHECK(es_load(&run, path) == 3, "three splits load from a pack file");
    CHECK(strcmp(epoch_splits_name(&run, 0), "Harp of Ages") == 0,
          "the title is the segment name");
    {
        EsRun missing;
        CHECK(es_load(&missing, "splits_test_nothing.txt") == -1,
              "a cart with no split file is not an error");
    }

    run.set.gate_addr = 0xC6AB;
    run.set.gate_addr2 = 0xCD00;

    int fired[ES_MAX_SPLITS];

    /* -- a fresh file, nothing earned yet ------------------------------ */
    in_play();
    CHECK(es_evaluate(&run, wram, 0, fired, ES_MAX_SPLITS) == 0,
          "a fresh run fires nothing on the first frame");
    CHECK(run.next == 0, "and is waiting on the first segment");

    /* -- earning them, one at a time ----------------------------------- */
    w8(0xC69C, 0x02);                 /* treasure $11, the harp */
    int n = es_evaluate(&run, wram, 600, fired, ES_MAX_SPLITS);
    CHECK(n == 1 && fired[0] == 0, "the harp fires its own split");
    CHECK(run.hit[0].done && run.hit[0].frame == 600,
          "and the time is recorded in frames");
    CHECK(run.next == 1, "the next segment is now the second");

    CHECK(es_evaluate(&run, wram, 660, fired, ES_MAX_SPLITS) == 0,
          "a split already taken does not fire again");

    w8(0xC6BF, 0x01);                 /* one essence */
    n = es_evaluate(&run, wram, 1200, fired, ES_MAX_SPLITS);
    CHECK(n == 1 && fired[0] == 1, "the first essence fires the next split");

    w8(0xC6BF, 0x03);                 /* two essences */
    n = es_evaluate(&run, wram, 1800, fired, ES_MAX_SPLITS);
    CHECK(n == 1 && fired[0] == 2, "the second essence fires the last split");
    CHECK(run.next == 3, "the route is complete");
    CHECK(run.hit[2].frame == 1800, "with each time kept separately");

    /* -- the file select screen ---------------------------------------- *
     * A save block in WRAM with the gate shut must not split. This is the
     * bug that would fire a whole route by browsing the file menu. */
    {
        EsRun browse;
        es_load(&browse, path);
        browse.set.gate_addr = 0xC6AB;
        browse.set.gate_addr2 = 0xCD00;
        memset(wram, 0, sizeof(wram));
        w8(0xC6AB, 40);               /* health set... */
        w8(0xCD00, 0);                /* ...but not in a room */
        w8(0xC69C, 0x02);
        w8(0xC6BF, 0x03);
        CHECK(es_evaluate(&browse, wram, 5000, fired, ES_MAX_SPLITS) == 0,
              "the file select screen splits nothing");
    }

    /* -- loading a half-finished save ---------------------------------- *
     * Everything it already has is not something you just did. */
    {
        EsRun loaded;
        es_load(&loaded, path);
        loaded.set.gate_addr = 0xC6AB;
        loaded.set.gate_addr2 = 0xCD00;
        in_play();
        w8(0xC69C, 0x02);             /* already has the harp */
        w8(0xC6BF, 0x01);             /* and one essence      */
        CHECK(es_evaluate(&loaded, wram, 90000, fired, ES_MAX_SPLITS) == 0,
              "loading a save mid-route fires nothing");
        CHECK(loaded.hit[0].done && loaded.hit[1].done,
              "but those segments are marked as already behind you");
        CHECK(loaded.next == 2, "and the run resumes at the right segment");

        w8(0xC6BF, 0x03);
        n = es_evaluate(&loaded, wram, 91000, fired, ES_MAX_SPLITS);
        CHECK(n == 1 && fired[0] == 2,
              "the next real one still fires");
    }

    /* -- a new run ------------------------------------------------------ *
     * The game's clock only goes forward within a file, so it going back
     * is a different file: last run's times are not ours. */
    CHECK(es_maybe_reset(&run, 10) == true,
          "the clock jumping backwards starts a new run");
    CHECK(!run.hit[0].done && !run.hit[2].done,
          "and clears the times");
    CHECK(run.next == 0, "back to the first segment");
    CHECK(es_maybe_reset(&run, 400) == false,
          "the clock going forward is just the run continuing");

    /* A savestate load nudges the counter back a little; that is not a
     * new run and must not wipe the splits. */
    run.hit[0].done = true;
    CHECK(es_maybe_reset(&run, 380) == false,
          "a small step backwards is a savestate, not a reset");
    CHECK(run.hit[0].done, "so the times survive it");

    remove(path);

    /* -- the routes we actually ship ----------------------------------- *
     * Everything above proves the engine. This proves the two files, by
     * running them against the real saves in tests/saves. Both are finished
     * files, so every segment of that cart's route should read as already
     * behind you and none of them should fire. A transposed address or a
     * stale bit index leaves a segment unearned and names it below. */
    for (int which = 0; which < 2; which++) {
        const char* cart = which ? "tlozoos" : "tlozooa";
        const char* sav  = which ? "seasons-room-of-rites.sav"
                                 : "ages-veran-tower.sav";
        const uint16_t gate = which ? 0xC6A3 : 0xC6AB;

        char pack[256], save[256];
        snprintf(pack, sizeof(pack), "../splits/%s.txt", cart);
        snprintf(save, sizeof(save), "../tests/saves/%s", sav);

        EsRun ship;
        int n_splits = es_load(&ship, pack);
        if (n_splits <= 0) {
            snprintf(pack, sizeof(pack), "splits/%s.txt", cart);
            n_splits = es_load(&ship, pack);
        }
        CHECK(n_splits > 0, which ? "seasons route parses"
                                  : "ages route parses");
        if (n_splits <= 0) continue;

        FILE* fp = fopen(save, "rb");
        if (!fp) {
            snprintf(save, sizeof(save), "tests/saves/%s", sav);
            fp = fopen(save, "rb");
        }
        if (!fp) {
            printf("skip %s (no %s to check against)\n", cart, sav);
            continue;
        }
        static uint8_t raw[0x8000];
        size_t got = fread(raw, 1, sizeof(raw), fp);
        fclose(fp);

        /* The save's file block holds a copy of the $c6xx page: the file
         * starts at $010 and wc600Block sits $50 into it. */
        const size_t C6 = 0x010 + 0x50;
        CHECK(got >= C6 + 0x100, "the save is long enough to hold a file");
        memset(wram, 0, sizeof(wram));
        memcpy(wram + (0xC600 - 0xC000), raw + C6, 0x100);
        w8(0xCD00, 1);                       /* standing in a room */
        CHECK(wram[gate - 0xC000] != 0,
              which ? "seasons save has health where seasons keeps it"
                    : "ages save has health where ages keeps it");

        ship.set.gate_addr = gate;
        ship.set.gate_addr2 = 0xCD00;
        CHECK(es_evaluate(&ship, wram, 500000, fired, ES_MAX_SPLITS) == 0,
              "a finished file fires nothing when loaded");

        int done = 0;
        for (int i = 0; i < ship.count; i++) if (ship.hit[i].done) done++;
        if (!which) {
            CHECK(done == ship.count,
                  "every Ages segment reads as earned on a Veran's tower file");
        } else {
            CHECK(done == ship.count,
                  "every Seasons segment reads as earned on a Room of Rites file");
        }
        if (done != ship.count) {
            for (int i = 0; i < ship.count; i++)
                if (!ship.hit[i].done)
                    printf("       missing: %s\n", epoch_splits_name(&ship, i));
        }
    }

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
