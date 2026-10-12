#!/usr/bin/env node
// Krunker v7.1.4 map editor reference captures (doc/v7/editor.md "Reference capture", WP0).
//
//   node tools/editor/v7ref.js [--out doc/ref/krunker-v7.1.4/editor] [--list] [state...]
//
// Opens v7's own public/editor.html (served read-only from the reference tree; v7's npm server is
// never run) in headless Chromium at 1920x937 and scripts states through Runtime.evaluate on
// KE / GUI / window.windows / showWindow / closeWindow. Per state it writes, into --out:
//   <state>.png          the page (1920x937)
//   <state>.rects.json   (editor-1920 and the windows) viewport rects of the chrome, for gen_scenarios.py
//   <state>.dump.txt     a structural dump (format below; identical to the native editor's
//                        KRUNKNATIVE_EDITOR_SCRIPT="dump:<which>:<file>", client/src/editor/editor.c)
//   editor-1920.{png,html.gz,rects.json}   the editor with the Welcome window closed (uidiff ref)
//   prefabs/<id>.json, prefabs/<id>.right.dump.txt   KE.createObjects[id]()'s serialize() and
//                        right (object) panel, for every prefab the menus can create (captured
//                        without public/js/hook.js, as `logic`: see reloadPage)
//   inspector-edits.json  the objects after tools/editor/inspector_edits.json's panel edits (WP3)
//   right-object-cube.{png,rects.json,dump.txt}  a new cube's object panel, Transform + Style open (WP3)
//   scripting*.{png,dump.txt}   the KrunkScript window (scripting.html, lost from the tree:
//                        tools/editor/fetch_scripting_ref.sh recovers it into --scripting DIR,
//                        default ~/.cache/krunknative/scripting-ref) opened by v7's own
//                        KE.scriptPopup(), its window.open turned into an iframe at the same
//                        place (10% / 10%, 80% x 80%: KrunkNative's in-page window). Dump format
//                        of client/src/editor/editor_script.c ed_script_dump: H label, T tabs
//                        (" *" selected, [E] / [V] / [C] icons), B status button, C shown tab (lines,
//                        font), E error-marked lines, K hint rows ("* " = active), S selection.
// Determinism: no network but the static serve (https/wss blocked), empty localStorage (default kro_editor settings), a seeded Math.random before any
// page script, the hook's session timer box removed, CSS animations paused at 800 ms, the Welcome
// counters run to their maximum. The page is served on a free 127.0.0.1 port, so v7's
// config.isProd is true (as on krunker.io): localOnly prefabs (EVENT, TERMINAL, BOT_SPAWNER) are
// neither listed nor created, and the Event Objects counter stays hidden.
//
// Dump format. text(e): textContent cut to 511 chars, whitespace runs collapsed to one space,
// trimmed, then cut to the native buffer size (see LIM). attr missing -> "null". Walk the element
// children c of the root:
//   panelLabel -> "P <text>"; header|subheader|subsubheader -> "F|Fs|Fss <text(a)> open|closed";
//   inlineItem -> "L[<.section classes minus section>] <text(.section)> : <controls>" (controls:
//   input, select, .button / .eButton / .dbutton (v7's buttons) and .inlineLabel, in page order);
//   vector3 -> "V v/min,v/min,v/min"; hr -> "-"; section -> "S <text>";
//   div with one element child .button or .eButton -> "B <text>";
//   window mode only: menuTabNew|menuShortTabNew -> "T <text>[ *]"; input|select -> "I <control>";
//   leaf with text -> "X <text>"; otherwise "X <own text nodes>" when non-empty;
//   everything else: recurse.
// Window dumps start with "H <text(#windowHeader)>". The toolbar dump is a menu tree:
// "M <label>" (sub-menu, children indented by 2), "C" (a 12-row column), "O <item>", "-".
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const net = require('net');
const zlib = require('zlib');
const { V7_PUBLIC, staticServer, launchChrome, sleep } = require('../ui-import/v7_page.js');
const { CDP } = require('../kref/cdp.js');

const REPO = path.resolve(__dirname, '..', '..');
let OUT = path.join(REPO, 'doc', 'ref', 'krunker-v7.1.4', 'editor');
let SCRIPTING = path.join(process.env.XDG_CACHE_HOME || path.join(os.homedir(), '.cache'), 'krunknative', 'scripting-ref');
const scriptingPage = () => fs.existsSync(path.join(SCRIPTING, 'scripting.html'));

// The reference tree plus the recovered scripting page (/scripting.html, /css/scripting.css,
// /kn-cm/... CodeMirror) when it is there. GET/HEAD only; never writes.
const MIME = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css' };
function overlayServer() {
  const http = require('http');
  return new Promise((resolve) => {
    let inner = null;
    const server = http.createServer((req, res) => {
      const rel = decodeURIComponent(req.url.split('?')[0].split('#')[0]);
      const mine = rel === '/scripting.html' || rel === '/css/scripting.css' || rel.startsWith('/kn-cm/');
      const file = path.join(SCRIPTING, path.normalize(rel).replace(/^(\.\.[/\\])+/, ''));
      if (!mine || !file.startsWith(SCRIPTING) || (req.method !== 'GET' && req.method !== 'HEAD') || !fs.existsSync(file)) {
        // everything else: the reference tree's own server
        const prox = http.request({ host: '127.0.0.1', port: inner.address().port, path: req.url, method: req.method }, (r) => { res.writeHead(r.statusCode, r.headers); r.pipe(res); });
        prox.on('error', () => { res.writeHead(502); res.end(); });
        return req.pipe(prox);
      }
      const st = fs.statSync(file);
      res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Content-Length': st.size, 'Cache-Control': 'no-store' });
      if (req.method === 'HEAD') return res.end();
      fs.createReadStream(file).pipe(res);
    });
    staticServer(V7_PUBLIC, 0).then((s) => { inner = s; server.on('close', () => s.close()); server.listen(0, '127.0.0.1', () => resolve(server)); });
  });
}
const W = 1920, H = 937;
const ANIM_MS = 800;

