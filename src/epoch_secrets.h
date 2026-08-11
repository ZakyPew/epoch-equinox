/* Auto-entered secrets: the player types the code for you.
 *
 * The generator in the launcher tells you what to type; this types it.
 * Open a secret-entry screen, pick the secret from the Esc menu, and a
 * feedback controller walks the game's own cursor around the symbol
 * grid and presses A on each symbol in turn -- no ROM patching, no
 * memory writes into the entry buffer. It reads where the cursor *is*
 * from WRAM and pushes the d-pad accordingly, so if it ever disagrees
 * with the game the game wins.
 *
 * The encoder here is the same algorithm as launcher/oracle_secrets.py,
 * ported to C because this side reads live WRAM rather than a .sav.
 */
#ifndef EPOCH_SECRETS_H
#define EPOCH_SECRETS_H

#include <stdbool.h>
#include <stdint.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { ES_MAX_SYMBOLS = 20 };

/* Secret types, matching wSecretType. */
typedef enum {
    ES_TYPE_GAME  = 0,   /* 20 symbols: game transfer / hero's secret */
    ES_TYPE_RING  = 2,   /* 15 symbols: ring collection              */
    ES_TYPE_SHORT = 3,   /* 5 symbols: the NPC and return secrets    */
} EsType;

/* ---- encoder (pure, testable without a cart) ----------------------- */

/** Encode a secret from a $100-byte copy of the c6xx block.
 *  `short_index` is only read for ES_TYPE_SHORT. Writes `out` symbol
 *  values (0-63) and returns how many, or 0 on a bad type. */
int es_encode(const uint8_t* c6, EsType type, uint8_t short_index,
              uint8_t* out);

/** Symbol value -> the character the entry grid shows. */
char es_symbol_char(uint8_t value);

/** Decode symbols back to {type, game id}; false if the checksum fails.
 *  Used by the tests, and to sanity-check before typing. */
bool es_decode(const uint8_t* cells, int count, int* type, int* game_id);

/* ---- the typist ---------------------------------------------------- */

/** Queue a secret to be typed into whatever entry screen is open.
 *  Returns false if a secret is already being typed. */
bool epoch_secrets_type(const uint8_t* cells, int count);

/** True while symbols are still being entered. */
bool epoch_secrets_busy(void);

/** Abandon the current run (the player pressed something). */
void epoch_secrets_cancel(void);

/** Service one frame. Call after gb_platform_poll_events so the
 *  injected buttons survive into the frame the cart runs. */
void epoch_secrets_tick(GBContext* ctx, const char* game_id);

/** Progress and a one-line status for the menu. */
void epoch_secrets_status(int* done, int* total, const char** message);

/** Build a secret from the live save in WRAM and start typing it. */
bool epoch_secrets_type_from_wram(GBContext* ctx, EsType type,
                                  uint8_t short_index);

/** Same, using the context the tick last saw -- for the Esc menu, which
 *  is drawn without one. */
bool epoch_secrets_type_current(EsType type, uint8_t short_index);

/** True when a file's data is actually loaded (not the title screen). */
bool epoch_secrets_have_save(void);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_SECRETS_H */
