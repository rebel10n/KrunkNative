/*
 * KrunkNative web glue (Emscripten build only; the native client runs no JavaScript and
 * its engine ignores <script>). Counterpart of client/src/html/web_dom.c; contract in
 * doc/html-ui-engine.md. No UI logic lives here: every inline handler in the Krunker
 * markup becomes a ccall into C (kn_ui_event), the few non-inline listeners are
 * forwarded the same way, and C drives the DOM back through knId()/knEl().
 *
 * Always defined: knId, knEl, knObserve, knStorageGet, knStorageSet, knCall. The page's
 * handler globals (setSetting, showWindow, windows[n].*, ...) and listeners are installed
 * by knActivate(), which C calls from html_ui_backend_init() once the wasm is running.
 */
(function (global) {
  'use strict';

  /* ------------------------------------------------------------ element ids */
  var nodes = [null];                       // id -> node (strong refs: detached elements survive)
  var ids = new WeakMap();                  // node -> id
  function knId(node) {
    if (!node || typeof node !== 'object' || !node.nodeType) return 0;
    var id = ids.get(node);
    if (!id) {
      id = nodes.length;
      nodes.push(node);
      ids.set(node, id);
      if (node.nodeType === 1) node.setAttribute('data-kn-id', String(id));
    }
    return id;
  }
  function knEl(id) { return nodes[id | 0] || null; }

  /* ------------------------------------------------------------ resize observer */
  var ro = null;
  function knObserve(id, on) {
    var el = knEl(id);
    if (!el || el.nodeType !== 1 || !global.ResizeObserver) return;
    if (!ro) {
      ro = new ResizeObserver(function (entries) {
        if (!engineReady()) return;
        for (var i = 0; i < entries.length; i++) {
          try { global.Module.ccall('kn_ui_resize_observed', null, ['number'], [knId(entries[i].target)]); }
          catch (e) { console.error('[kn] resize_observed failed', e); }
        }
      });
    }
    if (on === false) ro.unobserve(el); else ro.observe(el);
  }

  /* ------------------------------------------------------------ storage */
  function knStorageGet(k) { try { return global.localStorage.getItem(k); } catch (e) { return null; } }
  function knStorageSet(k, v) {
    try { if (v === null || v === undefined) global.localStorage.removeItem(k); else global.localStorage.setItem(k, String(v)); }
    catch (e) { /* private mode / quota */ }
  }

  /* ------------------------------------------------------------ page -> C */
  var active = false;
  function engineReady() {
    var m = global.Module;
    return !!(m && typeof m.ccall === 'function' && typeof m._kn_ui_event === 'function' &&
              (m.calledRun || global.__krunknative_wasm_ready));
  }

  function serialise(args) {
    var out = [];
    for (var i = 0; i < args.length; i++) {
      var a = args[i];
      if (a === null || a === undefined) out.push(null);
      else if (typeof a === 'string' || typeof a === 'number' || typeof a === 'boolean') out.push(a);
      else if (a.nodeType) out.push({ kn: 'this', id: knId(a) });
      else if (typeof Event !== 'undefined' && a instanceof Event) out.push({ kn: 'event' });
      else out.push(String(a));
    }
    return JSON.stringify(out);
  }

  // handler: "<type>|<name>" (inline) or "listener:<elemId>:<type>". Returns false when C
  // returned 0 (= `return false` in the original handler).
  function send(handler, argsJson, el, ev, valueOverride) {
    if (!engineReady()) return true;
    var target = el && el.nodeType ? el : (ev && (ev.currentTarget && ev.currentTarget.nodeType === 1 ? ev.currentTarget : ev.target));
    var value = valueOverride != null ? String(valueOverride) : (target && target.value != null ? String(target.value) : '');
    var checked = target && target.checked ? 1 : 0;
    var key = ev && /^key/.test(ev.type || '') ? (ev.keyCode || ev.which || 0) : 0;
    var button = ev && typeof ev.button === 'number' ? ev.button : 0;
    // key events carry no position: x = 1 tells C that Shift is held (the console's Shift+Tab)
    var x = ev && typeof ev.clientX === 'number' ? ev.clientX : (ev && ev.shiftKey ? 1 : 0);
    var y = ev && typeof ev.clientY === 'number' ? ev.clientY : 0;
    try {
      var r = global.Module.ccall('kn_ui_event', 'number',
        ['string', 'string', 'number', 'string', 'number', 'number', 'number', 'number', 'number'],
        [handler, argsJson, knId(target), value, checked, key, button, x, y]);
      // bit 1: C asked for event.stopPropagation() (the native engine stops the ancestors' handlers)
      if ((r & 2) && ev && ev.stopPropagation) ev.stopPropagation();
      return (r & 1) !== 0;
    } catch (e) {
      console.error('[kn] kn_ui_event failed for ' + handler, e);
      return true;
    }
  }

  function knCall(name, args) {
    var ev = global.event || null;
    var type = ev && ev.type ? ev.type : 'click';
    return send(type + '|' + name, serialise(args), null, ev);
  }
  function knListen(elemId, type, el, ev) { return send('listener:' + elemId + ':' + type, '[]', el, ev); }
  function forward(name) { return function () { return knCall(name, arguments); }; }

  var NAMES = ['setSetting', 'delayExecute', 'showWindow', 'selectClass', 'playTick', 'openURL', 'openTab',
    'openHostWindow', 'openJoinWindow', 'copyInviteLink', 'quickJoinRegion', 'joinGame', 'createPrivateRoom',
    'registerAcc', 'loginAcc', 'resetSettings', 'exportSettings', 'importSettings', 'playMap', 'selectHostMap',
    'favoriteM', 'selectStat', 'skinSelector', 'selectSkinColor', 'changeCont', 'unbindCont', 'changeContSet',
    'updateWSliderLabel', 'updateSliderLabel', 'aspectSelect', 'sub2FA', 'bdayPlay', 'checkTerms', 'clearPops',
    'toggleTeam', 'toggleSpect', 'claimReward', 'shoPolicePop', 'focusChat', 'sendReaction', 'closeTwitch',
    'toggleHF', 'setExpandedRegion', 'selectSettingPre', 'knRetry', 'knHostOffline', 'knHostMap',
    'selectSkin', 'selectHat', 'selectBack', 'selectWaist', 'selectMelee', 'selectDye', 'selectFace',
    'selectAttachment', 'selectReticle', 'selectSpray', 'hidePolicePop', 'updateWindow',
    // Krunker v7.1.4 menu chrome (index.html; knForwardHandlers also finds them in the markup)
    'playSelect', 'logBtnFRVR', 'closWind', 'showGames', 'openDiscord', 'openGuide', 'setSpect', 'purchaseCancel',
    'openServerWindow', 'clientExitPopup', 'abTestsPopup', 'expertMode', 'switchChat', 'goBackWindow',
    'checkedSwitchServer', 'setExpandedRegion', 'finderJoin', 'openConnectWindow', 'connectServer'];
  var WINDOW_METHODS = ['changeTab', 'searchList', 'collapseFolder', 'switchTab', 'action', 'toggle',
    'presetAction', 'updateSet', 'changeWep', 'toggleWep', 'premium', 'purchase', 'toggleLog', 'searchMaps',
    'toggleType', 'loadMore', 'openHome', 'viewAll', 'readAll', 'craftMenu', 'newPop', 'toggleEffects', 'changeEffects'];

  function isField(t) { return !!(t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable)); }
  function isEscOrEnter(e) { return e.key === 'Escape' || e.keyCode === 27 || e.key === 'Enter' || e.keyCode === 13; }
  // Fields with data-kn-keys (the console input) also hand Tab, Up/Down and ` to C.
  function isFieldKey(e) {
    var k = e.keyCode;
    return !!(e.target && e.target.hasAttribute && e.target.hasAttribute('data-kn-keys')) && (k === 9 || k === 38 || k === 40 || k === 192);
  }

  function installListeners() {
    var holder = document.getElementById('instructionHolder');
    if (holder) holder.addEventListener('mousedown', function (e) {
      if (!knListen('instructionHolder', 'mousedown', e.target, e)) e.preventDefault();
    });
    var wholder = document.getElementById('windowHolder');
    if (wholder) wholder.addEventListener('mousedown', function (e) {
      if (!knListen('windowHolder', 'mousedown', e.target, e)) e.preventDefault();
    });
    // Never navigate away: every anchor click is cancelled and reported with the <a> as target.
    document.addEventListener('click', function (e) {
      var a = e.target && e.target.closest ? e.target.closest('a') : null;
      if (!a) return;
      e.preventDefault();
      knListen('', 'click', a, e);
    });
    // Keydown in fields: see the window capture listener installed at load time below.
    // Enter keyup in a field with an id goes to C's addEventListener('keyup') stand-in
    // (social searchPInput / chatBoxInput).
    document.addEventListener('keyup', function (e) {
      if (!isField(e.target)) return;
      e.stopPropagation();
      if ((e.key === 'Enter' || e.keyCode === 13) && e.target.id) knListen(e.target.id, 'keyup', e.target, e);
    }, true);
    // Escape/Enter anywhere else: C decides (close window / toggle menu / ...).
    global.addEventListener('keydown', function (e) {
      if (!isEscOrEnter(e)) return;
      if (!knListen('', 'keydown', e.target, e) || e.keyCode === 27 || e.key === 'Escape') e.preventDefault();
    });
    global.addEventListener('resize', function (e) { knListen('', 'resize', null, e); });
  }

  function knActivate() {
    if (active) return true;
    if (!engineReady()) return false;
    active = true;
    for (var i = 0; i < NAMES.length; i++) global[NAMES[i]] = forward(NAMES[i]);
    global.SOUND = { play: forward('SOUND.play') };
    // window.open('/social.html', '_blank') & co.: C decides (Krunker pages ported into the
    // client open in place, anything else in a tab through knRealOpen, ui_dialog.c).
    if (!global.knRealOpen) {
      global.knRealOpen = global.open.bind(global);
      global.open = forward('window.open');
    }
    // Krunker v7.1.4's windows[0..51]. Each is a Proxy: methods are forwarders (installed
    // here and by knForwardHandlers), and a plain property assignment in the markup
    // (`windows[13].subTab=0`, `windows[20].tabIndex=1`) reaches C as the handler
    // "windows.13.subTab=" with the value, the name the native engine dispatches it under.
    var wins = [];
    function windowProxy(w) {
      var obj = {};
      for (var m = 0; m < WINDOW_METHODS.length; m++) obj[WINDOW_METHODS[m]] = forward('windows.' + w + '.' + WINDOW_METHODS[m]);
      if (typeof Proxy !== 'function') return obj;
      return new Proxy(obj, {
        set: function (target, prop, value) {
          target[prop] = value;
          if (typeof value !== 'function' && typeof prop === 'string') knCall('windows.' + w + '.' + prop + '=', [value]);
          return true;
        }
      });
    }
    for (var w = 0; w < 52; w++) wins.push(windowProxy(w));
    global.windows = wins;
    // The markup's `window.canShowExternalLinks ? openTab(url) : showWindow(47)`: both
    // builds open external links in a browser tab (C answers the same natively).
    global.canShowExternalLinks = true;
    installListeners();
    knForwardHandlers(document.body); // handler names of this page (social/editor/viewer markup)
    console.info('[kn] kn-shim active: inline handlers and listeners forward to kn_ui_event');
    return true;
  }

  /* ------------------------------------------------------------ input that must beat GLFW
   * Installed at load time, i.e. before client.js runs glfwInit(), so these window capture
   * listeners run ahead of GLFW's own (window capture keydown; canvas-only mouse/wheel).
   * They do nothing until knActivate(). */
  var gameCanvas = null;
  function isCanvas(t) { return t && t === (gameCanvas || (gameCanvas = document.getElementById('canvas'))); }

  // GLFW's keydown preventDefault()s Backspace and Tab, so they never edit a field: keys
  // typed into a field (chat, name, searches, number inputs) are kept away from GLFW and
  // from the game entirely. Escape/Enter in a field go to C with the field as target (C
  // blurs / sends the chat / commits the name). keypress is left alone (inline
  // onkeypress="return delayExecute(...)" handlers) and so is keyup (GLFW must see the
  // release of a key that was held before the field took focus).
  global.addEventListener('keydown', function (e) {
    if (!active || !isField(e.target)) return;
    e.stopImmediatePropagation();
    if ((isEscOrEnter(e) || isFieldKey(e)) && !knListen('', 'keydown', e.target, e)) e.preventDefault();
    // The propagation stop above also keeps the field's own inline onkeydown from running
    // (`if(event.keyCode==13)connectServer()`, the Custom Games search): run it here, as the
    // browser would have (window.event is still this event), unless C took the key.
    if (!e.defaultPrevented && typeof e.target.onkeydown === 'function') {
      try { if (e.target.onkeydown.call(e.target, e) === false) e.preventDefault(); } catch (x) { console.error('[kn] onkeydown failed', x); }
    }
  }, true);

  // controls.js keydown/keyup: e.preventDefault() while CONTROLS.enabled (the game owns
  // the input). These bubble listeners run after GLFW's window capture listener, which has
  // already taken the key through C; C answers whether the game owned it. A key whose
  // keydown was prevented gets its keyup prevented too: pListKey (Alt) frees the pointer
  // on keydown, and a bare Alt release would otherwise focus the browser's menu bar
  // (Firefox, Chrome on Windows) and keep the keyboard away from the page.
  var gameKeys = {};
  function gameKey() {
    if (!active || !engineReady() || !global.Module._kn_ui_key_prevent) return false;
    try { return global.Module._kn_ui_key_prevent() !== 0; } catch (x) { return false; }
  }
  global.addEventListener('keydown', function (e) {
    if (!gameKey()) return;
    e.preventDefault();
    gameKeys[e.keyCode] = 1;
  });
  global.addEventListener('keyup', function (e) {
    if (gameKey() || gameKeys[e.keyCode]) e.preventDefault();
    delete gameKeys[e.keyCode];
  });
  global.addEventListener('blur', function () { gameKeys = {}; });

  // Side buttons: never let the browser navigate back/forward (M4/M5 are game binds).
  function noNavigate(e) { if (active && (e.button === 3 || e.button === 4)) e.preventDefault(); }
  global.addEventListener('mouseup', noNavigate, true);
  global.addEventListener('auxclick', noNavigate, true);
  // Middle/side buttons and the wheel over the page (GLFW only sees the canvas) go to C for
  // the controls window's "Press any Key" capture; C answers 0 when it consumed the event.
  // Pointer glue for elements marked data-kn-pointer (the dev console modal, mod bind rows):
  // the left button's down/move/up go to C as window listeners knpointer*; when C takes the
  // press (returns 0) the default is prevented and moves/up follow it until release.
  // data-kn-pointer="2" (the editor page): moves/up always follow the press, C's answer
  // only decides preventDefault (fields under it must still take focus).
  var knPointer = false;
  function inPointerZone(t) { return !!(t && t.closest && t.closest('[data-kn-pointer]')); }
  function followZone(t) { var z = t && t.closest && t.closest('[data-kn-pointer]'); return !!(z && z.getAttribute('data-kn-pointer') === '2'); }
  global.addEventListener('mousemove', function (e) {
    if (active && knPointer) knListen('', 'knpointermove', e.target, e);
  }, true);
  global.addEventListener('mouseup', function (e) {
    if (!active || !knPointer || e.button !== 0) return;
    knPointer = false;
    if (!knListen('', 'knpointerup', e.target, e)) e.preventDefault();
  }, true);
  global.addEventListener('mousedown', function (e) {
    if (!active) return;
    noNavigate(e);
    if (e.button === 0 && inPointerZone(e.target)) {
      var follow = followZone(e.target);
      if (!knListen('', 'knpointerdown', e.target, e)) { e.preventDefault(); knPointer = true; }
      else if (follow) knPointer = true;
      return;
    }
    if (e.button === 0 || e.button === 2 || isCanvas(e.target)) return;
    if (!knListen('', 'mousedown', e.target, e)) e.preventDefault();
  }, true);
  global.addEventListener('wheel', function (e) {
    if (!active || isCanvas(e.target) || !e.deltaY) return;
    if (!send('listener::wheel', '[]', e.target, e, e.deltaY < 0 ? 1 : -1)) e.preventDefault();
  }, { capture: true, passive: false });

  /* ------------------------------------------------------------ generic forwarders
   * The other pages (social.html, editor.html, viewer.html; each its own URL, loading this
   * shim and the same client.js) and markup C inserts later call handler names the
   * static NAMES list does not know: every call in an on* attribute (`updateWindow(..)`,
   * `windows[0].switchTab(1)`, `if (...) window.searchProfile();`) gets a forwarder unless
   * the name is a real browser global. Names are normalised like html_dom_core.c. */
  var KEYWORDS = { 'if': 1, 'return': 1, 'typeof': 1, 'function': 1, 'new': 1, 'this': 1, 'event': 1,
    'window': 1, 'document': 1, 'location': 1, 'console': 1, 'Math': 1, 'parseInt': 1, 'parseFloat': 1,
    'String': 1, 'Number': 1, 'alert': 1, 'confirm': 1, 'open': 1, 'setTimeout': 1 };
  var CALL_RE = /(^|[^\w$.])((?:window\.)?[A-Za-z_$][\w$]*(?:\s*\.\s*[A-Za-z_$][\w$]*|\[\d+\])*)\s*\(/g;
  function defineForwarder(path) {
    var name = path.replace(/^window\./, '').replace(/\s+/g, '');
    var parts = name.replace(/\[(\d+)\]/g, '.$1').split('.');
    if (!parts[0] || KEYWORDS[parts[0]]) return;
    var obj = global, i;
    for (i = 0; i < parts.length - 1; i++) {
      var next = obj[parts[i]];
      if (next === undefined || next === null) {
        next = /^\d+$/.test(parts[i + 1]) ? [] : {};
        try { Object.defineProperty(next, '__kn', { value: true }); } catch (e) {}
        obj[parts[i]] = next;
      } else if (typeof next !== 'object' && typeof next !== 'function') return;
      else if (obj === global && !next.__kn && next !== global.windows && next !== global.SOUND) return; // a browser object
      obj = next;
    }
    var last = parts[parts.length - 1];
    if (typeof obj[last] === 'function' && (obj[last].__kn || obj !== global)) return;
    if (obj === global && obj[last] !== undefined) return; // never shadow browser globals
    var fn = forward(parts.join('.'));
    try { Object.defineProperty(fn, '__kn', { value: true }); } catch (e) {}
    obj[last] = fn;
  }
  function forwardCode(code) {
    if (!code) return;
    CALL_RE.lastIndex = 0;
    var m;
    while ((m = CALL_RE.exec(code))) defineForwarder(m[2]);
  }
  function knForwardHandlers(root) {
    if (!active || !root || root.nodeType !== 1) return;
    var list = [root].concat(Array.prototype.slice.call(root.querySelectorAll('*')));
    for (var i = 0; i < list.length; i++) {
      var at = list[i].attributes;
      for (var j = 0; at && j < at.length; j++) if (/^on/i.test(at[j].name)) forwardCode(at[j].value);
    }
  }

  global.knId = knId;
  global.knEl = knEl;
  global.knObserve = knObserve;
  global.knStorageGet = knStorageGet;
  global.knStorageSet = knStorageSet;
  global.knCall = knCall;
  global.knActivate = knActivate;
  global.knForwardHandlers = knForwardHandlers;
})(window);
