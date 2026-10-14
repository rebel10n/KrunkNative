#!/usr/bin/env node
// Reconstructed Krunker v10 references for the hub and editor scenarios (doc/ref/krunker-v10/).
//
//   node tools/uidiff/capture-v10-static.js [--out doc/ref/krunker-v10] [--public <mirror>] [--list] [name...]
//
// No Krunker JavaScript runs here (v10's bundles are not even served). Each reference is the
// DOM of the matching Krunker v7.1.4 capture (doc/ref/krunker-v7.1.4/<dir>/<v7>.html.gz, made
// by v7's own client: tools/uidiff/capture-v7.js, tools/editor/v7ref.js with KN_V7REF_HTML=1)
// moved onto v10:
//   - the <head> is the live v10 page's (social.html / editor.html of the krunker.io mirror,
//     tools/ui-import/fetch_live_site.py), scripts removed: v10's stylesheets;
//   - the edits that turn v7.1.4's markup into v10's (OPS below), each one anchored in the
//     webcracked v10 code (~/.cache/kn-v10/webcrack/<chunk>/renamed.js:<line>) or in the v10
//     page markup: what v10's static page and its generators produce differently in that state.
// Chromium loads the result from a read-only 127.0.0.1 serve of the mirror (then ui/ for what
// the mirror never fetched, e.g. CodeMirror's CSS v10 loads from cdnjs) with page scripts
// disabled (Emulation.setScriptExecutionDisabled), every other host unresolvable and https/wss
// blocked; the edits and the measurements are CDP evaluations of this file's own code.
// CSS animations are paused at 800 ms (the scenarios' anim_time 0.8).
// Output: <out>/<dir>/<name>.{png,rects.json,html.gz} (the rects for tools/uidiff/gen_scenarios.py).
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const http = require('http');
const zlib = require('zlib');
const { spawn } = require('child_process');
const { CDP, sleep } = require('../kref/cdp.js');

const REPO = path.resolve(__dirname, '..', '..');
const V7REF = path.join(REPO, 'doc', 'ref', 'krunker-v7.1.4');
const V7_PUBLIC = process.env.KN_V7_PUBLIC || '/home/user/jails/kru/src/Krunker_v7.1.4/public';
let OUT = path.join(REPO, 'doc', 'ref', 'krunker-v10');
let PUBLIC = process.env.KN_V10_PUBLIC || path.join(os.homedir(), '.cache', 'kn-v10', 'live', 'site', 'public');
const ANIM_MS = 800;

const MIME = { '.html': 'text/html', '.css': 'text/css', '.png': 'image/png', '.jpg': 'image/jpeg', '.gif': 'image/gif',
  '.svg': 'image/svg+xml', '.webp': 'image/webp', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.ico': 'image/x-icon' };

// GET only; `/__ref.html` is the page built in memory, everything else the mirror then ui/.
// .js is never served (the page has no scripts; this keeps v10's bundles out by construction).
function server(pageHtml) {
  const roots = [PUBLIC, path.join(REPO, 'ui')];
  const srv = http.createServer((req, res) => {
    const rel = path.normalize(decodeURIComponent(req.url.split('?')[0].split('#')[0])).replace(/^(\.\.[/\\])+/, '');
    if (req.method !== 'GET') { res.writeHead(405); return res.end(); }
    if (rel === '/__ref.html') { res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end(pageHtml()); }
    if (path.extname(rel) === '.js') { res.writeHead(404); return res.end(); }
    for (const root of roots) {
      const f = path.join(root, rel);
      if (f.startsWith(root) && fs.existsSync(f) && fs.statSync(f).isFile()) {
        res.writeHead(200, { 'Content-Type': MIME[path.extname(f).toLowerCase()] || 'application/octet-stream', 'Cache-Control': 'no-store' });
        return fs.createReadStream(f).pipe(res);
      }
    }
    res.writeHead(404); res.end();
  });
  return new Promise((resolve) => srv.listen(0, '127.0.0.1', () => resolve(srv)));
}

function chrome(port, w, h, profile) {
  const bin = process.env.KN_CHROME || 'chromium-browser';
  return spawn(bin, ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--hide-scrollbars', '--mute-audio', `--window-size=${w},${h}`, '--font-render-hinting=none',
    '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1', '--disable-background-networking', '--no-first-run',
    `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`, 'about:blank'], { stdio: 'ignore', detached: true });
}

