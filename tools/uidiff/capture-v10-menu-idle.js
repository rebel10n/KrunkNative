#!/usr/bin/env node
// References for the menu idle-state uidiff scenarios (client/src/ui/v10_menu.c): v10's main menu
// (doc/ref/krunker-v10.0.2/main-menu.png's capture DOM) with what v10's own code writes in that
// state, laid out by Chromium with v10's CSS. No Krunker JavaScript runs (scripts disabled, .js
// never served; the method and serving of capture-v10-nowplaying.js).
//
//   node tools/uidiff/capture-v10-menu-idle.js --state buffs|krsale|popular [--out <png>]
//   node tools/uidiff/capture-v10-menu-idle.js --state flying --bg <render.png> --x <px> --y <px>
//                                              --text <-500> --t <ms> --out <png>
//
//   buffs    a Saturday of an even ISO week at the INIT: doubleClassXPAd / doubleRaidDropsAd
//            style.display = "block" (v10:219599-219600), #buffSepStart display "" (v10:147310)
//   krsale   a Saturday 12:00 UTC of an odd ISO week: doubleRaidDropsAd "block", the separator, and
//            the Store item's children (v10:147097-147111: " " + the kr-sale-info template with the
//            countdown to Monday 00:00 UTC, 36:00:00)
//   popular  Popular Now (_52548 v10:147200: the popRail / popRailCard templates, v10:141659-141660)
//            as #subLogoButtons' first child with Zombie Labs 40 / Rooftop Parkour 23 / Mini Burg 14
//   flying   the CurrencyDisplay's flying number (_8800 v10:25667: the full-window fixed layer of
//            _8787 with the flying-number template at left x / top y) over a KrunkNative render
//            (--bg, the page's own background), its flyUp animation paused at --t ms
//
// Animations are paused at 800 ms like capture-v10.js (the scenarios' anim_time), flying at --t.
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const http = require('http');
const { spawn } = require('child_process');
const { CDP, sleep } = require('../kref/cdp.js');

const REPO = path.resolve(__dirname, '..', '..');
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const STATE = opt('--state', 'buffs');
const CAPTURE = opt('--capture', path.join(os.homedir(), '.cache', 'kn-wt', 'v10-stage', 'v10-capture'));
const PUBLIC = opt('--public', process.env.KN_V10_PUBLIC || path.join(os.homedir(), '.cache', 'kn-v10', 'live', 'site', 'public'));
const OUT = path.resolve(opt('--out', path.join(REPO, 'doc', 'ref', 'krunker-v10.0.2', `main-menu-${STATE}.png`)));
const BG = opt('--bg', null);
const T = Number(opt('--t', STATE === 'flying' ? 750 : 800));
const [W, H] = [1280, 720];

const MIME = { '.html': 'text/html', '.css': 'text/css', '.png': 'image/png', '.jpg': 'image/jpeg', '.gif': 'image/gif',
  '.svg': 'image/svg+xml', '.webp': 'image/webp', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.ico': 'image/x-icon' };

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

const strip = (html) => html.replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '').replace(/<!--[\s\S]*?-->/g, '');

// [start, end) of the element opening at `start` (a <div>: counts nested <div> / </div>)
function divEnd(html, start) {
  const re = /<div\b|<\/div>/g;
  re.lastIndex = start;
  let depth = 0, m;
  while ((m = re.exec(html))) {
    depth += m[0] === '</div>' ? -1 : 1;
    if (!depth) return m.index + 6;
  }
  throw new Error('unbalanced <div> in the capture');
}

// the capture's document with #uiBase in its main-menu state (capture-v10-nowplaying.js page())
function base() {
  let html = strip(fs.readFileSync(path.join(CAPTURE, 'index.html'), 'utf8'));
  const st = strip(fs.readFileSync(path.join(CAPTURE, 'states', 'main-menu.html'), 'utf8'));
  const at = html.indexOf('<div id="uiBase"');
  if (at < 0 || !st.startsWith('<div id="uiBase"')) throw new Error('capture has no #uiBase');
  html = html.slice(0, at) + st + html.slice(divEnd(html, at));
  html = html.replace(/<canvas id="classPreviewCanvas"[^>]*?style="([^"]*)"[^>]*>[\s\S]*?<\/canvas>/,
    (m, style) => `<div id="classPreviewCanvas" style="display: inline-block; ${style}"></div>`);
  return html;
}

function display(html, id, value) { // element.style.display = value on a capture element with an inline style
  const re = new RegExp(`<[a-z]+ [^>]*id="${id}"[^>]*>`);
  const m = re.exec(html);
  if (!m || !/style="/.test(m[0])) throw new Error('capture has no #' + id + ' with a style');
  const tag = m[0].replace(/style="([^"]*)"/, (all, style) => {
    const rest = style.replace(/display:\s*[^;]*;?\s*/g, '').trim();
    return `style="${(rest + (value ? ` display: ${value};` : '')).trim()}"`;
  });
  return html.slice(0, m.index) + tag + html.slice(m.index + m[0].length);
}

const POPULAR = [['Zombie Labs', 40], ['Rooftop Parkour', 23], ['Mini Burg', 14]];

