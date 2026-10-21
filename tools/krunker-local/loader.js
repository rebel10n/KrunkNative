// Sketch loader (tools/krunker-local): injected into the official index.html in place of its WASM loader
// module (sketchPage in krunker-local.mjs). One generate-token GET, the renamed globals, then the
// unmodified bundle (/game.js, copied from krunkerSrc) run as a function of (token, callback). No WASM
// loader, no splits. The two parameter names and the renamed global are identifiers of the build in
// krunkerSrc: they change with each build and must be re-found from the bundle when the page stops working.
(function () {
  var PARAM_TOKEN = "b475796ed633d5fd0485";
  var PARAM_CALLBACK = "f9e2c8d63a4b7128b031";
  var RENAMED = { JfCzGzvGIQB8rrJX: "setTimeout" };
  function log(m) { try { console.log("[local] " + m); } catch (e) {} }
  async function run() {
    var t = await fetch("https://matchmaker.krunker.io/generate-token");
    if (!t.ok) throw new Error("generate-token -> HTTP " + t.status);
    var token = (await t.text()).trim();
    log("token " + token.length + " chars");
    Object.keys(RENAMED).forEach(function (name) { window[name] = window[RENAMED[name]]; });
    var src = await (await fetch("/game.js")).text();
    log("game.js " + src.length + " chars");
    var fn = new Function(PARAM_TOKEN, PARAM_CALLBACK, src + "\n//# sourceURL=https://krunker.io/js/app.js");
    log("executing");
    fn.apply(window, [token, undefined]);
    log("returned");
  }
  function go() { run().catch(function (e) { log("FAILED " + (e && e.stack || e)); }); }
  if (document.readyState === "complete") go(); else window.addEventListener("load", go, { once: true });
})();