function freePort() {
  return new Promise((res, rej) => { const s = net.createServer(); s.on('error', rej);
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
}

// Seeded Math.random (mulberry32) before any page script (as tools/uidiff/capture-v7.js), and
// v7's decompiled requestAnimationFrame(fn, delay) run as setTimeout (GUI.update.toolbar uses it).
const SEED_RNG = `(() => { let a = 0x4b4e; Math.random = function () { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a);
  t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };
  const raf = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = function (fn, delay) { return typeof delay === 'number' ? setTimeout(fn, delay) : raf(fn); };
  window.alert = function (m) { (window.__knAlerts = window.__knAlerts || []).push(String(m)); };
  window.confirm = function () { return true; };
  window.prompt = function () { return null; };
})();`;

// the hook's "00:00:07" session timer box, CSS animations paused at ANIM_MS
const TIDY = `(() => { for (const d of document.querySelectorAll('body > div')) if (!d.id && /^\\d\\d:\\d\\d:\\d\\d$/.test(d.textContent.trim())) d.remove();
  let n = 0; for (const a of document.getAnimations()) { try { a.pause(); a.currentTime = ${ANIM_MS}; n++; } catch (e) {} } return n; })()`;

// ---------------------------------------------------------------- the dump (page side)

const DUMP_LIB = `(() => {
  // native buffer sizes (client/src/editor/editor.c dump_walk / dump_ctls): output <= n - 1 chars
  const collapse = (raw, n) => { raw = (raw || '').slice(0, 511); let o = '', sp = true;
    for (const ch of raw) { if (o.length + 1 >= n) break; const ws = ch === ' ' || ch === '\\n' || ch === '\\t' || ch === '\\r';
      if (ws) { if (!sp) o += ' '; sp = true; } else { o += ch; sp = false; } }
    return o.replace(/ +$/, ''); };
  const text = (e, n) => e ? collapse(e.textContent, n) : '';
  const attr = (e, name, n) => { const v = e.getAttribute(name); return v === null ? 'null' : v.slice(0, n - 1); };
  const has = (e, c) => e.classList.contains(c);
  const tag = (e) => e.tagName.toLowerCase();
  function ctl(c) {
    const t = tag(c);
    if (t === 'input') {
      let type = c.getAttribute('type'); type = type === null ? 'text' : type.slice(0, 31);
      const v = attr(c, 'value', 256);
      if (type === 'checkbox') return 'switch=' + (c.checked ? 1 : 0);
      if (type === 'range') return 'range[' + attr(c, 'min', 32) + ',' + attr(c, 'max', 32) + ',' + attr(c, 'step', 32) + ']=' + v;
      if (has(c, 'sliderVal')) return 'sval=' + v;
      if (has(c, 'color')) return 'color=' + v;
      const ph = c.getAttribute('placeholder');
      return type + '(' + (ph === null ? '' : ph.slice(0, 63)) + ')=' + v;
    }
    if (t === 'select') {
      const o = [...c.querySelectorAll('option')].slice(0, 4096);
      const t1 = o.length ? text(o[0], 128) : '', t2 = o.length ? text(o[o.length - 1], 128) : '';
      const cur = (c.value || '').slice(0, 127);
      let sel = '';
      for (const op of o) { const ov = (op.getAttribute('value') || '').slice(0, 127); if (ov === cur) { sel = text(op, 128); break; } }
      return 'select#' + o.length + '[' + t1 + '..' + t2 + ']=' + sel;
    }
    if (has(c, 'button') || has(c, 'eButton') || has(c, 'dbutton')) return 'button:' + text(c, 128);
    return 'label:' + text(c, 128);
  }
  function walk(e, win, out) {
    for (const c of e.children) {
      const t = tag(c);
      if (has(c, 'panelLabel')) out.push('P ' + text(c, 256));
      else if (has(c, 'header') || has(c, 'subheader') || has(c, 'subsubheader')) {
        const icon = text(c.querySelector('i'), 64);
        out.push('F' + (has(c, 'header') ? '' : has(c, 'subheader') ? 's' : 'ss') + ' ' + text(c.querySelector('a'), 256) + ' ' +
          (icon.length >= 5 && icon.endsWith('right') ? 'closed' : 'open'));
      } else if (has(c, 'inlineItem')) {
        const sec = c.querySelector('.section');
        let sub = '', st = '';
        if (sec) { st = text(sec, 256); sub = (sec.getAttribute('class') || '').slice(0, 95).split(' ').filter((x) => x && x !== 'section').join(' '); }
        out.push('L[' + sub + '] ' + st + ' : ' + [...c.querySelectorAll('input, select, .button, .eButton, .dbutton, .inlineLabel')].slice(0, 64).map(ctl).join(' '));
      } else if (has(c, 'vector3')) {
        out.push('V ' + [...c.querySelectorAll('input')].slice(0, 3).map((i) => attr(i, 'value', 64) + '/' + attr(i, 'min', 32)).join(','));
      } else if (t === 'hr') out.push('-');
      else if (has(c, 'section')) out.push('S ' + text(c, 256));
      else if (t === 'div' && c.children.length === 1 && (has(c.children[0], 'button') || has(c.children[0], 'eButton'))) out.push('B ' + text(c.children[0], 256));
      else if (win && (has(c, 'menuTabNew') || has(c, 'menuShortTabNew'))) out.push('T ' + text(c, 256) + (has(c, 'tabANew') ? ' *' : ''));
      else if (win && (t === 'input' || t === 'select')) out.push('I ' + ctl(c));
      else if (win && !c.children.length) { const x = text(c, 256); if (x) out.push('X ' + x); }
      else {
        if (win) { let own = ''; for (const n of c.childNodes) if (n.nodeType === 3) own += n.nodeValue; own = collapse(own, 256); if (own) out.push('X ' + own); }
        walk(c, win, out);
      }
    }
    return out;
  }
  function toolbar(e, ind, out) {
    for (const c of e.children) {
      const t = tag(c);
      const drop = [...c.children].find((x) => has(x, 'toolbarDropDown') || has(x, 'toolbarSubDropDown'));
      if ((has(c, 'toolbarItem') || has(c, 'toolbarDropDownItem')) && drop) {
        let own = ''; for (const n of c.childNodes) if (n.nodeType === 3) own += n.nodeValue;
        out.push(ind + 'M ' + collapse(own, 256));
        toolbar(drop, ind + '  ', out);
      } else if (has(c, 'toolbarChunkSubDropDown')) { out.push(ind + 'C'); toolbar(c, ind, out); }
      else if (has(c, 'toolbarDropDownItem')) out.push(ind + 'O ' + text(c, 256));
      else if (t === 'hr') out.push(ind + '-');
      else toolbar(c, ind, out);
    }
    return out;
  }
  const lines = (a) => a.map((l) => l + '\\n').join('');
  window.__knDump = {
    panel: (side) => lines(walk(document.querySelector('.' + side + '.panel .list'), false, [])),
    window: () => lines(['H ' + text(document.getElementById('windowHeader'), 256)].concat(walk(document.getElementById('menuWindow'), true, []))),
    popup: () => lines(['H '].concat(walk(document.getElementById('genericPop'), true, []))),
    toolbar: () => lines(toolbar(document.getElementById('_toolbar'), '', [])),
  };
  return 1; })()`;

// viewport rects of the editor chrome (tools/uidiff/gen_scenarios.py builds regions from them)
const RECT_IDS = ['hotbar', '_toolbar', 'playbar', 'rightMenu', 'versionTxt', 'canvasUI', 'transform', 'translateOption',
  'rotateOption', 'scaleOption', 'spaceOption', 'faceOption', 'groupOption', 'wireframeOption', 'snappingOption', 'chatHolder',
  'chatList', 'canvasInfo', 'canvasAdd', 'canvasObjEdit', 'center', 'container', 'view', 'windowHolder', 'menuWindow',
  'windowHeader', 'genericPop', 'popupHolder', 'editor-overlay'];
const RECTS = `(() => { const o = {}; const put = (k, e) => { if (!e) return; const cs = getComputedStyle(e);
  if (cs.display === 'none' || cs.visibility === 'hidden') return; const r = e.getBoundingClientRect(); if (r.width < 1 || r.height < 1) return;
  o[k] = [Math.round(r.x), Math.round(r.y), Math.round(r.width), Math.round(r.height)]; };
  for (const id of ${JSON.stringify(RECT_IDS)}) put(id, document.getElementById(id));
  put('left-panel', document.querySelector('.left.panel')); put('right-panel', document.querySelector('.right.panel'));
  put('left-restore', document.querySelector('.left.restore')); put('right-restore', document.querySelector('.right.restore'));
  return o; })()`;

// ---------------------------------------------------------------- session

async function openEditor() {
  const server = scriptingPage() ? await overlayServer() : await staticServer(V7_PUBLIC, 0);
  const httpPort = server.address().port;
  const port = await freePort();
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'kn-v7ref-chrome-'));
  const chrome = launchChrome(port, W, H, profile);
  const c = await CDP.connect(port);
  await c.send('Runtime.enable');
  await c.send('Page.enable');
  const errors = [];
  c.on((m) => { if (m.method === 'Runtime.exceptionThrown') errors.push((m.params.exceptionDetails.exception || {}).description || m.params.exceptionDetails.text); });
  await c.setViewport(W, H);
  // offline: nothing but the static serve (the YouTube iframes of windows 12 / 13, captcha, ads)
  await c.send('Network.enable');
  await c.send('Network.setBlockedURLs', { urls: ['https://*', 'wss://*'] });
  const base = `http://127.0.0.1:${httpPort}/`;
  await c.send('Page.navigate', { url: base + 'manifest.json' });
  await sleep(300);
  await c.eval('(() => { localStorage.clear(); return 1; })()');
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: SEED_RNG });
  await c.send('Page.navigate', { url: base + 'editor.html' });
  await c.waitFor('typeof KE === "object" && typeof GUI === "object" && !!document.getElementById("_toolbar").children.length', 60000);
  await sleep(3000);
  await c.eval(DUMP_LIB);
  const close = () => { try { c.close(); } catch (e) { } try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { } server.close(); fs.rmSync(profile, { recursive: true, force: true }); };
  return { c, errors, close };
}

