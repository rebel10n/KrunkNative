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
// The asset host's assets/version.txt (deploy/ansible role kn_web) versions the
// /assets/ URLs (shared/src/utils.c web_asset_url).
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

  window.Module = Object.assign({
    canvas: document.getElementById('canvas'),
    // Override the game WebSocket endpoint; default is ws(s)://<host>/native, or ?ws=<url>.
    krunkNativeWsUrl: undefined,
    krunkNativeBridgeUrl: bridge || undefined, // net_web.c net_web_target_url
    krunkNativeBrainUrl: brain || undefined,
    krunkNativeWtUrl: wt || undefined, // net_wt.c
    krunkNativeRtcUrl: rtc || undefined, // net_rtc.c
    krunkNativeAssetBase: assetBase,
    krunkNativeAssetVersion: '',
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

  // The content version before main() runs; without one (no file, offline, slow host)
  // assets are fetched unversioned and revalidated instead.
  var started = false;
  function go(version) {
    if (started) return;
    started = true;
    if (version && /^[0-9A-Za-z._-]{1,64}$/.test(version)) window.Module.krunkNativeAssetVersion = version;
    start();
  }
  if (typeof fetch !== 'function') return go('');
  setTimeout(function () { go(''); }, 3000);
  fetch(assetBase + 'assets/version.txt', {cache: 'no-store'})
    .then(function (r) { return r.ok ? r.text() : ''; })
    .then(function (text) { go(String(text).trim()); }, function () { go(''); });
})();
