// Shared harness for the Krunker v7.1.4 captures (capture_v7_windows.js, tools/uidiff/capture-v7.js):
// a read-only static server over v7's public/ (no npm server: it writes into the reference
// tree) and headless Chromium over CDP (tools/kref/cdp.js). The page runs v7's own client
// (index.js) offline: with no matchmaker it starts its local sandbox game (connectToSandbox).
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawn } = require('child_process');
const { CDP, sleep } = require('../kref/cdp.js');

const V7_PUBLIC = process.env.KN_V7_PUBLIC || '/home/user/jails/kru/src/Krunker_v7.1.4/public';

const MIME = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.gif': 'image/gif', '.svg': 'image/svg+xml', '.webp': 'image/webp',
  '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg',
  '.mp4': 'video/mp4', '.webm': 'video/webm', '.obj': 'text/plain', '.glb': 'model/gltf-binary', '.gltf': 'model/gltf+json',
  '.map': 'application/json', '.ico': 'image/x-icon', '.jspck': 'application/octet-stream', '.txt': 'text/plain' };

// GET/HEAD only, paths confined to `root`; never writes.
function staticServer(root, port) {
  const server = http.createServer((req, res) => {
    let rel = decodeURIComponent(req.url.split('?')[0].split('#')[0]);
    if (rel.endsWith('/')) rel += 'index.html';
    const file = path.join(root, path.normalize(rel).replace(/^(\.\.[/\\])+/, ''));
    if (!file.startsWith(root) || (req.method !== 'GET' && req.method !== 'HEAD')) { res.writeHead(404); return res.end(); }
    fs.stat(file, (err, st) => {
      if (err || !st.isFile()) { res.writeHead(404); return res.end(); }
      res.writeHead(200, { 'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream', 'Content-Length': st.size, 'Cache-Control': 'no-store' });
      if (req.method === 'HEAD') return res.end();
      fs.createReadStream(file).pipe(res);
    });
  });
  return new Promise((resolve) => server.listen(port, '127.0.0.1', () => resolve(server)));
}

function launchChrome(port, w, h, profile) {
  const bin = process.env.KN_CHROME || 'chromium-browser';
  const args = ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--hide-scrollbars', '--mute-audio', '--autoplay-policy=no-user-gesture-required', `--window-size=${w},${h}`,
    '--font-render-hinting=none', `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`, 'about:blank'];
  return spawn(bin, args, { stdio: 'ignore', detached: true });
}

// Before index.js runs: a returning guest (krunker_haslogin -> not a new user: full menu, no
// bot FTUE), consent given, settings left at their defaults.
const GUEST_STORAGE = { krunker_haslogin: 'true', consent: '1' };

// The sandbox chrome connectToSandbox adds (f2064 hides the top-left info, sub-logo buttons,
// news, header bars and the Maps/Challenges/Shop/Profile items) is undone, so the capture is
// the menu a guest connected to a server sees (fullMenu + those elements shown again).
const CONNECTED_MENU = `(() => {
  window.fullMenu(true);
  for (const id of ['tlInfHold','subLogoButtons','newsHolder','signedOutHeaderBar','headerRight']) { const e = document.getElementById(id); if (e) e.style.display = ''; }
  for (const id of ['streamContainer','topRightAdHolder','menuItemContainer']) { const e = document.getElementById(id); if (e) e.style.top = ''; }
  for (const id of ['menuBtnMaps','menuBtnChall','menuBtnShop','menuBtnProfile','menuBtnSocial','menuBtnMods']) { const e = document.getElementById(id); if (e) e.parentNode.classList.toggle('mIH', false); }
  const tl = document.getElementById('topLeftAdHolder'); if (tl) tl.style.display = 'none';
  return true; })()`;

async function openPage({ w = 1280, h = 720, port = 9461, httpPort = 6791, storage = GUEST_STORAGE, page = 'index.html', query = '', settle = 12000, log = () => {} } = {}) {
  const server = await staticServer(V7_PUBLIC, httpPort);
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'kn-v7-chrome-'));
  const chrome = launchChrome(port, w, h, profile);
  const c = await CDP.connect(port);
  await c.send('Runtime.enable');
  await c.send('Page.enable');
  const errors = [];
  c.on((m) => { if (m.method === 'Runtime.exceptionThrown') errors.push((m.params.exceptionDetails.exception || {}).description || m.params.exceptionDetails.text); });
  await c.setViewport(w, h);
  const base = `http://127.0.0.1:${httpPort}/`;
  await c.send('Page.navigate', { url: base + 'manifest.json' });
  await sleep(300);
  await c.eval(`(() => { localStorage.clear(); const s = ${JSON.stringify(storage)}; for (const k in s) localStorage.setItem(k, s[k]); return 1; })()`);
  await c.send('Page.navigate', { url: base + page + (query ? '?' + query : '') });
  await sleep(settle);
  log(`loaded ${page} (${errors.length} page errors)`);
  const close = () => { try { c.close(); } catch (e) { } try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { } server.close(); fs.rmSync(profile, { recursive: true, force: true }); };
  return { c, errors, close, base };
}

module.exports = { V7_PUBLIC, staticServer, launchChrome, openPage, CONNECTED_MENU, GUEST_STORAGE, sleep };