function write(rel, data) {
  const f = path.join(OUT, rel);
  fs.mkdirSync(path.dirname(f), { recursive: true });
  fs.writeFileSync(f, data);
}

async function shot(c, name) {
  await c.eval(TIDY);
  await sleep(150);
  fs.mkdirSync(path.dirname(path.join(OUT, name)), { recursive: true });
  await c.shot(path.join(OUT, name + '.png'));
}

// reset between states: no window, no popup, no open menu, pointer parked over the version text
async function reset(c) {
  await c.mouse('mouseMoved', 1800, 15, 'none');
  await c.eval(`(() => { clearPops(); closeWindow(); for (const d of document.querySelectorAll('#_toolbar .toolbarDropDown, #_toolbar .toolbarSubDropDown')) d.style.display = '';
    return 1; })()`);
  await sleep(250);
}

async function centre(c, expr) {
  return c.eval(`(() => { const e = ${expr}; if (!e) return null; const r = e.getBoundingClientRect(); return [r.x + Math.min(r.width / 2, 30), r.y + r.height / 2]; })()`);
}

// ---------------------------------------------------------------- states

const S = {};
const state = (name, run) => { S[name] = run; };

state('editor-1920', async (c) => {
  await reset(c);
  await sleep(500);
  await shot(c, 'editor-1920');
  write('editor-1920.rects.json', JSON.stringify(await c.eval(RECTS), null, 0) + '\n');
  // the closed Welcome window's markup stays in #menuWindow with its counters still running:
  // pinned to their final values in the same evaluation that reads the DOM
  const html = await c.eval(`(() => { ['160,000,000', '12', '750,000'].forEach((v, i) => { const e = document.getElementById('animVl' + i); if (e) e.textContent = v; });
    return document.documentElement.outerHTML; })()`);
  write('editor-1920.html.gz', zlib.gzipSync(Buffer.from(html, 'utf8'), { level: 9 }));
});

