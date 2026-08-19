/* Talking to LiveSplit's Server component.
 *
 * LiveSplit (with the Server component started) listens on a TCP port and
 * takes one-word commands terminated by CRLF: "startorsplit", "split",
 * "reset", "pause", "resume". That is the whole protocol we need. It
 * never answers anything we care about, so this only ever writes.
 *
 * Two rules, because this runs on the thread that is emulating a Game
 * Boy sixty times a second:
 *
 *   1. Nothing blocks. The socket is non-blocking from the moment it is
 *      created, a connection in progress is simply not ready yet, and a
 *      send that would block is dropped rather than waited on.
 *   2. Nothing is fatal. LiveSplit not running, the Server component not
 *      started, the port taken, the cable pulled -- all of it is "no
 *      LiveSplit", which is the normal state for most people. It retries
 *      quietly on a timer and never says a word after the first notice.
 *
 * Off unless asked for: see epoch_livesplit_enable.
 */
#ifndef EPOCH_LIVESPLIT_H
#define EPOCH_LIVESPLIT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Turn the connection on or off. Off is the default; turning it off
 *  closes any socket immediately. `port` 0 means LiveSplit's default. */
void epoch_livesplit_enable(bool on, int port);

/** True when a socket is open and writable. Purely informational -- the
 *  send functions do not need it. */
bool epoch_livesplit_connected(void);

/** Send one command. Silently does nothing when disabled or not yet
 *  connected, so callers never have to check first. */
void epoch_livesplit_split(void);
void epoch_livesplit_reset(void);
void epoch_livesplit_pause(bool paused);

/** Called once per frame to nurse the connection along: finishes a
 *  connect that was in progress, and retries a dropped one now and
 *  then. Cheap enough to call unconditionally. */
void epoch_livesplit_tick(void);

/** Release the socket. Safe to call when there is none. */
void epoch_livesplit_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_LIVESPLIT_H */
