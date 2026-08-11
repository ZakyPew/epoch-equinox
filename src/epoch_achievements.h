/* Achievements: data-driven conditions over the game's WRAM, popped as
 * toasts over the window -- never drawn into the game itself.
 *
 * An achievement pack is a text file (see achievements/README.md); the
 * engine watches the conditions every frame and remembers unlocks in
 * states/. The emulated machine is only ever read.
 *
 * The core works on a plain WRAM buffer so it can be exercised without a
 * cart (tools/achievements_test.c); the GBContext glue is one thin call.
 */
#ifndef EPOCH_ACHIEVEMENTS_H
#define EPOCH_ACHIEVEMENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { EA_MAX_ACHIEVEMENTS = 128, EA_MAX_CONDS = 8 };

typedef enum {
    EA_BYTE,     /* one byte at addr                     */
    EA_WORD,     /* two bytes at addr, little-endian     */
    EA_BCD,      /* two bytes at addr, BCD (e.g. deaths) */
    EA_BITS,     /* popcount of the byte at addr         */
    EA_BITSET,   /* popcount over len bytes at addr      */
    EA_FLAG,     /* a single bit of the byte at addr     */
} EaKind;

typedef enum { EA_EQ, EA_NE, EA_GE, EA_LE, EA_GT, EA_LT } EaOp;

typedef struct {
    EaKind   kind;
    uint16_t addr;      /* WRAM address, 0xC000.. */
    uint8_t  len;       /* EA_BITSET: byte count; EA_FLAG: bit index */
    EaOp     op;
    uint32_t value;
} EaCond;

typedef struct {
    char   id[48];
    char   title[64];
    char   desc[96];
    EaCond conds[EA_MAX_CONDS];
    int    n_conds;
    bool   unlocked;
} EaAchievement;

typedef struct {
    EaAchievement list[EA_MAX_ACHIEVEMENTS];
    int           count;
    /* The in-game gate: evaluation is skipped while either byte is zero.
     * Both are needed. wLinkMaxHealth alone is not enough -- the file
     * select screen loads a file's c6xx block into WRAM just to draw the
     * preview card, so browsing a file would unlock everything it had
     * earned. wScrollMode is zero everywhere outside an actual room.
     * Set to wLinkMaxHealth (per cart) and wScrollMode. 0 disables. */
    uint16_t gate_addr;
    uint16_t gate_addr2;
} EaSet;

/* ---- core (no GBContext, no files beyond what you hand it) ---------- */

/** Parse one pack file into `set`, appending to what is already there.
 *  Unknown keys are skipped with a note; a malformed condition drops its
 *  whole achievement rather than half-watching it. Returns achievements
 *  added, -1 if the file cannot be read. */
int ea_load_pack(EaSet* set, const char* path);

/** Evaluate every condition against a WRAM snapshot (0x2000 bytes,
 *  0xC000-based). Newly satisfied achievements are marked unlocked and
 *  their indices written to `newly` (up to `cap`). Returns how many. */
int ea_evaluate(EaSet* set, const uint8_t* wram, int* newly, int cap);

/** One condition against WRAM; exposed for the tests. */
bool ea_cond_holds(const EaCond* c, const uint8_t* wram);

/* ---- persistence ---------------------------------------------------- */

/** Mark achievements listed in the state file as already unlocked. */
void ea_load_unlocked(EaSet* set, const char* path);

/** Append one achievement id to the state file. */
void ea_save_unlock(const char* path, const char* id);

/* ---- runner glue ---------------------------------------------------- */

/** Call once per emulated frame. Loads packs for the cart on first call,
 *  evaluates, and queues toasts for new unlocks. */
void epoch_achievements_tick(GBContext* ctx, const char* game_id);

/** How many are unlocked / defined, for the HUD's little tally. */
void epoch_achievements_progress(int* unlocked, int* total);

/* ---- toast queue (drawn by epoch_achievements_hud.cpp) -------------- */

typedef struct {
    char  title[64];
    char  desc[96];
    float age;        /* seconds since queued; drawer advances it */
} EaToast;

/** The toast now on screen, or NULL. The drawer owns the animation clock
 *  and calls ea_toast_advance to pass time / retire finished toasts. */
const EaToast* ea_toast_current(void);
void ea_toast_advance(float dt);

/** Queue a toast (used by the tick; exposed for a --toast-test). */
void ea_toast_push(const char* title, const char* desc);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_ACHIEVEMENTS_H */