state('toolbar', async (c) => {
  await reset(c);
  write('toolbar.dump.txt', await c.eval('__knDump.toolbar()'));
  const roots = await c.eval(`[...document.querySelectorAll('#_toolbar > .toolbarItem')].map((e) => [...e.childNodes].filter((n) => n.nodeType === 3).map((n) => n.nodeValue).join('').trim())`);
  for (let i = 0; i < roots.length; i++) {
    await reset(c);
    const p = await centre(c, `document.querySelectorAll('#_toolbar > .toolbarItem')[${i}]`);
    await c.mouse('mouseMoved', p[0], p[1], 'none');
    await sleep(300);
    await shot(c, 'toolbar-' + roots[i].toLowerCase());
  }
  for (const sub of ['Basic', 'Models', 'Lights']) {
    await reset(c);
    const root = await centre(c, `[...document.querySelectorAll('#_toolbar > .toolbarItem')].find((e) => e.firstChild.nodeValue.trim() === 'Object')`);
    await c.mouse('mouseMoved', root[0], root[1], 'none');
    await sleep(300);
    const item = `[...document.querySelectorAll('#_toolbar > .toolbarItem')].find((e) => e.firstChild.nodeValue.trim() === 'Object')
      .querySelector('.toolbarDropDown').querySelectorAll(':scope > .toolbarDropDownItem')`;
    const p = await centre(c, `[...${item}].find((e) => e.firstChild.nodeValue.trim() === '${sub}')`);
    // through the dropdown column to the item (a jump out of the menu would close it)
    await c.mouse('mouseMoved', root[0], p[1], 'none');
    await c.mouse('mouseMoved', p[0], p[1], 'none');
    await sleep(400);
    await shot(c, 'toolbar-object-' + sub.toLowerCase());
  }
  await reset(c);
});

async function windowState(c, name, n, extra, wait = 600) {
  await reset(c);
  await c.eval(`(() => { showWindow(${n}); return 1; })()`);
  await sleep(wait);
  if (extra) { await c.eval(`(() => { ${extra}; return 1; })()`); await sleep(wait); }
  if (n === 8) await c.waitFor(`(() => { const want = ['160,000,000', '12', '750,000'];
    return want.every((v, i) => { const e = document.getElementById('animVl' + i); return e && e.textContent === v; }); })()`, 30000);
  await shot(c, name);
  write(name + '.dump.txt', await c.eval('__knDump.window()'));
  write(name + '.rects.json', JSON.stringify(await c.eval(RECTS), null, 0) + '\n');
}

// 12 / 13: YouTube iframes (blocked offline) take a while to settle on Chromium's error page
for (let n = 1; n <= 15; n++) state('window' + n, (c) => windowState(c, 'window' + n, n, null, [12, 13].includes(n) ? 4000 : [2, 5, 7, 9, 14, 15].includes(n) ? 1500 : 600));
state('window1-advanced', (c) => windowState(c, 'window1-advanced', 1, 'windows[0].switchTab(1)'));
state('window3-display', (c) => windowState(c, 'window3-display', 3, 'windows[2].switchTab(1)'));
state('window3-object', (c) => windowState(c, 'window3-object', 3, 'windows[2].switchTab(2)'));
state('window3-quickgui', (c) => windowState(c, 'window3-quickgui', 3, 'windows[2].switchTab(3)'));
state('window4-edit', (c) => windowState(c, 'window4-edit', 4, 'windows[3].toggleEditMode()'));

state('teleport', async (c) => {
  await reset(c);
  await c.eval('(() => { KE.teleportPopup(); return 1; })()');
  await sleep(500);
  await shot(c, 'teleport');
  write('teleport.dump.txt', await c.eval('__knDump.popup()'));
  await reset(c);
});

// the publishing / My Games popups (editor WP6, editor_net.c): KE.account is faked (a free account,
// premiumT 0) since v7's login needs the live socket; the publish popup's map field stays empty
// (no mapToPub). net/<name>.{png,dump.txt}
const NET_POPUPS = {
  'publish-game': 'KE.publishGamePopup(true)',
  'update-game': 'KE.publishGamePopup(false, {map_id: 7, map_name: "VisMap", map_description: "about", map_updatecounter: 1, ca: 2, mob: 1})',
  'publish-asset': 'KE.publishAssetPopup()',
  'publish-asset-sound': 'KE.publishAssetPopup(); gameType.value = 3; KE.updateAssetType(3)',
  'edit-map': 'openEdit("VisMap", 7, 1234)',
  'map-resp': 'KE.mapResp("Game Uploaded!", "VisMap")',
  'delete-map': 'deleteCreationConf(7, 0)',
};
for (const [name, call] of Object.entries(NET_POPUPS)) {
  state('net-' + name, async (c) => {
    await reset(c);
    await c.eval(`(() => { KE.account = {name: 'visuser', id: 1, premiumT: 0, funds: 0}; ${call}; return 1; })()`);
    await sleep(500);
    await shot(c, 'net/' + name);
    write('net/' + name + '.dump.txt', await c.eval('__knDump.popup()'));
    await c.eval('(() => { KE.account = null; clearPops(); return 1; })()');
    await reset(c);
  });
}

