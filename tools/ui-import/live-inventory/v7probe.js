#!/usr/bin/env node
// Dynamic inventory: run Krunker v7.1.4's own client (headless Chromium, read-only static serve of
// public/) on each map as the offline sandbox's custom map, intercept every request to the v7 asset
// hosts (assets.krunker.io, user-assets.krunker.io) and answer it from the local public/ tree.
// Records each requested asset path and whether the reference tree has it.
//   node v7probe.js <out.jsonl> <map.json>... (env SETTLE ms per map, CDP_PORT, HTTP_PORT, EXTRA=1)
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const { V7_PUBLIC, staticServer, launchChrome, GUEST_STORAGE, sleep } = require(path.join(__dirname, '..', 'v7_page.js'));
const { CDP } = require(path.join(__dirname, '..', '..', 'kref', 'cdp.js'));

const [out, ...maps] = process.argv.slice(2);
const SETTLE = +(process.env.SETTLE || 20000);
const cdpPort = +(process.env.CDP_PORT || 9581), httpPort = +(process.env.HTTP_PORT || 6881);
const MIME = { '.png': 'image/png', '.jpg': 'image/jpeg', '.webp': 'image/webp', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg',
  '.mp4': 'video/mp4', '.obj': 'text/plain', '.gltf': 'model/gltf+json', '.glb': 'model/gltf-binary', '.json': 'application/json',
  '.svg': 'image/svg+xml', '.gif': 'image/gif', '.fbx': 'application/octet-stream' };

(async () => {
  delete process.env.DISPLAY; delete process.env.WAYLAND_DISPLAY;
  const server = await staticServer(V7_PUBLIC, httpPort);
  const profile = fs.mkdtempSync(path.join(process.env.HOME, '.cache', 'kn-v7probe-chrome-'));
  const chrome = launchChrome(cdpPort, 1280, 720, profile);
  const c = await CDP.connect(cdpPort);
  await c.send('Runtime.enable'); await c.send('Page.enable'); await c.send('Network.enable');
  await c.send('Fetch.enable', { patterns: [{ urlPattern: '*://assets.krunker.io/*' }, { urlPattern: '*://user-assets.krunker.io/*' },
    { urlPattern: '*://krunker.io/*' }] });
  let cur = 'boot';
  const rec = fs.openSync(out, 'a');
  const seen = new Set();
  const note = (o) => { const k = cur + '|' + o.host + o.path; if (seen.has(k)) return; seen.add(k); fs.writeSync(rec, JSON.stringify({ map: cur, ...o }) + '\n'); };
  c.on(async (m) => {
    if (m.method === 'Network.responseReceived') {
      const u = new URL(m.params.response.url);
      if (u.hostname === '127.0.0.1') note({ host: 'local', path: decodeURIComponent(u.pathname), status: m.params.response.status, type: m.params.type });
      return;
    }
    if (m.method !== 'Fetch.requestPaused') return;
    const { requestId, request, resourceType } = m.params;
    const u = new URL(request.url);
    let rel = decodeURIComponent(u.pathname).replace(/^\/+/, '');
    if (u.hostname === 'user-assets.krunker.io') rel = 'user-assets/' + rel;
    const file = path.join(V7_PUBLIC, path.normalize(rel));
    let ok = false;
    try { ok = file.startsWith(V7_PUBLIC) && fs.statSync(file).isFile(); } catch (e) { }
    note({ host: u.hostname, path: rel, status: ok ? 200 : 404, type: resourceType });
    try {
      if (ok) {
        const body = fs.readFileSync(file);
        await c.send('Fetch.fulfillRequest', { requestId, responseCode: 200, body: body.toString('base64'), responseHeaders: [
          { name: 'Content-Type', value: MIME[path.extname(file).toLowerCase()] || 'application/octet-stream' },
          { name: 'Access-Control-Allow-Origin', value: '*' }] });
      } else {
        await c.send('Fetch.fulfillRequest', { requestId, responseCode: 404, body: '', responseHeaders: [{ name: 'Access-Control-Allow-Origin', value: '*' }] });
      }
    } catch (e) { }
  });
  await c.setViewport(1280, 720);
  const base = `http://127.0.0.1:${httpPort}/`;
  for (const map of maps) {
    cur = path.basename(map, '.json');
    const json = fs.readFileSync(map, 'utf8');
    await c.send('Page.navigate', { url: base + 'manifest.json' });
    await sleep(300);
    const storage = Object.assign({}, GUEST_STORAGE, { custMap: json });
    await c.eval(`(() => { localStorage.clear(); const s = ${JSON.stringify(storage)}; for (const k in s) localStorage.setItem(k, s[k]); return 1; })()`);
    await c.send('Page.navigate', { url: base + 'index.html' });
    await sleep(SETTLE);
    let info = '';
    try { info = await c.eval(`(() => { try { return (window.Game && Game.map && Game.map.name) || (document.getElementById('mapInfo')||{}).textContent || ''; } catch (e) { return 'err ' + e; } })()`); } catch (e) { info = 'eval ' + e.message; }
    console.log(cur, 'loaded:', String(info).slice(0, 60), 'records', seen.size);
  }
  fs.closeSync(rec);
  try { c.close(); } catch (e) { }
  try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { }
  server.close();
  fs.rmSync(profile, { recursive: true, force: true });
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(1); });
