/* The one owner of the runtime's host-overlay hook.
 *
 * gb_platform_set_host_overlay takes a single function, and two modules
 * want to draw -- the rewind HUD in the corner and the achievement toast
 * up top. This is the two-line dispatcher that lets them both.
 */
#ifndef EPOCH_OVERLAY_H
#define EPOCH_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/** Install the shared overlay. Call after gb_platform_init();
 *  replaces the old epoch_rewind_install(). */
void epoch_overlay_install(void);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_OVERLAY_H */