function page() {
  let html = base();
  if (STATE === 'buffs' || STATE === 'krsale') {
    if (STATE === 'buffs') html = display(html, 'doubleClassXPAd', 'block');
    html = display(html, 'doubleRaidDropsAd', 'block');
    html = display(html, 'buffSepStart', '');
  }
  if (STATE === 'krsale') {
    const re = /(<div class="menuItemTitle svelte-fgmdj8" id="menuBtnShop">Store)\s*(<\/div>)/;
    if (!re.test(html)) throw new Error('capture has no #menuBtnShop');
    html = html.replace(re, (m, a, b) => a + ' <span class="kr-sale-info svelte-159x57l"><span class="kr-sale-title svelte-159x57l">KR DISCOUNT</span> '
      + '<span class="kr-sale-timer svelte-159x57l">36:00:00</span></span>' + b);
  }
  if (STATE === 'popular') {
    const cards = POPULAR.map(([name, n]) => `<div class="popRailCard svelte-6ewa5r" title="${esc(name)}"><div class="popRailInfo svelte-6ewa5r">`
      + `<div class="popRailName svelte-6ewa5r">${esc(name)}</div> <div class="popRailCount svelte-6ewa5r">${n} playing</div></div> `
      + '<span class="material-icons popRailPlay svelte-6ewa5r">play_arrow</span></div>').join('');
    const rail = '<div class="popRail svelte-6ewa5r"><div class="popRailHeader svelte-6ewa5r">Popular Now</div> '
      + `<div class="popRailCards svelte-6ewa5r">${cards}</div></div>`;
    if (!html.includes('<div id="subLogoButtons">')) throw new Error('capture has no #subLogoButtons');
    html = html.replace('<div id="subLogoButtons">', '<div id="subLogoButtons">' + rail);
  }
  if (STATE === 'flying') {
    // the page's background (body) is the KrunkNative render; only the layer is v10's
    const at = html.indexOf('<div id="uiBase"');
    html = html.slice(0, at) + html.slice(divEnd(html, at));
    const x = Number(opt('--x')), y = Number(opt('--y')), text = opt('--text', '+1');
    const cls = text.startsWith('-') ? ' negative' : ' positive';
    // the component's own stylesheet (Svelte injects it on mount; the capture never mounted one):
    // svelte-1gpc1nd's block of ui/css/svelte.css, v10's code verbatim (v10:25662)
    const css = fs.readFileSync(path.join(REPO, 'ui', 'css', 'svelte.css'), 'utf8');
    const from = css.indexOf('/* svelte-1gpc1nd */'), to = css.indexOf('/* svelte-', from + 10);
    if (from < 0) throw new Error('svelte.css has no svelte-1gpc1nd');
    html = html.replace(/<body([^>]*)>/, (m, a) => `<body${a}><style>html, body { background: url(/__bg.png) no-repeat 0 0 !important; }\n${css.slice(from, to)}</style>`
      + '<div style="position: fixed; top: 0px; left: 0px; pointer-events: none; z-index: 9999; width: 100%; height: 100%;">'
      + `<div class="flying-number svelte-1gpc1nd${cls}" style="left: ${x}px; top: ${y}px;">${esc(text)}</div></div>`);
  }
  return '<!doctype html>' + html;
}

function server(html) {
  const roots = [PUBLIC, path.join(REPO, 'ui')];
  const srv = http.createServer((req, res) => {
    const rel = path.normalize(decodeURIComponent(req.url.split('?')[0].split('#')[0])).replace(/^(\.\.[/\\])+/, '');
    if (req.method !== 'GET') { res.writeHead(405); return res.end(); }
    if (rel === '/__ref.html') { res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end(html); }
    if (rel === '/__bg.png' && BG) { res.writeHead(200, { 'Content-Type': 'image/png' }); return fs.createReadStream(BG).pipe(res); }
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

function freePort() {
  return new Promise((resolve) => { const s = require('net').createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });
}

async function main() {
  const srv = await server(page());
  const port = await freePort();
  const profile = fs.mkdtempSync(path.join(os.homedir(), '.cache', 'kn-v10-menu-idle-chrome.'));
  const bin = process.env.KN_CHROME || 'chromium-browser';
  const chrome = spawn(bin, ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--hide-scrollbars', '--mute-audio', `--window-size=${W},${H}`,
    '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1', '--disable-background-networking', '--no-first-run',
    `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`, 'about:blank'], { stdio: 'ignore', detached: true });
  try {
    const cdp = await CDP.connect(port);
    await cdp.send('Page.enable');
    await cdp.send('Emulation.setScriptExecutionDisabled', { value: true });
    await cdp.send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
    await cdp.send('Page.navigate', { url: `http://127.0.0.1:${srv.address().port}/__ref.html` });
    for (let i = 0; i < 100 && (await cdp.eval('document.readyState')) !== 'complete'; i++) await sleep(100);
    await cdp.eval('document.fonts.ready.then(() => 1)');
    await cdp.eval(`document.getAnimations().forEach((a) => { a.pause(); a.currentTime = ${T}; }), 1`);
    await sleep(300);
    const rects = await cdp.eval(`(() => { const o = {}; for (const s of ['.popRail', '#activeBuffs', '#doubleClassXPAd', '#doubleRaidDropsAd', '.kr-sale-info', '.flying-number', '#menuItemShop']) {
      const e = document.querySelector(s); if (!e) continue; const r = e.getBoundingClientRect();
      o[s] = [r.x, r.y, r.width, r.height].map(Math.round); } return o; })()`);
    fs.mkdirSync(path.dirname(OUT), { recursive: true });
    await cdp.shot(OUT);
    cdp.ws.close();
    console.log(OUT, JSON.stringify(rects));
  } finally {
    try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { }
    srv.close();
    await sleep(200);
    fs.rmSync(profile, { recursive: true, force: true });
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
