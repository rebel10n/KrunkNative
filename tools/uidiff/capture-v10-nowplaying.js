#!/usr/bin/env node
// Reference for the main-menu-nowplaying-long uidiff scenario: v10's main menu with a long
// "Now Playing: <mode> on <map>" and a region, laid out by Chromium with v10's own CSS.
//
//   node tools/uidiff/capture-v10-nowplaying.js [--capture <dir>] [--public <mirror>] [--out <png>]
//                                               [--map-info <text>] [--region <text>] [--as-captured]
//
// No Krunker JavaScript runs here. The page is the DOM the v10 client left in its offline capture
// (~/.cache/kn-wt/v10-stage/v10-capture, the source of doc/ref/krunker-v10.0.2/main-menu.png:
// index.html with its #uiBase from states/main-menu.html), scripts and comments removed, with the two texts v10's
// own code writes changed: #mapInfo.textContent = mode + " on " + map (v10.js:167344) and
// #menuRegionLabel (setGameInstanceId). The match-info bar's segments are content-sized flex items
// (svelte.css .match-seg: display:flex; white-space:nowrap), so the bar grows around a long name.
// Chromium loads it from a read-only 127.0.0.1 serve of the krunker.io mirror (then ui/), .js never
// served, page scripts disabled (Emulation.setScriptExecutionDisabled), every other host unresolvable.
// The defaults are tools/uidiff/scenarios/main-menu-nowplaying-long.json's gameName / region.
// --as-captured keeps the capture's own texts: its render is doc/ref/krunker-v10.0.2/main-menu.png
// (a check of the method: 0.3 % of the pixels differ, the class card's weapon thumbnail, an
// assets.krunker.io image).
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
const CAPTURE = opt('--capture', path.join(os.homedir(), '.cache', 'kn-wt', 'v10-stage', 'v10-capture'));
const PUBLIC = opt('--public', process.env.KN_V10_PUBLIC || path.join(os.homedir(), '.cache', 'kn-v10', 'live', 'site', 'public'));
const OUT = path.resolve(opt('--out', path.join(REPO, 'doc', 'ref', 'krunker-v10.0.2', 'main-menu-nowplaying-long.png')));
const AS_CAPTURED = args.includes('--as-captured');
const MAP_INFO = AS_CAPTURED ? null : opt('--map-info', 'Capture the Flag on Hardpoint Overgrowth Winter Edition');
const REGION = AS_CAPTURED ? null : opt('--region', 'Frankfurt');
const [W, H] = [1280, 720]; // the capture's viewport (capture.json)

const MIME = { '.html': 'text/html', '.css': 'text/css', '.png': 'image/png', '.jpg': 'image/jpeg', '.gif': 'image/gif',
  '.svg': 'image/svg+xml', '.webp': 'image/webp', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.ico': 'image/x-icon' };

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

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

function page() {
  // index.html: the document (head: v10's stylesheets; body: --ui-scale) saved before the menu
  // settled; states/main-menu.html: #uiBase in the main-menu state (main-menu.png's frame)
  let html = strip(fs.readFileSync(path.join(CAPTURE, 'index.html'), 'utf8'));
  const base = strip(fs.readFileSync(path.join(CAPTURE, 'states', 'main-menu.html'), 'utf8'));
  const at = html.indexOf('<div id="uiBase"');
  if (at < 0 || !base.startsWith('<div id="uiBase"')) throw new Error('capture has no #uiBase');
  html = html.slice(0, at) + base + html.slice(divEnd(html, at));
  // with scripts disabled a <canvas> is laid out as its fallback content (no box): the class
  // preview's 750x600 canvas then collapses, and #menuClassContainer::before's glow (z-index 99999
  // container) moves down over the bar. A <div> of the same id and style keeps v10's box (the
  // canvas is opacity 0 until the preview draws: #classPreviewCanvas, main.css); inline-block with
  // no content sits on the baseline like the replaced element it stands in for.
  html = html.replace(/<canvas id="classPreviewCanvas"[^>]*?style="([^"]*)"[^>]*>[\s\S]*?<\/canvas>/,
    (m, style) => `<div id="classPreviewCanvas" style="display: inline-block; ${style}"></div>`);
  if (!html.includes('<div id="classPreviewCanvas"')) throw new Error('capture has no #classPreviewCanvas');
  const edit = (re, to, what) => { if (!re.test(html)) throw new Error('capture has no ' + what); html = html.replace(re, to); };
  if (MAP_INFO !== null) edit(/<div id="mapInfo"([^>]*)>[^<]*<\/div>/, (m, a) => `<div id="mapInfo"${a}>${esc(MAP_INFO)}</div>`, '#mapInfo');
  if (REGION !== null) edit(/<div id="menuRegionLabel"([^>]*)>[^<]*<\/div>/, (m, a) => `<div id="menuRegionLabel"${a}>${esc(REGION)}</div>`, '#menuRegionLabel');
  // outerHTML has no doctype: without it the page renders in quirks mode
  return '<!doctype html>' + html;
}

function server(html) {
  const roots = [PUBLIC, path.join(REPO, 'ui')];
  const srv = http.createServer((req, res) => {
    const rel = path.normalize(decodeURIComponent(req.url.split('?')[0].split('#')[0])).replace(/^(\.\.[/\\])+/, '');
    if (req.method !== 'GET') { res.writeHead(405); return res.end(); }
    if (rel === '/__ref.html') { res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end(html); }
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
  const profile = fs.mkdtempSync(path.join(os.homedir(), '.cache', 'kn-v10-nowplaying-chrome.'));
  const bin = process.env.KN_CHROME || 'chromium-browser';
  const chrome = spawn(bin, ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--hide-scrollbars', '--mute-audio', `--window-size=${W},${H}`, // the flags of capture_v10_live.js (main-menu.png)
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
    await cdp.eval(`document.getAnimations().forEach((a) => { a.pause(); a.currentTime = 800; }), 1`);
    await sleep(300);
    const rects = await cdp.eval(`(() => { const o = {}; for (const id of ['matchInfoHolder', 'mapInfoHld', 'menuRegionLabel', 'inviteButton', 'menuBtnJoin', 'menuPingDisplay']) {
      const e = document.getElementById(id); if (!e) continue; const r = e.getBoundingClientRect();
      o[id] = [r.x, r.y, r.width, r.height].map(Math.round); } return o; })()`);
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