state('left-essential', async (c) => {
  await reset(c);
  write('left-essential.dump.txt', await c.eval('__knDump.panel("left")'));
});

for (const p of ['environment', 'terrain', 'zone', 'ocean', 'tools', 'prefab', 'objectList', 'gui', 'blank']) {
  state('right-' + p, async (c) => {
    await reset(c);
    await c.eval(`(() => { KE.hideTransform && KE.hideTransform(); GUI.panel.right.${p}.show(); return 1; })()`);
    await sleep(500);
    await shot(c, 'right-' + p);
    write('right-' + p + '.dump.txt', await c.eval('__knDump.panel("right")'));
  });
}

// the object inspector (WP3's uidiff scenario editor-inspector): a cube with Transform and Style open
state('right-object-cube', async (c) => {
  await reset(c);
  await c.eval(`(() => { KE.clearMap(); KE.createObjects[0](); document.getElementById('transformH').click();
    document.getElementById('styleH').click(); return 1; })()`);
  await sleep(500);
  await shot(c, 'right-object-cube');
  write('right-object-cube.rects.json', JSON.stringify(await c.eval(RECTS), null, 0) + '\n');
  write('right-object-cube.dump.txt', await c.eval('__knDump.panel("right")'));
  await c.eval('(() => { KE.clearMap(); GUI.panel.right.blank.show(); return 1; })()');
});

// public/js/hook.js defines a non-configurable Object.prototype.events accessor, which hides the
// "Trigger Event" folder of TRIGGER (29) / AI_SPAWNER (55) (vO234.events never becomes an own key
// of the Technical blueprint): like `logic`, this state loads the page without the hook.
async function reloadPage(c, hook) {
  await c.send('Network.setBlockedURLs', { urls: hook ? ['https://*', 'wss://*'] : ['https://*', 'wss://*', '*/js/hook.js*'] });
  await c.eval('(() => { window.onbeforeunload = null; return 1; })()'); // "You have unsaved changes!" would block the reload
  await c.send('Page.reload', {});
  await c.waitFor('typeof KE === "object" && typeof GUI === "object" && !!document.getElementById("_toolbar").children.length', 60000);
  await sleep(2500);
  await c.eval(DUMP_LIB);
  if (!hook && await c.eval(`Object.prototype.hasOwnProperty('events')`)) throw new Error('hook.js still loaded');
}

state('prefabs', async (c) => {
  await reloadPage(c, false);
  await reset(c);
  const ids = await c.eval(`(() => { const o = []; for (const i in KE.createObjects) o.push(+i); return o; })()`);
  const skipped = [];
  for (const i of ids) {
    if (i === 44) { skipped.push(i + ' (CUSTOM_ASSET opens the asset browser)'); continue; }
    const r = await c.eval(`(() => { try {
      KE.clearMap(); GUI.panel.right.blank.show(); const before = KE.objInsts.length;
      KE.createObjects[${i}]();
      if (KE.objInsts.length === before) return {skip: 'nothing created (localOnly in production?)'};
      const o = KE.objInsts[KE.objInsts.length - 1];
      return {name: o.objType, json: JSON.stringify(o.serialize(), null, 1)};
    } catch (e) { return {skip: 'threw ' + e}; } })()`);
    await c.eval(`(() => { closeWindow(); clearPops(); return 1; })()`);
    if (r.skip) { skipped.push(i + ' ' + r.skip); continue; }
    await sleep(120);
    write(`prefabs/${i}.json`, r.json + '\n');
    write(`prefabs/${i}.right.dump.txt`, await c.eval('__knDump.panel("right")'));
  }
  write('prefabs/skipped.txt', skipped.map((s) => s + '\n').join(''));
  console.error('[v7ref] prefabs: ' + (ids.length - skipped.length) + ' written, skipped: ' + skipped.join('; '));
  await reloadPage(c, true); // back to the page the other states capture (with the hook)
});

