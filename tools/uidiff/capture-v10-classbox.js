#!/usr/bin/env node
// Reference for the main-menu-classbox-skin uidiff scenario: v10's main menu whose class box (the
// card under the preview: #menuClassName, #menuClassSubtext) shows a skinned loadout, laid out by
// Chromium with v10's own CSS.
//
//   node tools/uidiff/capture-v10-classbox.js [--capture <dir>] [--public <mirror>] [--out <png>] [--as-captured]
//
// No Krunker JavaScript runs here. The page is the DOM the v10 client left in its offline capture
// (~/.cache/kn-wt/v10-stage/v10-capture, the source of doc/ref/krunker-v10.0.2/main-menu.png:
// index.html with its #uiBase from states/main-menu.html), scripts and comments removed, with the
// class box rewritten to what v10's own updateClassPreview writes for a Triggerman whose Assault
// Rifle wears Moon Dragon (store skin 4, rarity 3 Legendary): game.deob.js iììîîii writes
// #menuClassSubtext.innerHTML = "<span style='color:" + store.rarities[3].color + "'>" + weaponConfig[1].name
// + "</span>" (the weapon's name, never the skin's; the colour is the rarity's, "#FBC02D"), and
// #menuClassIcn.src = the skin's thumbnail, which the offline capture cannot load (assets.krunker.io
// is unreachable): the scenario's region leaves the icon out. --as-captured keeps the capture's own
// class box: its render is main-menu.png (the method check).
// Chromium loads it from a read-only 127.0.0.1 serve of the krunker.io mirror (then ui/), .js never
// served, page scripts disabled (Emulation.setScriptExecutionDisabled), every other host unresolvable.
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
const OUT = path.resolve(opt('--out', path.join(REPO, 'doc', 'ref', 'krunker-v10.0.2', 'main-menu-classbox-skin.png')));
const AS_CAPTURED = args.includes('--as-captured');
const [W, H] = [1280, 720]; // the capture's viewport (capture.json)

// v10's text for the skinned loadout (see the header): weapon name, rarity colour
const SUBTEXT = "<span style='color:#FBC02D'>Assault Rifle</span>";

const MIME = { '.html': 'text/html', '.css': 'text/css', '.png': 'image/png', '.jpg': 'image/jpeg', '.gif': 'image/gif',
  '.svg': 'image/svg+xml', '.webp': 'image/webp', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.ico': 'image/x-icon' };

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
  // with scripts disabled a <canvas> is laid out as its fallback content (no box): see capture-v10-nowplaying.js
  html = html.replace(/<canvas id="classPreviewCanvas"[^>]*?style="([^"]*)"[^>]*>[\s\S]*?<\/canvas>/,
    (m, style) => `<div id="classPreviewCanvas" style="display: inline-block; ${style}"></div>`);
  if (!html.includes('<div id="classPreviewCanvas"')) throw new Error('capture has no #classPreviewCanvas');
  if (!AS_CAPTURED) {
    // #menuClassSubtext holds only spans: its first </div> closes it (the container's </div> follows)
    const re = /<div id="menuClassSubtext">[\s\S]*?<\/div>/;
    if (!re.test(html)) throw new Error('capture has no #menuClassSubtext');
    html = html.replace(re, `<div id="menuClassSubtext">${SUBTEXT}</div>`);
  }
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
  const profile = fs.mkdtempSync(path.join(os.homedir(), '.cache', 'kn-v10-classbox-chrome.'));
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
    fs.mkdirSync(path.dirname(OUT), { recursive: true });
    await cdp.shot(OUT);
    cdp.ws.close();
    console.log(OUT);
  } finally {
    try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { }
    srv.close();
    await sleep(200);
    fs.rmSync(profile, { recursive: true, force: true });
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
