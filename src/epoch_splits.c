/* See epoch_splits.h. */
#include "epoch_splits.h"

#include "epoch_livesplit.h"

#include <stdio.h>
#include <string.h>

int es_load(EsRun* run, const char* path) {
    memset(run, 0, sizeof(*run));
    int n = ea_load_pack(&run->set, path);
    if (n < 0) return -1;                    /* no file: not an error */
    run->count = run->set.count < ES_MAX_SPLITS ? run->set.count
                                               : ES_MAX_SPLITS;
    return run->count;
}

const char* epoch_splits_name(const EsRun* run, int i) {
    if (!run || i < 0 || i >= run->count) return "";
    /* The pack's title is the segment name; the id is what a tool would
     * key on, and the description is free for a note about the route. */
    return run->set.list[i].title;
}

bool es_maybe_reset(EsRun* run, uint32_t frames) {
    /* The game's clock only goes forward within a file. It going
     * backwards means a different file, or the same one reloaded --
     * either way this is a new run and last run's times are not ours.
     * A small step back is not enough: the counter is read from a live
     * machine and a savestate load can nudge it. */
    bool back = frames + 120 < run->last_frame;
    bool first = !run->running;
    run->last_frame = frames;
    if (!back && !first) return false;

    for (int i = 0; i < run->count; i++) {
        run->hit[i].frame = 0;
        run->hit[i].done = false;
    }
    /* The conditions themselves have to forget too, or a split that was
     * satisfied by the last file stays satisfied for this one. */
    for (int i = 0; i < run->set.count; i++) run->set.list[i].unlocked = false;
    run->next = 0;
    run->started = frames;
    run->running = true;
    return true;
}

int es_evaluate(EsRun* run, const uint8_t* wram, uint32_t frames,
                int* fired, int cap) {
    int out = 0;
    if (run->count == 0) return 0;

    if (es_maybe_reset(run, frames)) {
        /* Tell LiveSplit as well: a new file mid-session should not
         * carry the previous run's clock. */
        epoch_livesplit_reset();
        /* One evaluation to swallow whatever is already true of this
         * file -- loading a half-finished save must not fire six splits
         * at once -- then start watching from there. */
        int seen[ES_MAX_SPLITS];
        ea_evaluate(&run->set, wram, seen, ES_MAX_SPLITS);
        for (int i = 0; i < run->count; i++) {
            if (run->set.list[i].unlocked) {
                run->hit[i].done = true;
                run->hit[i].frame = frames;
                if (run->next == i) run->next = i + 1;
            }
        }
        return 0;
    }

    int newly[ES_MAX_SPLITS];
    int n = ea_evaluate(&run->set, wram, newly, ES_MAX_SPLITS);
    for (int k = 0; k < n; k++) {
        int i = newly[k];
        if (i < 0 || i >= run->count || run->hit[i].done) continue;
        run->hit[i].done = true;
        run->hit[i].frame = frames;
        if (out < cap) fired[out] = i;
        out++;
    }
    /* The segment being run is the first one still open, so a split that
     * fires out of order (a route that skips one) does not strand it. */
    while (run->next < run->count && run->hit[run->next].done) run->next++;
    return out;
}

/* ---- runner glue ---------------------------------------------------- */

static EsRun g_run;
static bool  g_loaded = false;
static char  g_cart[16];

const EsRun* epoch_splits_run(void) {
    return (g_loaded && g_run.count > 0) ? &g_run : NULL;
}

void epoch_splits_tick(GBContext* ctx, const char* game_id) {
    if (!ctx || !ctx->wram || !game_id) return;

    if (!g_loaded || strcmp(g_cart, game_id) != 0) {
        snprintf(g_cart, sizeof(g_cart), "%s", game_id);
        char path[256];
        snprintf(path, sizeof(path), "splits/%s.txt", game_id);
        int n = es_load(&g_run, path);
        if (n > 0) {
            fprintf(stderr, "[splits] %s: %d split(s)\n", path, n);
            /* The same gate the achievements use, for the same reason:
             * the file select screen has a save block in memory and
             * would otherwise split on someone else's progress. */
            g_run.set.gate_addr =
                strcmp(game_id, "tlozooa") == 0 ? 0xC6AB : 0xC6A3;
            g_run.set.gate_addr2 = 0xCD00;
        }
        g_loaded = true;
    }
    if (g_run.count == 0) return;

    epoch_livesplit_tick();

    /* The run's clock is the game's own, which is what the overlay and
     * LiveSplit both end up showing. */
    uint32_t frames = 0;
    for (int i = 0; i < 4; i++)
        frames |= (uint32_t)ctx->wram[0xC622 - 0xC000 + i] << (8 * i);

    int fired[ES_MAX_SPLITS];
    int n = es_evaluate(&g_run, ctx->wram, frames, fired, ES_MAX_SPLITS);
    for (int k = 0; k < n; k++) {
        fprintf(stderr, "[splits] %s at %u:%02u.%02u\n",
                epoch_splits_name(&g_run, fired[k]),
                frames / 3600, (frames / 60) % 60,
                (unsigned)((frames % 60) * 100 / 60));
        epoch_livesplit_split();
    }
}