// inspector edits (doc/v7/editor.md WP3): every case of tools/editor/inspector_edits.json on a
// cleared map, each [label, value] set on the right panel's nth control so labelled as a user edit
// (the value, then its oninput / onchange / onclick), then the object's serialize(). Written as an
// editor_serialize_test golden ({"maps": {"inspector-edits": [[edited, ObjectInstance.deserialize(
// edited).serialize()], ...]}}: the native model must load v7's result as v7 does) plus the case
// list; tools/editor/inspector_check.py replays the cases in the native editor and compares its
// objects with the second form (the native editor holds objects in that form).
state('edits', async (c) => {
  await reset(c);
  const spec = JSON.parse(fs.readFileSync(path.join(REPO, 'tools', 'editor', 'inspector_edits.json'), 'utf8'));
  const pairs = [], cases = [];
  for (const k of spec.cases) {
    // async: the generated geometry (genGeo, e.g. SPAWN_POINT / SERVER_NODE sizes) settles first
    const r = await c.eval(`(async () => { try {
      const settle = () => new Promise((res) => setTimeout(res, 250));
      KE.clearMap(); GUI.panel.right.blank.show(); KE.createObjects[${k.prefab}]();
      await settle();
      const o = KE.objInsts[KE.objInsts.length - 1];
      const miss = [], errs = [];
      for (const [lab, val] of ${JSON.stringify(k.set)}) {
        const m = /^(.*?)(?:#(\\d+))?$/.exec(lab), want = m[1], nth = +(m[2] || 0);
        let seen = 0, ctl = null;
        for (const it of document.querySelectorAll('.right.panel .list .inlineItem')) {
          const sec = it.querySelector('.section');
          if (!sec || sec.textContent.trim() !== want) continue;
          if (seen++ === nth) { ctl = it.querySelector('input, select'); break; }
        }
        if (!ctl) { miss.push(lab); continue; }
        try {
          if (ctl.type === 'checkbox') { ctl.checked = val === '1' || val === 'true'; ctl.onclick.call(ctl); }
          else { ctl.value = val; if (ctl.oninput) ctl.oninput.call(ctl); if (ctl.onchange) ctl.onchange.call(ctl); }
        } catch (e) { errs.push(lab + ': ' + e); } // a setter's mesh update can throw offline (no texture loaded)
      }
      await settle();
      const obj = o.serialize();
      let again = null;
      try { const o2 = o.constructor.deserialize(JSON.parse(JSON.stringify(obj))); await settle(); again = o2.serialize(); } catch (e) { errs.push('deserialize: ' + e); }
      return {obj, again, miss, errs};
    } catch (e) { return {error: String(e)}; } })()`);
    if (r.error) throw new Error('edits ' + k.name + ': ' + r.error);
    cases.push(Object.assign({name: k.name, prefab: k.prefab}, r.miss.length ? {miss: r.miss} : {}, r.errs.length ? {errors: r.errs} : {}));
    pairs.push([r.obj, r.again || r.obj]);
  }
  write('inspector-edits.json', JSON.stringify({cases, maps: {'inspector-edits': pairs}}, null, 1) + '\n');
  await c.eval('(() => { KE.clearMap(); GUI.panel.right.blank.show(); return 1; })()');
  console.error('[v7ref] edits: ' + cases.length + ' cases' + (cases.some((x) => x.miss) ? ', missing controls: ' +
    cases.filter((x) => x.miss).map((x) => x.name + ' ' + x.miss.join('/')).join('; ') : ''));
});

// ---------------------------------------------------------------- logic (editor WP4)
// tools/editor/logic_cases.json replayed in v7: trigger edits through GUI.action on the object
// panel's blueprint (the same handlers a click runs), the Custom GUI panel's blueprint, and the
// left panel's values / Editor methods. Writes logic/<case>.json (the edited object's serialize(),
// or {config, gui} of getMapExport) and logic/<case>.dump.txt (the right or left panel).
// public/js/hook.js defines a non-configurable Object.prototype.events accessor, which hides the
// "Trigger Event" folder (vO234.events never becomes an own key of the blueprint): this state
// loads the page without it (the editor does not need the hook).
const LOGIC_LIB = `(() => {
  const fake = (kind, v) => kind === 'switch' ? {type: 'checkbox', checked: !!v} : kind === 'select' ? {type: 'select-one', value: String(v)}
    : kind === 'text' || kind === 'color' ? {type: 'text', value: String(v)} : {type: kind === 'number' ? 'number' : 'range', value: String(v)};
  const objPath = (prop) => {
    const m = /^(\\w+?)(?:@(\\d+))?$/.exec(prop), name = m[1], i = m[2];
    const top = {canTrigger: ['events', 'children', 'canTrigger'], event: ['events', 'children', 'event'], constantEvent: ['events', 'children', 'constE'],
      actionMethod: ['actions', 'children', 'actionMethod'], actionCost: ['actions', 'children', 'actionCost'], chance: ['actions', 'children', 'chance']};
    if (top[name]) return top[name];
    const a = ['actions', 'children', 'action' + i, 'children'];
    if (['action', 'constantType', 'constantCust', 'constantTxt', 'constant', 'constant2', 'delay'].includes(name)) return [...a, name];
    if (name === 'condition') return [...a, 'conditions', 'children', 'type'];
    const cv = /^conditionVal(\\d)$/.exec(name);
    if (cv) { const ch = GUI.panel.right.object.blueprint.technical.children.actions.children['action' + i].children.conditions.children;
      return [...a, 'conditions', 'children', Object.keys(ch)[1 + +cv[1]]]; }
    return [...a, 'sound', 'children', {snd: 'type', assetSound: 'custom', vol: 'volume', rate: 'rate', emitfrom: 'emitfrom'}[name]];
  };
  const guiKey = {lbl: 'elementLabel', t: 'type', vt: 'valueType', v: 'text', f: 'fontsize', s: 'img', c: 'color', o: 'opacity', x: 'x', y: 'y',
    w: 'width', h: 'height', z: 'zIndex', nv: 'hide', id: 'elementId'};
  const setPath = (root, path, value) => { const keys = path.split('.'); let o = root;
    for (const k of keys.slice(0, -1)) o = o[k] = o[k] && typeof o[k] === 'object' ? o[k] : {}; o[keys[keys.length - 1]] = value; };
  window.__knLogic = {
    run(c) {
      closeWindow(); clearPops(); KE.clearMap();
      let o = null;
      if (c.prefab != null) { KE.createObjects[c.prefab](); o = KE.objInsts[KE.objInsts.length - 1]; }
      for (const s of c.steps) {
        if (s.set != null) GUI.action(['panel', 'right', 'object', 'blueprint', 'technical', 'children', ...objPath(s.set)], fake(s.kind, s.value));
        else if (s.add != null) o.addAction();
        else if (s.remove != null) o.removeAction(null, {id: s.remove});
        else if (s.gui_add != null) { GUI.panel.right.gui.show(); KE.addGUIElement(); }
        else if (s.gui_remove != null) KE.removeGUIElement(null, {id: s.gui_remove});
        else if (s.gui_dup != null) KE.duplicateGUIElement(null, {id: s.gui_dup});
        else if (s.gui_set != null) { GUI.panel.right.gui.show();
          GUI.action(['panel', 'right', 'gui', 'blueprint', 'element' + s.gui_set, 'children', guiKey[s.key]], fake(s.kind, s.value)); }
        else if (s.cfg_set != null) { if (s.cfg_set.startsWith('map.')) KE.mapConfig[s.cfg_set.slice(4)] = s.value; else setPath(KE.serverConfig, s.cfg_set, s.value);
          GUI.panel.left.essential.show(); }
        else if (s.cfg_copy != null) KE.copyPlayerConfig(null, {id: s.cfg_copy});
        else if (s.cfg_copycat != null) KE.copyPlayerConfigCat(null, {id: s.cfg_copycat, cat: s.sub});
        else if (s.cfg_reset != null) (s.cfg_reset === 'cls' ? KE.resetPlayerConfig : KE.resetEffectConfig).call(KE, null, {id: s.index});
      }
      if (o) { KE.updateObjConfigGUI(); return {json: JSON.stringify(o.serialize(), null, 1), dump: __knDump.panel('right')}; }
      let e = KE.getMapExport(); if (typeof e === 'string') e = JSON.parse(e);
      const gui = c.steps.some((s) => s.gui_add != null);
      if (gui) GUI.panel.right.gui.show(); else GUI.panel.left.essential.show();
      return {json: JSON.stringify({config: e.config || null, gui: e.gui || null}, null, 1), dump: __knDump.panel(gui ? 'right' : 'left')};
    },
  };
  return 1; })()`;

