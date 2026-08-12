/* Live overlay data, shared by every layout in this folder.
 *
 * Two feeds, both delivered the same odd way:
 *
 *   now.js   NOW("chasing down the tree shapes")   -- a line you edit
 *   live.js  EPOCH({cart:"tlozooa", ...})          -- written by the player
 *
 * They are JavaScript rather than JSON or plain text for an annoying
 * reason: OBS loads a local overlay over file://, and Chromium blocks
 * fetch() and XHR against file:// URLs, so the obvious implementation
 * fails silently and the numbers never move. Loading a <script> from the
 * same folder IS allowed, so each feed is one call and this re-injects
 * it with a cache-buster.
 *
 * Every layout uses the same element ids, so this file has no idea which
 * one it is running in. Anything missing is skipped rather than thrown.
 */
(function () {
  function el(id) { return document.getElementById(id); }

  function inject(src) {
    var s = document.createElement('script');
    s.src = src + '?t=' + Date.now();
    s.onload = s.onerror = function () { s.remove(); };
    document.head.appendChild(s);
  }

  /* ---- the "now building" line ------------------------------------- */
  window.NOW = function (t) {
    var n = el('now');
    if (t && n) n.textContent = t;
  };
  inject('now.js');
  setInterval(function () { inject('now.js'); }, 4000);

  /* ---- live game state --------------------------------------------- */
  var hud = el('hud');
  var card = el('card');
  var lastSerial = -1, lastSeen = 0, cardTimer = null;

  function hearts(cur, max) {
    /* Health is stored in quarter-hearts. Show whole containers, with
       the ones you have lost hollowed out rather than removed, so the
       row does not jump around as you take damage. */
    var total = Math.max(0, Math.round(max / 4));
    var full = Math.max(0, Math.round(cur / 4));
    var out = '';
    for (var i = 0; i < total; i++) {
      out += '<span class="heart' + (i < full ? '' : ' empty') + '"><span></span></span>';
    }
    return out;
  }

  function pips(n) {
    var out = '';
    for (var i = 0; i < 8; i++) out += '<i class="pip' + (i < n ? ' on' : '') + '"></i>';
    return out;
  }

  function clock(sec) {
    var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60);
    return h + ':' + (m < 10 ? '0' : '') + m;
  }

  function set(id, text) { var e = el(id); if (e) e.textContent = text; }
  function html(id, markup) { var e = el(id); if (e) e.innerHTML = markup; }

  window.EPOCH = function (s) {
    if (!s || !s.cart) return;
    lastSeen = Date.now();
    if (hud) hud.hidden = false;

    set('hud-game', s.title || s.cart);
    set('hud-room', 'Room ' + s.room);
    var linked = el('hud-linked');
    if (linked) linked.hidden = !s.linked;
    html('pips', pips(s.essences || 0));
    html('hearts', hearts(s.hearts || 0, s.maxHearts || 0));
    set('rings', s.rings || 0);
    set('rupees', s.rupees || 0);
    set('deaths', s.deaths || 0);
    set('time', clock(s.seconds || 0));

    var pct = s.total ? (100 * s.unlocked / s.total) : 0;
    var fill = el('ach-fill');
    if (fill) fill.style.width = pct.toFixed(1) + '%';
    set('ach-text', (s.unlocked || 0) + ' / ' + (s.total || 0) + ' earned');

    /* The most recent unlock, standing rather than flying past. Layouts
       that have the room for it show a plaque; the ones that do not just
       have no #last and skip this. Empty until the first unlock of the
       save, so a fresh file does not advertise a blank. */
    var last = el('last');
    if (last) {
      var have = !!(s.lastTitle || s.lastId);
      last.hidden = !have;
      if (have) {
        set('last-title', s.lastTitle || s.lastId);
        set('last-desc', s.lastDesc || '');
      }
    }

    /* serial counts unlocks, so a new one is a change rather than the
       same title arriving again on the next poll. The first sample of
       a session is adopted silently: reloading the overlay mid-stream
       should not replay an achievement earned an hour ago. */
    if (lastSerial === -1) { lastSerial = s.serial || 0; return; }
    if ((s.serial || 0) !== lastSerial) {
      lastSerial = s.serial;
      set('card-title', s.lastTitle || s.lastId || '');
      set('card-desc', s.lastDesc || '');
      if (card) {
        card.classList.remove('show');
        void card.offsetWidth;            /* restart the animation */
        card.classList.add('show');
        clearTimeout(cardTimer);
        cardTimer = setTimeout(function () { card.classList.remove('show'); }, 7600);
      }
    }
  };

  function tick() {
    inject('live.js');
    /* The player stops writing when it exits. Rather than leave stale
       numbers up as if they were live, hide the panel. */
    if (lastSeen && Date.now() - lastSeen > 12000) {
      if (hud) hud.hidden = true;
      var last = el('last');
      if (last) last.hidden = true;
    }
  }
  tick();
  setInterval(tick, 1000);
})();
