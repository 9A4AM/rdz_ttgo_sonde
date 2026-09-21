/* Theme resolution for the rdzTTGOSonde web UI.
 *
 * Loaded as a BLOCKING script in <head> (no defer/async) so data-theme is set
 * before first paint -- otherwise every page flashes light before going dark.
 *
 * Resolution order:
 *   1. localStorage['theme'] === 'light' | 'dark'  -> explicit, sticky, wins.
 *   2. nothing saved ("auto"): prefers-color-scheme: dark -> dark
 *   3. nothing saved: local hour >= NIGHT_START or < NIGHT_END -> dark
 *   4. otherwise -> light
 *
 * "Auto" is only the state before the user ever flips the switch; it is not a
 * UI option. While auto, the clock is re-checked every 60 s and OS changes are
 * followed. Once a preference is saved, BOTH stop -- an explicit choice must
 * never be overridden.
 *
 * Every page (including the iframes in index.html) loads this file. Changes
 * propagate through the 'storage' event, which fires in every same-origin
 * document except the one that wrote -- i.e. the sibling iframes and any other
 * open tab. ES5 only: no modules, no arrow functions, no let/const.
 *
 * rdzTheme.onChange(fn) calls fn(effective) once immediately at registration
 * (with the theme already applied at load time), in addition to on every
 * later change. Callers should expect that initial synchronous call and not
 * mistake it for a real change event.
 */
(function () {
  var KEY = 'theme';
  var NIGHT_START = 20;   /* 20:00 -> dark */
  var NIGHT_END = 8;      /*  8:00 -> light */

  var listeners = [];
  var tick = null;
  var applied = null;
  /* Fallback when localStorage throws (private mode): holds the choice for this page's
     lifetime only. No storage event fires without localStorage, so sibling iframes/tabs
     will NOT follow -- only this document keeps the choice, and only until reload. */
  var memPref = null;

  function saved() {
    var v = null;
    try { v = window.localStorage.getItem(KEY); } catch (e) { v = null; }
    if (v !== 'light' && v !== 'dark') v = memPref;
    return (v === 'light' || v === 'dark') ? v : null;
  }

  function osDark() {
    return !!(window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
  }

  /* Pure: given an optional hour (0-23), what should the theme be? */
  function resolve(hour) {
    var pref = saved();
    if (pref) return pref;
    if (osDark()) return 'dark';
    var h = (typeof hour === 'number') ? hour : new Date().getHours();
    return (h >= NIGHT_START || h < NIGHT_END) ? 'dark' : 'light';
  }

  function apply(theme) {
    document.documentElement.setAttribute('data-theme', theme);
    if (theme === applied) return;
    applied = theme;
    for (var i = 0; i < listeners.length; i++) {
      try { listeners[i](theme); } catch (e) { /* a bad listener must not break theming */ }
    }
  }

  /* The clock tick and the OS listener only matter while nothing is saved. */
  function watchAuto() {
    if (tick) { window.clearInterval(tick); tick = null; }
    if (saved()) return;
    tick = window.setInterval(function () { apply(resolve()); }, 60000);
  }

  function set(theme) {
    if (theme !== 'light' && theme !== 'dark') return;
    try { window.localStorage.setItem(KEY, theme); } catch (e) { memPref = theme; }
    watchAuto();
    apply(theme);
  }

  apply(resolve());
  watchAuto();

  if (window.matchMedia) {
    var mq = window.matchMedia('(prefers-color-scheme: dark)');
    var onOS = function () { if (!saved()) apply(resolve()); };
    if (mq.addEventListener) mq.addEventListener('change', onOS);
    else if (mq.addListener) mq.addListener(onOS);   /* Safari < 14 */
  }

  /* Sibling iframes and other tabs. Does NOT fire in the document that wrote. */
  window.addEventListener('storage', function (e) {
    if (e.key !== null && e.key !== KEY) return;   /* key===null means storage.clear() */
    watchAuto();
    apply(resolve());
  });

  window.rdzTheme = {
    effective: function () { return applied; },
    saved: saved,
    set: set,
    resolve: resolve,
    onChange: function (fn) { if (typeof fn === 'function') { listeners.push(fn); fn(applied); } }
  };
})();

/* The switch. Rendered only in a top-level document: inside index.html's
 * iframes the parent already shows one, and a second copy per frame would be
 * both wrong and duplicated. A page may provide #themeToggle to place it
 * (index.html does, in the nav); otherwise it is injected as a fixed-position
 * control so opening e.g. config.html directly still lets you switch.
 */
(function () {
  if (window.self !== window.top) return;

  var SUN = '<svg class="themetoggle-icon themetoggle-sun" viewBox="0 0 24 24" width="18" height="18" aria-hidden="true">'
          + '<circle cx="12" cy="12" r="4.2" fill="currentColor"/>'
          + '<g stroke="currentColor" stroke-width="1.8" stroke-linecap="round">'
          + '<path d="M12 2.6v2.4M12 19v2.4M2.6 12h2.4M19 12h2.4"/>'
          + '<path d="M5.4 5.4l1.7 1.7M16.9 16.9l1.7 1.7M18.6 5.4l-1.7 1.7M7.1 16.9l-1.7 1.7"/>'
          + '</g></svg>';
  var MOON = '<svg class="themetoggle-icon themetoggle-moon" viewBox="0 0 24 24" width="18" height="18" aria-hidden="true">'
           + '<path d="M20 14.5A8.6 8.6 0 0 1 9.5 4a8.5 8.5 0 1 0 10.5 10.5z" fill="currentColor"/>'
           + '</svg>';

  function build() {
    var btn = document.createElement('button');
    btn.type = 'button';
    btn.id = 'themeSwitch';
    btn.className = 'themetoggle';
    /* This is an ACTION button (icon shows what a click will do), not a two-position
       switch -- no role="switch"/aria-checked here. Those announce STATE ("Dark mode,
       switch, off"), which would contradict an icon that shows the opposite of the
       current state. aria-label is dynamic and equal to the action (same string as
       title), which is how icon toggles conventionally expose their state to
       assistive tech: the action implies the current state unambiguously. */
    btn.setAttribute('title', 'Switch to dark mode');
    btn.setAttribute('aria-label', 'Switch to dark mode');
    /* Both icons stay in the markup permanently; CSS shows one at a time off the
       is-dark class. Swapping innerHTML on every toggle would re-parse SVG on each
       click and drop any element state -- neither is needed since visibility is a
       pure function of is-dark. */
    btn.innerHTML = SUN + MOON;

    function sync(theme) {
      var isDark = theme === 'dark';
      btn.classList.toggle('is-dark', isDark);
      /* title/aria-label both name the ACTION (what a click does), not the current
         state -- kept identical so the tooltip and the accessible name agree. */
      var actionLabel = isDark ? 'Switch to light mode' : 'Switch to dark mode';
      btn.setAttribute('title', actionLabel);
      btn.setAttribute('aria-label', actionLabel);
    }
    btn.addEventListener('click', function () {
      window.rdzTheme.set(window.rdzTheme.effective() === 'dark' ? 'light' : 'dark');
    });
    window.rdzTheme.onChange(sync);
    return btn;
  }

  function mount() {
    var host = document.getElementById('themeToggle');
    var btn = build();
    if (host) { host.appendChild(btn); }
    else { btn.classList.add('themetoggle-floating'); document.body.appendChild(btn); }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', mount);
  } else {
    mount();
  }
})();
