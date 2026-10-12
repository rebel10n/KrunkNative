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
//                        right (object) panel, for every prefab the menus can create
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
  const server = await staticServer(V7_PUBLIC, 0);
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

state('prefabs', async (c) => {
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
  await c.eval('(() => { KE.clearMap(); GUI.panel.right.blank.show(); return 1; })()');
  console.error('[v7ref] prefabs: ' + (ids.length - skipped.length) + ' written, skipped: ' + skipped.join('; '));
});

// ---------------------------------------------------------------- main

(async () => {
  const args = process.argv.slice(2);
  const names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
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