state('logic', async (c) => {
  await reset(c);
  const cases = JSON.parse(fs.readFileSync(path.join(__dirname, 'logic_cases.json'), 'utf8')).cases;
  for (const k of cases) {
    // every case starts from a fresh page (the server / map config and the GUI list are page state)
    console.error('[v7ref] logic ' + k.name);
    await reloadPage(c, false);
    await c.eval(LOGIC_LIB);
    const r = await c.eval(`__knLogic.run(${JSON.stringify(k)})`);
    write(`logic/${k.name}.json`, r.json + '\n');
    write(`logic/${k.name}.dump.txt`, r.dump);
  }
  await reloadPage(c, true); // back to the page the other states capture (with the hook)
});

// ---------------------------------------------------------------- tools (editor WP6)
// tools/editor/tools_cases.json in v7: each map imported (KE.importMap) into a cleared editor,
// then KE.optimizeFaces() or KE.mergeColors() (advancedOptions.colorRange = range; confirm()
// answers true). Writes tools/<case>.json: {alert, before, after}, before / after = every
// object's serialize() (tools/editor/tools_check.py compares the native editor with them).
state('tools', async (c) => {
  await reset(c);
  const cases = JSON.parse(fs.readFileSync(path.join(__dirname, 'tools_cases.json'), 'utf8')).cases;
  for (const k of cases) {
    const r = await c.eval(`(async () => { try {
      const settle = () => new Promise((res) => setTimeout(res, 250));
      closeWindow(); clearPops(); KE.clearMap(); GUI.panel.right.blank.show();
      KE.importMap(${JSON.stringify(JSON.stringify(k.map))});
      await settle();
      const before = KE.objInsts.map((o) => o.serialize());
      window.__knAlerts = [];
      if (${JSON.stringify(k.tool)} === 'mergeColors') { KE.advancedOptions.colorRange = ${JSON.stringify(k.range || 1500)}; KE.mergeColors(); }
      else KE.optimizeFaces();
      await settle();
      return {alert: window.__knAlerts.join('\\n'), before, after: KE.objInsts.map((o) => o.serialize())};
    } catch (e) { return {error: String(e)}; } })()`);
    if (r.error) throw new Error('tools ' + k.name + ': ' + r.error);
    write(`tools/${k.name}.json`, JSON.stringify(r, null, 1) + '\n');
    console.error('[v7ref] tools ' + k.name + ': ' + r.alert);
  }
  await c.eval('(() => { KE.clearMap(); GUI.panel.right.blank.show(); return 1; })()');
});

// ---------------------------------------------------------------- the KrunkScript window

// the script window in ed_script_dump's format (client/src/editor/editor_script.c)
const SCRIPT_DUMP = `(() => { const w = KE.scriptWin; if (!w || !w.cm) return 'closed\\n';
  const d = w.document, out = [], txt = (s) => (s || '').replace(/\\s+/g, ' ').trim();
  out.push('H ' + txt(d.getElementById('label').textContent));
  for (const t of d.querySelectorAll('#tabHolder .tab')) { const ic = t.querySelector('.tabIcon');
    out.push('T ' + txt(t.textContent) + (t.classList.contains('selected') ? ' *' : '') + (ic.classList.contains('iconE') ? ' [E]' : '') + (ic.classList.contains('iconV') ? ' [V]' : '') + (ic.classList.contains('iconC') ? ' [C]' : '')); }
  out.push('B ' + txt(d.getElementById('compile').innerText));
  const key = d.querySelector('#tabHolder .tab.selected').id, cm = w.cm[key];
  out.push('C ' + key + ' lines=' + cm.lineCount() + ' font=' + cm.getWrapperElement().style.fontSize);
  for (const m of cm.getAllMarks()) if (m.className === 'code-error') { const r = m.find(); if (r) out.push('E ' + r.to.line); }
  for (const li of d.querySelectorAll('.CodeMirror-hints li')) { const a = li.querySelector('a'), t = li.textContent;
    out.push('K ' + (li.classList.contains('CodeMirror-hint-active') ? '* ' : '') + (a ? t.slice(0, t.length - a.textContent.length - 1) : t) + ' | ' + (a ? a.textContent : '')); }
  out.push('S ' + cm.indexFromPos(cm.getCursor('from')) + ' ' + cm.indexFromPos(cm.getCursor('to')));
  return out.join('\\n') + '\\n'; })()`;