function freePort() {
  return new Promise((resolve) => { const s = require('net').createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });
}

const stripScripts = (html) => html.replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '').replace(/<!--[\s\S]*?-->/g, '');

// The v10 page's <head> (scripts, preloads, metas removed; cdnjs CodeMirror -> ui/libs).
function v10Head(page) {
  let html = stripScripts(fs.readFileSync(path.join(PUBLIC, page), 'utf8'));
  let head = html.slice(html.indexOf('<head>') + 6, html.indexOf('</head>'));
  head = head.replace(/<link rel="modulepreload"[^>]*>/g, '').replace(/<meta (?:property|name)="(?:og:|twitter:|description|keywords)[^>]*>/g, '')
    .replace(/https:\/\/cdnjs\.cloudflare\.com\/ajax\/libs\/codemirror\/5\.61\.0\//g, './libs/codemirror/5.61.0/');
  // stylesheets v10 links from <body> (editor.html: google-secular-one, material-icons)
  const body = html.slice(html.indexOf('<body'));
  for (const m of body.matchAll(/<link rel="stylesheet"[^>]*>/g)) head += m[0];
  return head;
}

// mode "dom" (the editor: its toolbar, panels and windows are built by script): the v7.1.4
// capture with v10's head. mode "page" (the hub: a static page whose script only shows, hides and
// fills elements): v10's own page markup, scripts removed; the v7.1.4 capture's script-made state
// is moved onto it by TRANSPLANT (page side).
function buildPage(sc) {
  let html;
  if (sc.mode === 'page') {
    html = stripScripts(fs.readFileSync(path.join(PUBLIC, sc.page), 'utf8'));
    const a = html.indexOf('<head>'), b = html.indexOf('</head>');
    html = html.slice(0, a + 6) + v10Head(sc.page) + html.slice(b);
    html = html.replace(/(<body[^>]*>[\s\S]*?)<link rel="stylesheet"[^>]*>/g, '$1');
    return html.replace(/<!doctype html>/i, '').replace(/^/, '<!doctype html>');
  }
  const v7 = zlib.gunzipSync(fs.readFileSync(path.join(V7REF, sc.dir, sc.v7 + '.html.gz'))).toString('utf8');
  html = stripScripts(v7);
  const a = html.indexOf('<head>'), b = html.indexOf('</head>');
  html = html.slice(0, a + 6) + '<meta charset="UTF-8">' + v10Head(sc.page) + html.slice(b);
  // outerHTML has no doctype: without it the page renders in quirks mode (a full-height <body>
  // whose background covers the hub's z-index:-1 backdrop)
  html = '<!doctype html>' + html;
  // v7's body-level stylesheet links (editor.html) are in the new head already
  return html.replace(/(<body[^>]*>[\s\S]*?)<link rel="stylesheet" href="[^"]*(?:google-secular-one|material-icons)\.css[^"]*">/g, '$1');
}

// page side: every element (by id) whose style / class v7.1.4's script changed (its static page vs
// its capture) gets the captured value; the containers its script filled (TRANSPLANT_HTML) get the
// captured markup. Elements v10's page no longer has are skipped.
const TRANSPLANT_HTML = ['genericPop'];
function transplantOp(sc) {
  const v7static = fs.readFileSync(path.join(V7_PUBLIC, sc.page), 'utf8');
  const v7cap = zlib.gunzipSync(fs.readFileSync(path.join(V7REF, sc.dir, sc.v7 + '.html.gz'))).toString('utf8');
  return `const P = new DOMParser(); const S = P.parseFromString(${JSON.stringify(stripScripts(v7static))}, 'text/html');
    const C = P.parseFromString(${JSON.stringify(stripScripts(v7cap))}, 'text/html');
    for (const ce of C.querySelectorAll('[id]')) { const e = $(ce.id); if (!e) continue; const se = S.getElementById(ce.id);
      for (const at of ['style', 'class']) { const cv = ce.getAttribute(at), sv = se ? se.getAttribute(at) : null;
        if (cv !== sv) { if (cv === null) e.removeAttribute(at); else e.setAttribute(at, cv); } } }
    for (const id of ${JSON.stringify(TRANSPLANT_HTML)}) { const ce = C.getElementById(id), e = $(id); if (ce && e) e.innerHTML = ce.innerHTML; }`;
}

const RECT_IDS = {
  hub: ['navBar', 'navTitle', 'profileInfo', 'navButtons', 'loadMessage', 'genericPop', 'popupHolder', 'editrDB', 'steamDB', 'iosDB', 'androidDB', 'viewStack'],
  editor: ['hotbar', '_toolbar', 'playbar', 'rightMenu', 'versionTxt', 'canvasUI', 'transform', 'translateOption',
    'rotateOption', 'scaleOption', 'spaceOption', 'faceOption', 'groupOption', 'wireframeOption', 'snappingOption', 'chatHolder',
    'chatList', 'canvasInfo', 'canvasAdd', 'canvasObjEdit', 'center', 'container', 'view', 'windowHolder', 'menuWindow',
    'windowHeader', 'genericPop', 'popupHolder', 'editor-overlay', 'mpToolbarBtn'],
};
const RECTS = (dir) => `(() => { const o = {}; const put = (k, e) => { if (!e) return; const cs = getComputedStyle(e);
  if (cs.display === 'none' || cs.visibility === 'hidden') return; const r = e.getBoundingClientRect(); if (r.width < 1 || r.height < 1) return;
  o[k] = [Math.round(r.x), Math.round(r.y), Math.round(r.width), Math.round(r.height)]; };
  for (const id of ${JSON.stringify(RECT_IDS[dir])}) put(id, document.getElementById(id));
  put('left-panel', document.querySelector('.left.panel')); put('right-panel', document.querySelector('.right.panel'));
  put('left-restore', document.querySelector('.left.restore')); put('right-restore', document.querySelector('.right.restore'));
  return o; })()`;

// ---------------------------------------------------------------- v7.1.4 -> v10 edits
// Each op is page-side code (a function body over `$` = getElementById, `$q` = querySelector).

const HUB_OPS = [
  // social.html: #leftBox (Krunk TV) and #rightFollBox (Who to Follow) are gone; the right column
  // keeps the ad box only (pages/social.v10.pretty.html vs v7 :208-221)
  `for (const id of ['leftBox', 'rightFollBox', 'steamDB']) { const e = $(id); if (e) e.remove(); }`,
  // the app badges: v10 shows #androidDB only (social/renamed.js:1514-1518; v7 social.js:39-43 showed
  // iosDB + androidDB and always steamDB, which v10's page no longer has)
  `const ios = $('iosDB'); if (ios) ios.style.display = 'none';`,
  // #hackText "HACKER" -> "BANNED" (page markup)
  `const h = $('hackText'); if (h) h.textContent = 'BANNED';`,
  // leaderboard buttons: v10 adds "Ranked" (updateWindow("leaders","player_ranked_matches"), page markup)
  `const bs = [...document.querySelectorAll('.leaderBtn2')]; const last = bs[bs.length - 1];
   if (last && !bs.some((b) => /player_ranked_matches/.test(b.getAttribute('onclick') || ''))) {
     const n = document.createElement('div'); n.className = 'leaderBtn2'; n.setAttribute('onclick', 'updateWindow("leaders","player_ranked_matches")');
     n.textContent = 'Ranked'; last.after(n); }`,
  // the ad slots are renamed (no visual change) and v10 adds the Svelte modal stack #viewStack at the end of <body>
  `if (!$('viewStack')) { const v = document.createElement('div'); v.id = 'viewStack'; document.body.appendChild(v); }`,
];

// the hub on v10's own page: what v10's script does differently from v7.1.4's in these states
const HUB_OPS_V10 = [
  // the app badges: v10's script shows #androidDB only (social/renamed.js:1514-1518; v7.1.4's showed iosDB
  // and androidDB, plus steamDB, which v10's page no longer has: the transplant set iosDB's display)
  `const ios = $('iosDB'); if (ios) ios.style.display = 'none';`,
];

const EDITOR_OPS = [
  // editor.html: #twoFAPop removed (page markup; KrunkNative keeps it hidden, css/kn-twofa.css)
  `const t = $('twoFAPop'); if (t) t.remove();`,
  // #canvasInfo: Entities n / 64 and Archetypes n / 32, hidden until the map has some (page markup)
  `const ev = $('serverNodeHolder'); if (ev && !$('entityHolder')) ev.insertAdjacentHTML('beforebegin',
     '<div class="statTitle" id="entityHolder" style="display:none">Entities</div><div id="entityCount" class="stat" style="display:none">0 / 64</div>' +
     '<div class="statTitle" id="archetypeHolder" style="display:none">Archetypes</div><div id="archetypeCount" class="stat" style="display:none">0 / 32</div>');`,
  // the multiplayer button, 4th in #playbar (editor/renamed.js:9701-9713: playbarItemR #mpToolbarBtn,
  // title "Multiplayer Editing", <i id="multiplayerIcon" class="material-icons">group_add</i>)
  `const pb = $('playbar'); if (pb && !$('mpToolbarBtn')) { const d = document.createElement('div'); d.id = 'mpToolbarBtn'; d.className = 'playbarItemR';
     d.title = 'Multiplayer Editing'; d.innerHTML = '<i id="multiplayerIcon" class="material-icons">group_add</i>'; pb.appendChild(d); }`,
  `if (!$('viewStack')) { const v = document.createElement('div'); v.id = 'viewStack'; document.body.appendChild(v); }`,
];

// Svelte component stylesheets (injected as <style> at mount by v10's runtime): the `{hash, code}`
// literals of the webcracked shared chunk (static strings, read as JSON, never run)
const WEBCRACK_INDEX = process.env.KN_V10_WEBCRACK_INDEX || path.join(os.homedir(), '.cache', 'kn-v10', 'webcrack', 'shared', 'index', 'renamed.js');
function svelteCss(hashes) {
  const src = fs.readFileSync(WEBCRACK_INDEX, 'utf8');
  return hashes.map((h) => {
    const m = new RegExp('hash: "svelte-' + h + '",\\s*code: ("(?:[^"\\\\]|\\\\.)*")').exec(src);
    if (!m) throw new Error('svelte-' + h + ' not in ' + WEBCRACK_INDEX);
    return JSON.parse(m[1]);
  }).join('\n');
}

// v10's hub login (social/renamed.js:4375 loginPopup -> index:8478 viewStack push of the LoginModal
// index:11493 with {canRegister:false}) in its first state for a guest with no saved
// krunker_username (index:11506 `useEmail = !name`): the email field, the password input
// present but `hidden-password` until typed (index:11760), the Login submit button, no register
// row (canRegister false), no message. Markup = the component templates index:11466-11479
// (_3631, _3634, _3635, _3638) in the popup frame index:8534 (_2423, width 800) inside the
// view stack index:12537 (_3945/_3946); text from translations.en (bundle/i18n-en.json).
// The view-stack wrapper's --ui-scale vars are written by app.js's scaleBase(), which
// social.html does not run: the fallbacks (scale 1, 100%) apply.
const HUB_LOGIN = () => {
  const css = svelteCss(['sdh4ve', '1wz7ymp', '16z1lxi']);
  const html = '<div class="view-stack svelte-sdh4ve"><div class="view-stack-overlay svelte-sdh4ve"><div class="view-wrapper svelte-sdh4ve" style="display: block">' +
    '<div class="popup-container svelte-1wz7ymp"><div class="popup svelte-1wz7ymp" style="width: 800px;"><div class="io-popup-container"><div class="io-popup-header">Login to Krunker</div> ' +
    '<div class="io-popup-content"><div class="io-card"><form data-priority="true"><div class="io-input-container svelte-16z1lxi">' +
    '<div class="label-row"><label for="accEmail" class="input-label">Email</label> <button type="button" class="auth-toggle-btn svelte-16z1lxi">(use username instead)</button></div> ' +
    '<input id="accEmail" type="email" class="io-input-field svelte-16z1lxi" autocomplete="email" placeholder="Enter Email"> ' +
    '<input id="accPass" type="password" autocomplete="current-password" placeholder="Enter Password" class="io-input-field hidden-password svelte-16z1lxi"> ' +
    '<button type="submit" class="io-button io-button--accept svelte-16z1lxi">Login</button></div></form></div>    </div></div></div></div></div></div></div>';
  return `const st = document.createElement('style'); st.textContent = ${JSON.stringify(css)}; document.head.appendChild(st);
    const ph = $('popupHolder'); if (ph) ph.style.display = 'none'; const gp = $('genericPop'); if (gp) { gp.innerHTML = ''; gp.style.display = 'none'; }
    $('viewStack').innerHTML = ${JSON.stringify(html)};`;
};

// the Essential panel's closed "History" folder after Effect Config (shared/index/renamed.js:15117-15119:
// addFolder("History", ..., closed) with a history_display child, :14362 its markup), in the folder
// markup of v7.1.4's capture (the header / rule / hidden body of the folders before it)
const EDITOR_HISTORY = `const list = document.querySelector('.left.panel .list'); if (list && !$('historyH')) list.insertAdjacentHTML('beforeend',
  '<div id="historyH" class="header" onclick="GUI.update.folder(this);GUI.action([\\'panel\\',\\'left\\',\\'essential\\',\\'blueprint\\',\\'History\\'], this)">\\n\\t\\t\\t\\t\\t\\t<i class="material-icons">keyboard_arrow_right</i><a>History</a></div><hr>\\n\\t\\t\\t\\t\\t\\t' +
  '<div id="historyB" style="display:none"><div class="history-display"><div class="history-display-header">Recent Actions:</div><div id="historyList" class="history-list"><div class="history-empty">No recent actions</div></div></div></div>');`;
EDITOR_OPS.push(EDITOR_HISTORY);

const EDITOR_VERSION = `const v = $('versionTxt'); if (v) v.innerHTML = "Krunker Editor v2.0.0 - <span style='color:rgba(255,255,255,0.5)'> FRVR</span>";`;
EDITOR_OPS.push(
  // "Krunker Editor v2.0.0 - FRVR" (editor/renamed.js:13908; v7 editor.js:1179 "Yendis Entertainment")
  EDITOR_VERSION);

// Settings (window 3): a fifth tab "Multiplayer" (editor/renamed.js:13035 tabNames; the tab bar markup of
// renamed.js:13044: <div class="menuShortTabNew "> per name, onclick windows[2].switchTab(i))
const SETTINGS_MP_TAB = `const tabs = [...document.querySelectorAll('#menuWindow .menuShortTabNew')]; const last = tabs[tabs.length - 1];
  if (last && !tabs.some((t) => t.textContent.trim() === 'Multiplayer')) { const n = last.cloneNode(false); n.className = 'menuShortTabNew ';
    n.setAttribute('onclick', 'windows[2].switchTab(' + tabs.length + ')'); n.textContent = 'Multiplayer'; last.after(n); }`;

// A CUBE's object panel: the "Dynamic" switch (prefab.canDynamic: CUBE, TRIGGER, CUSTOM_ASSET) after
// Step Sound (index:15953-15962), in the switch row markup of the panel's other switches
const CUBE_DYNAMIC = `const sel = document.querySelector(".right.panel select[onchange*=\\"'stepSrc'\\"]"); const row = sel && sel.closest('.inlineItem');
  const sw = [...document.querySelectorAll('.right.panel .inlineItem')].find((r) => r.querySelector('label.switch input[type=checkbox]'));
  if (row && sw) { const n = sw.cloneNode(true); n.querySelector('.section').textContent = 'Dynamic'; const cb = n.querySelector('input[type=checkbox]');
    cb.removeAttribute('checked'); cb.checked = false; cb.setAttribute('onclick', "GUI.action(['panel','right','object','blueprint','dynamic'], this)"); row.after(n); }`;

const SCENARIOS = [];
const sc = (name, dir, page, v7, size, ops, mode = 'dom') => SCENARIOS.push({ name, dir, page, v7, size, ops, mode });
for (const n of ['hub-maps', 'hub-feed', 'hub-leaders', 'hub-market', 'hub-mods', 'hub-settings', 'hub-login']) {
  const s = { name: n, dir: 'hub', v7: n, page: 'social.html' };
  sc(n, 'hub', 'social.html', n, [1280, 720], [() => transplantOp(s)].concat(HUB_OPS_V10, n === 'hub-login' ? [HUB_LOGIN] : []), 'page');
}
sc('editor-1920', 'editor', 'editor.html', 'editor-1920', [1920, 937], EDITOR_OPS);
sc('right-object-cube', 'editor', 'editor.html', 'right-object-cube', [1920, 937], EDITOR_OPS.concat([CUBE_DYNAMIC]));
for (const n of ['window1', 'window2', 'window9']) sc(n, 'editor', 'editor.html', n, [1920, 937], EDITOR_OPS);
sc('window3', 'editor', 'editor.html', 'window3', [1920, 937], EDITOR_OPS.concat([SETTINGS_MP_TAB]));

async function capture(s) {
  let page = buildPage(s);
  const srv = await server(() => page);
  const port = await freePort();
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'kn-v10ref-chrome-'));
  const ch = chrome(port, s.size[0], s.size[1], profile);
  try {
    const c = await CDP.connect(port);
    await c.send('Page.enable');
    await c.send('Runtime.enable');
    await c.send('Network.enable');
    await c.send('Network.setBlockedURLs', { urls: ['https://*', 'wss://*', 'ws://*'] });
    await c.send('Emulation.setScriptExecutionDisabled', { value: true });
    await c.setViewport(s.size[0], s.size[1]);
    await c.send('Page.navigate', { url: `http://127.0.0.1:${srv.address().port}/__ref.html` });
    await c.waitFor('document.readyState === "complete"', 30000);
    await c.eval('document.fonts.ready.then(() => 1)');
    const ops = s.ops.map((o) => typeof o === 'function' ? o() : o);
    for (const op of ops) await c.eval(`(() => { const $ = (id) => document.getElementById(id); const $q = (q) => document.querySelector(q); ${op}; return 1; })()`);
    await sleep(500);
    await c.eval('document.fonts.ready.then(() => 1)');
    // every <img> and CSS background image decoded before the shot
    await c.eval(`Promise.all([...document.images].map((i) => i.decode().catch(() => 0)).concat([...document.querySelectorAll('*')].flatMap((e) => {
      const m = /url\\("([^"]+)"\\)/.exec(getComputedStyle(e).backgroundImage); if (!m) return [];
      const i = new Image(); i.src = m[1]; return [i.decode().catch(() => 0)]; }))).then(() => 1)`);
    await sleep(300);
    await c.eval(`(() => { for (const a of document.getAnimations()) { try { a.pause(); a.currentTime = ${ANIM_MS}; } catch (e) {} } return 1; })()`);
    await sleep(200);
    if (process.env.KN_V10REF_DEBUG) console.error(JSON.stringify(await c.eval(process.env.KN_V10REF_DEBUG)));
    const dir = path.join(OUT, s.dir);
    fs.mkdirSync(dir, { recursive: true });
    await c.shot(path.join(dir, s.name + '.png'));
    fs.writeFileSync(path.join(dir, s.name + '.rects.json'), JSON.stringify(await c.eval(RECTS(s.dir)), null, 0) + '\n');
    fs.writeFileSync(path.join(dir, s.name + '.html.gz'), zlib.gzipSync(Buffer.from(await c.eval('document.documentElement.outerHTML'), 'utf8'), { level: 9 }));
    c.close();
    console.error(`[capture-v10-static] ${s.dir}/${s.name}.png (${ops.length} edits)`);
  } finally {
    try { process.kill(-ch.pid, 'SIGKILL'); } catch (e) { }
    srv.close();
    fs.rmSync(profile, { recursive: true, force: true });
  }
}

if (require.main === module) (async () => {
  const args = process.argv.slice(2), names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
    else if (args[i] === '--public') PUBLIC = path.resolve(args[++i]);
    else if (args[i] === '--list') { for (const s of SCENARIOS) console.log(`${s.dir}/${s.name}`); return; }
    else names.push(args[i]);
  }
  for (const p of ['social.html', 'editor.html']) if (!fs.existsSync(path.join(PUBLIC, p))) throw new Error(`${PUBLIC}/${p} missing (fetch_live_site.py --stage)`);
  for (const s of SCENARIOS) if (!names.length || names.includes(s.name)) await capture(s);
})().catch((e) => { console.error(e); process.exit(1); });

module.exports = { SCENARIOS };
