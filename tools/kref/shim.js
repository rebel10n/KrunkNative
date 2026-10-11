// Pointer-lock shim for headless Chromium: makes requestPointerLock() succeed
// synchronously so Krunker's pointerlockchange handler (controls.enabled=true ->
// enterGame()) runs exactly as it would in a real browser.
(function () {
  try { localStorage.setItem('krk_force_offline_sandbox','false'); } catch (e) {}
  var locked = null;
  try {
    Object.defineProperty(Document.prototype, 'pointerLockElement', { get: function () { return locked; }, configurable: true });
    Element.prototype.requestPointerLock = function () { locked = this; setTimeout(function(){ document.dispatchEvent(new Event('pointerlockchange')); }, 0); };
    Document.prototype.exitPointerLock = function () { locked = null; setTimeout(function(){ document.dispatchEvent(new Event('pointerlockchange')); }, 0); };
    window.__shimExitLock = function(){ document.exitPointerLock(); };
  } catch (e) { console.log('shim failed', e); }
})();
