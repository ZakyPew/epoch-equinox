/* Stream overlay switches.
 *
 * Written by the launcher's Stream page, and safe to edit by hand. The
 * overlays load this the same way they load now.js: as a script from
 * their own folder, because OBS opens them over file:// where fetch()
 * is blocked.
 *
 *   cam    a second, camera-shaped opening in the framed layouts
 *   guide  print each opening's exact rectangle over it, for lining a
 *          capture up in OBS -- turn it off before going live
 */
CONFIG({cam: false, guide: false});
