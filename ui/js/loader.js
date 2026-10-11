// Web build only: loads the Emscripten client (client.js + client.wasm) when this page
// is served over HTTP. Opened from file:// it does nothing, so the bare page can still
// be previewed in a browser (without the C UI logic: no menu state, no windows). The
// native client never runs this file: its HTML engine ignores <script>.
(function () {
  if (location.protocol !== 'http:' && location.protocol !== 'https:') return;

  window.Module = Object.assign({
    canvas: document.getElementById('canvas'),
    // Override the game WebSocket endpoint; default is ws(s)://<host>/native, or ?ws=<url>.
    krunkNativeWsUrl: undefined,
    onRuntimeInitialized: function () {
      window.__krunknative_wasm_ready = true; // kn-shim.js checks this before ccall()
    },
  }, window.Module || {});

  var script = document.createElement('script');
  script.src = 'client.js';
  script.async = true;
  script.onerror = function () {
    // No wasm next to the page (e.g. ui/ served on its own): leave the static page up.
    window.__krunknative_wasm_missing = true;
    delete window.Module;
    console.warn('[KrunkNative] client.js not found; running the page without the game');
  };
  document.body.appendChild(script);
})();