// The client script of the error state: the fixture tools/editor/script_window_test.py loads
// natively
const BAD_SCRIPT = fs.readFileSync(path.join(REPO, 'client', 'tests', 'data', 'editor_script_bad.krnk'), 'utf8');

async function scriptingOpen(c, fresh) {
  await reset(c);
  // scriptPopup's window.open -> an iframe at the same place; load -> initCodeMirror as in v7
  const ok = await c.eval(`(() => new Promise((res) => {
    if (!(KE.scriptWin && KE.scriptWin.cm)) {
      const realOpen = window.open;
      window.open = (url) => { const f = document.createElement('iframe'); f.id = 'knScriptRef'; f.src = url;
        f.style.cssText = 'position:fixed;left:10%;top:10%;width:80%;height:80%;border:0;margin:0;z-index:1000;box-shadow:0 0 20px rgba(0,0,0,0.6)';
        document.body.appendChild(f); return f.contentWindow; };
      try { KE.scriptPopup(); } finally { window.open = realOpen; }
    }
    const t0 = Date.now();
    // opener: the page calls opener.KE (changeTab -> KE.updateTab, the status button)
    const poll = () => { const w = KE.scriptWin; if (w && w.cm && w.cm.client && w.cm.server) { w.opener = window; res(1); } else if (Date.now() - t0 > 20000) res(0); else setTimeout(poll, 100); };
    poll(); }))()`);
  if (!ok) throw new Error('scripting: initCodeMirror never ran');
  await sleep(400);
  if (fresh) return; // the window as scriptPopup leaves it
  // back to the template on the client tab, nothing marked (the states reuse the window)
  await c.eval(`(() => { const w = KE.scriptWin; const sel = w.document.querySelector('#tabHolder .tab.selected');
    if (sel.id !== 'client') w.changeTab(w.document.getElementById('client'));
    for (const k of ['client', 'server']) { w.cm[k].setValue(KE.scripts[k].code || w.cm[k].getValue()); w.cm[k].setCursor({line: 0, ch: 0}); w.cm[k].highlightErrors(); w.updateTabIcons(k, false, false); }
    if (w.cm.client.state.completionActive) w.cm.client.state.completionActive.close();
    w.compile.innerText = 'Validate Script'; w.compile.style.backgroundColor = null; return 1; })()`);
  await sleep(700); // updateTab's "Saving..." resets after 500 ms
}

async function scriptingState(c, name, setup, wait = 400) {
  if (!scriptingPage()) { console.error('[v7ref] ' + name + ': no recovered scripting.html in ' + SCRIPTING + ' (tools/editor/fetch_scripting_ref.sh)'); return; }
  await scriptingOpen(c, name === 'scripting');
  if (setup) { await c.eval(`(() => { const w = KE.scriptWin; ${setup}; return 1; })()`); await sleep(wait); }
  await shot(c, name);
  write(name + '.dump.txt', await c.eval(SCRIPT_DUMP));
}

state('scripting', (c) => scriptingState(c, 'scripting'));
state('scripting-server', (c) => scriptingState(c, 'scripting-server', `w.changeTab(w.document.getElementById('server'))`, 800));
// io_ksC with an error naming line 5 (E:1963-1991; the editor socket is offline here, so its
// effects are replayed: highlightErrors, the red status button, the tab icon)
state('scripting-error', (c) => scriptingState(c, 'scripting-error', `w.cm.client.setValue(${JSON.stringify(BAD_SCRIPT)});
  w.compile.innerText = 'Failed - errors found!'; w.compile.style.backgroundColor = '#f44336'; w.cm.client.highlightErrors([5]); w.updateTabIcons('client', true, false)`));
// show-hint while typing: "\tGAME.PLAYERS.ge" on a new line 8 of the template (cursorActivity)
state('scripting-hint', (c) => scriptingState(c, 'scripting-hint', `const cm = w.cm.client; cm.focus(); cm.replaceRange('\\n', {line: 7, ch: 0}); cm.setCursor({line: 7, ch: 0});
  cm.replaceRange('\\tGAME.PLAYERS.ge', {line: 7, ch: 0}); cm.setCursor({line: 7, ch: 16})`, 600));

// ---------------------------------------------------------------- main

(async () => {
  const args = process.argv.slice(2);
  const names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
    else if (args[i] === '--scripting') SCRIPTING = path.resolve(args[++i]);
    else if (args[i] === '--list') { console.log(Object.keys(S).join('\n')); return; }
    else names.push(args[i]);
  }
  for (const n of names) if (!S[n]) throw new Error('unknown state ' + n + ' (--list)');
  const list = names.length ? names : Object.keys(S);
  fs.mkdirSync(OUT, { recursive: true });
  const { c, errors, close } = await openEditor();
  try {
    for (const n of list) {
      const e0 = errors.length;
      await S[n](c);
      console.error(`[v7ref] ${n}${errors.length > e0 ? ' (' + (errors.length - e0) + ' page errors: ' + errors.slice(e0, e0 + 2).join(' | ').slice(0, 200) + ')' : ''}`);
    }
  } finally { close(); }
})().catch((e) => { console.error(e); process.exit(1); });
