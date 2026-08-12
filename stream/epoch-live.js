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

  /* ---- scene options ------------------------------------------------
     The camera opening and the alignment guide are switches rather than
     layout: the launcher's Stream page writes them into config.js, and
     ?cam / ?guide (or the C and G keys) still override for a quick look.
     A query string is no use in OBS's "Local file" mode, which is how
     most people add these, so the file is the real setting. */
  function scene(opts) {
    if (!opts) return;
    var body = document.body;
    if ('cam' in opts) body.classList.toggle('cam-on', !!opts.cam);
    if ('guide' in opts) body.classList.toggle('guide-on', !!opts.guide);
    label();
  }
  window.CONFIG = scene;

  /* The guide prints the rectangle to type into OBS's Transform box. It
     is read back out of the CSS rather than written twice, so moving a
     box in config or by hand cannot leave the label lying. */
  function label() {
    var css = getComputedStyle(document.documentElement);
    function num(name) { return parseInt(css.getPropertyValue(name), 10); }
    function fill(id, prefix, p) {
      var e = el(id);
      if (!e) return;
      var x = num('--' + p + '-x'), y = num('--' + p + '-y'),
          w = num('--' + p + '-w'), h = num('--' + p + '-h');
      if ([x, y, w, h].some(isNaN)) { e.textContent = ''; return; }
      var text = prefix + ' — position ' + x + ', ' + y +
                 ' · size ' + w + ' × ' + h;
      /* Whole multiples of the Game Boy screen keep flat mode crisp, so
         say so when it is one -- that is the reason for the odd sizes. */
      if (w % 160 === 0 && h % 144 === 0 && w / 160 === h / 144) {
        text += ' (' + (w / 160) + '× of 160×144)';
      }
      e.textContent = text;
    }
    fill('guide-game', 'Game capture', 'box');
    fill('guide-cam', 'Camera', 'cam');
  }

  /* Each layout declares the canvas it was drawn for. Put it in a browser
     source of a different size and it does not scale -- it gets cropped,
     or stranded in a corner with the mat ending mid-screen, which looks
     like the overlay is broken rather than the source being misconfigured.
     Say which numbers to type instead of leaving someone to work it out
     from a mangled render. */
  function checkSize() {
    var want = document.documentElement.getBoundingClientRect();
    var w = Math.round(want.width), h = Math.round(want.height);
    var have = window.innerWidth, haveH = window.innerHeight;
    var id = 'epoch-size-warning';
    var box = el(id);
    if (Math.abs(w - have) < 2 && Math.abs(h - haveH) < 2) {
      if (box) box.remove();
      return;
    }
    if (!box) {
      box = document.createElement('div');
      box.id = id;
      box.style.cssText =
        'position:fixed;left:0;top:0;right:0;z-index:9999;' +
        'background:rgba(140,30,30,.94);color:#FFEFC0;' +
        'font:600 20px/1.45 ui-monospace,Menlo,Consolas,monospace;' +
        'padding:16px 22px;text-align:center;' +
        'border-bottom:2px solid #DEB24C;';
      document.body.appendChild(box);
    }
    box.textContent =
      'This overlay is ' + w + ' × ' + h + ', but the browser source is ' +
      have + ' × ' + haveH + '. Set the source to ' + w + ' × ' + h + '.';
  }
  checkSize();
  addEventListener('resize', checkSize);

  if (location.search.indexOf('guide') >= 0) document.body.classList.add('guide-on');
  if (location.search.indexOf('cam') >= 0) document.body.classList.add('cam-on');
  addEventListener('keydown', function (e) {
    if (e.key === 'g' || e.key === 'G') document.body.classList.toggle('guide-on');
    if (e.key === 'c' || e.key === 'C') document.body.classList.toggle('cam-on');
  });
  label();
  inject('config.js');

  /* ---- live game state --------------------------------------------- */
  var hud = el('hud');
  var card = el('card');
  var lastSerial = -1, cardTimer = null;
  /* Liveness is "the numbers moved", not "the file was there".
     The player leaves live.js on disk when it exits, so re-reading it
     succeeds forever and the panel would sit there showing an hour-old
     heart count. `tick` moves on every write and stops when the player
     does; a player too old to send one falls back to the rest of the
     payload changing, which it does while anyone is actually playing. */
  var lastBeat = null, beatAt = 0;

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

  /* The achievement's own icon, mirrored out of achievements/icons into
     stream/icons as PNG -- the packs ship PAM, which no browser reads.
     The drawn medal sits underneath and shows through whenever there is
     no icon: an achievement from a mod, a typo in an id, a folder that
     did not get copied. Ids come from pack files a player edits, so the
     path is built from known-safe characters only rather than trusted. */
  function icon(slotId, cart, id) {
    var img = el(slotId);
    if (!img) return;
    var slot = img.parentNode;
    /* The medal is drawn UNDER the icon, so it has to be taken away when
       one loads -- an icon is mostly transparent, and a medal showing
       through its gaps looks like a rendering fault. */
    function show(on) {
      img.hidden = !on;
      if (slot && slot.classList) slot.classList.toggle('has-icon', on);
    }
    var safe = /^[A-Za-z0-9._-]+$/;
    if (!cart || !id || !safe.test(cart) || !safe.test(id)) {
      show(false);
      return;
    }
    var src = 'icons/' + cart + '/' + id + '.png';
    if (img.getAttribute('src') === src && !img.hidden) return;
    show(false);
    img.onload = function () { show(true); };
    img.onerror = function () { show(false); };
    img.setAttribute('src', src);
  }

  window.EPOCH = function (s) {
    if (!s || !s.cart) return;

    var beat = 'tick' in s ? String(s.tick)
             : [s.seconds, s.rupees, s.hearts, s.room, s.deaths,
                s.kills, s.serial].join('|');
    if (beat !== lastBeat) { lastBeat = beat; beatAt = Date.now(); }
    else if (beatAt && Date.now() - beatAt > 12000) { return; }

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
        icon('last-icon', s.cart, s.lastId);
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
      icon('card-icon', s.cart, s.lastId);
      if (card) {
        card.classList.remove('show');
        void card.offsetWidth;            /* restart the animation */
        card.classList.add('show');
        clearTimeout(cardTimer);
        cardTimer = setTimeout(function () { card.classList.remove('show'); }, 7600);
      }
    }
  };

  function poll() {
    inject('live.js');
    /* Nothing new for twelve seconds means nobody is playing, whether
       the player exited, crashed, or the file went away. Take the panel
       down rather than leave stale numbers up looking live. */
    if (beatAt && Date.now() - beatAt > 12000) {
      if (hud) hud.hidden = true;
      var last = el('last');
      if (last) last.hidden = true;
    }
  }
  poll();
  setInterval(poll, 1000);
})();
