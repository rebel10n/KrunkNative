// Web build only: loads the Emscripten client (client.js + client.wasm) when this page
// is served over HTTP. Opened from file:// it does nothing, so the bare page can still
// be previewed in a browser (without the C UI logic: no menu state, no windows). The
// native client never runs this file: its HTML engine ignores <script>.
//
// Deployment knobs are <meta> tags that deploy/pages/stage.py fills in (all empty in
// the plain build, which then behaves as before: everything next to the page):
//   kn-build       content hash: load client.<hash>.{js,wasm,data} (immutable files)
//   kn-asset-base  game content host, e.g. https://assets.krunk.io/ ("" = this origin)
//   kn-brain       account service / matchmaker URL ("" = this origin)
//   kn-bridge      net-bridge WebSocket endpoint the game connects through, e.g.
//                  wss://play.krunk.io/native ("" = ws(s)://<this host>/native); ?ws= wins
//   kn-rtc         WebRTC gateway signaling URL (net-bridge/rtc, doc/transport.md), e.g.
//                  https://play.krunk.io/rtc; "off" = no WebRTC; "" = fetch rtc.json next to
//                  the page (dev gateway); ?rtc= wins. Tried first, then kn-wt, then kn-bridge
//   kn-wt          WebTransport relay URL (net-bridge/wt, doc/transport.md), e.g.
//                  https://wt.krunk.io:8443/native; "off" = WebSocket only; "" = fetch wt.json
//                  next to the page (dev relay with a self-signed certificate hash)
//   kn-config      "" = none; else a JSON file next to the page (stage.py writes kn-config.json)
//                  read before boot, which picks the game content origin at run time:
//                    {"asset_origin": "cdn", "asset_fallback": "own",
//                     "asset_origins": {"cdn": "https://<project>.pages.dev/", "own": "https://assets.krunk.io/"}}
//                  asset_origin wins over kn-asset-base, so moving the content to another origin
//                  is a change of that file (a redeploy of the page, no client build);
//                  asset_fallback is where a failed download is retried (shared/src/utils.c);
//                  asset_split (deploy/asset-shards.json, stage.py): [{"base": URL, "routes":
//                  [[prefix, URL], ...]}]: on that base, a path below assets/ with one of the
//                  prefixes (longest first) loads from the route's URL instead (the second Pages
//                  project); Module.krunkNativeAssetBaseFor(path) answers it for utils.c
//   ?assets=<name> one of asset_origins for this tab (kept in sessionStorage across the site's
//                  page loads, ?assets=default forgets it); a full URL only on a localhost page
// deploy/pages/README.md "Asset origins". The chosen origin's assets/version.txt (deploy/ansible
// role kn_web, or the CDN copy) versions the /assets/ URLs (shared/src/utils.c web_asset_url).
(function () {
  if (location.protocol !== 'http:' && location.protocol !== 'https:') return;

  function meta(name) {
    var tag = document.querySelector('meta[name="' + name + '"]');
    return tag ? tag.getAttribute('content') || '' : '';
  }
  var build = meta('kn-build');
  var assetBase = meta('kn-asset-base');
  var brain = meta('kn-brain');
  var bridge = meta('kn-bridge');
  var wt = meta('kn-wt');
  var rtc = meta('kn-rtc');
  var config = meta('kn-config');
  var assetFallback = '';
  var assetRoutes = [];

  window.Module = Object.assign({
    canvas: document.getElementById('canvas'),
    // Override the game WebSocket endpoint; default is ws(s)://<host>/native, or ?ws=<url>.
    krunkNativeWsUrl: undefined,
    krunkNativeBridgeUrl: bridge || undefined, // net_web.c net_web_target_url
    krunkNativeBrainUrl: brain || undefined,
    krunkNativeWtUrl: wt || undefined, // net_wt.c
    krunkNativeRtcUrl: rtc || undefined, // net_rtc.c
    krunkNativeAssetBase: assetBase, // set again by go() once kn-config / ?assets= are read
    krunkNativeAssetFallback: '',
    krunkNativeAssetVersion: '',
    krunkNativeAssetRoutes: [],
    // The origin of a path below assets/ ("textures/x.png"): its split route, else the base.
    krunkNativeAssetBaseFor: function (rel) {
      var routes = window.Module.krunkNativeAssetRoutes || [];
      for (var i = 0; i < routes.length; i++)
        if (rel.lastIndexOf(routes[i][0], 0) === 0) return routes[i][1];
      return window.Module.krunkNativeAssetBase;
    },
    locateFile: function (path, prefix) {
      return prefix + (build ? path.replace(/^client\./, 'client.' + build + '.') : path);
    },
    onRuntimeInitialized: function () {
      window.__krunknative_wasm_ready = true; // kn-shim.js checks this before ccall()
    },
  }, window.Module || {});

  function start() {
    var script = document.createElement('script');
    script.src = build ? 'client.' + build + '.js' : 'client.js';
    script.async = true;
    script.onerror = function () {
      // No wasm next to the page (e.g. ui/ served on its own): leave the static page up.
      window.__krunknative_wasm_missing = true;
      delete window.Module;
      console.warn('[KrunkNative] client.js not found; running the page without the game');
    };
    document.body.appendChild(script);
  }

  // The game content origin: kn-config's asset_origin over kn-asset-base, ?assets= over both.
  function chooseAssets(cfg) {
    var origins = cfg && typeof cfg.asset_origins === 'object' && cfg.asset_origins || {};
    function url(u) { return typeof u === 'string' && /^https?:\/\/[^\s"'<>]+$/.test(u) ? u.replace(/\/?$/, '/') : ''; }
    function named(n) { return typeof n === 'string' && Object.prototype.hasOwnProperty.call(origins, n) ? url(origins[n]) : ''; }
    var local = /^(localhost|127\.0\.0\.1|\[::1\])$/.test(location.hostname);
    function pick(p) { return named(p) || (local ? url(p) : ''); }
    var base = named(cfg && cfg.asset_origin) || assetBase;
    try {
      var q = new URLSearchParams(location.search).get('assets');
      if (q === 'default') sessionStorage.removeItem('kn-assets');
      else if (q && pick(q)) sessionStorage.setItem('kn-assets', q);
      else if (q) console.warn('[KrunkNative] ?assets=' + q + ': not an asset origin of this site');
      base = pick(sessionStorage.getItem('kn-assets') || '') || base;
    } catch (e) {}
    var fallback = named(cfg && cfg.asset_fallback);
    assetBase = base;
    assetFallback = fallback !== base ? fallback : '';
    // the split of this base (deploy/asset-shards.json): prefix -> the other shard's origin
    var splits = cfg && Array.isArray(cfg.asset_split) ? cfg.asset_split : [];
    assetRoutes = [];
    for (var i = 0; i < splits.length; i++) {
      if (!splits[i] || url(splits[i].base) !== base || !Array.isArray(splits[i].routes)) continue;
      for (var j = 0; j < splits[i].routes.length; j++) {
        var r = splits[i].routes[j];
        if (Array.isArray(r) && typeof r[0] === 'string' && /^[A-Za-z0-9._\/-]+$/.test(r[0]) && url(r[1]))
          assetRoutes.push([r[0], url(r[1])]);
      }
    }
    assetRoutes.sort(function (a, b) { return b[0].length - a[0].length; });
  }

  // The content version before main() runs; without one (no file, offline, slow host)
  // assets are fetched unversioned and revalidated instead.
  var started = false;
  function go(version) {
    if (started) return;
    started = true;
    window.Module.krunkNativeAssetBase = assetBase;
    window.Module.krunkNativeAssetFallback = assetFallback;
    window.Module.krunkNativeAssetRoutes = assetRoutes;
    if (version && /^[0-9A-Za-z._-]{1,64}$/.test(version)) window.Module.krunkNativeAssetVersion = version;
    start();
  }
  function version() {
    setTimeout(function () { go(''); }, 3000);
    fetch(assetBase + 'assets/version.txt', {cache: 'no-store'})
      .then(function (r) { return r.ok ? r.text() : ''; })
      .then(function (text) { go(String(text).trim()); }, function () { go(''); });
  }
  if (typeof fetch !== 'function') { chooseAssets(null); return go(''); }
  if (!config) { chooseAssets(null); return version(); }
  var configured = false;
  function configure(cfg) {
    if (configured) return;
    configured = true;
    chooseAssets(cfg);
    version();
  }
  setTimeout(function () { configure(null); }, 2000);
  fetch(config, {cache: 'no-cache'})
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(configure, function () { configure(null); });
})();
