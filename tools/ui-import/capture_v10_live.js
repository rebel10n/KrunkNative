#!/usr/bin/env node
// Rendered-DOM capture of the LIVE krunker.io (Krunker v10) menu in sandboxed headless Chromium.
// RUNS KRUNKER'S LIVE CLIENT (the page's own JS): only with the user's explicit approval. The
// static fallback, which runs nothing, is v10_bundle.js (import.py --ref v10).
//
//   node tools/ui-import/capture_v10_live.js <stage dir>
//
// Sandbox: a fresh profile <stage>/chrome-profile (deleted on exit), --headless=new (no display;
// DISPLAY / WAYLAND_DISPLAY are removed from Chromium's environment), every request through
// KN_FETCH_PROXY (default http://127.0.0.1:8888, --proxy-server), and Fetch-domain interception that
// fails every request except GET/HEAD to krunker.io / *.krunker.io and the CDN hosts in EXTRA_HOSTS
// (so no logins, purchases or analytics posts). No WebSocket is ever opened (matchmaker, game,
// queue, social): window.WebSocket throws, set before any page script. No cookies or tokens of
// ours: empty profile, Network.setBlockedURLs for analytics/ads, storage holds only the guest flags below.
//
// Output (idempotent: every file rewritten each run; <stage>/v10-capture/ is replaced):
//   <stage>/v10-capture/index.html          document.documentElement.outerHTML after boot (main menu)
//   <stage>/v10-capture/states/<name>.html  outerHTML of the changed container per state
//   <stage>/v10-capture/states/<name>.png   screenshot (1280x720) per state
//   <stage>/v10-capture/styles.css          every <style> element and adopted / constructed sheet
//                                           (CSSOM cssText), in document order, deduplicated
//   <stage>/v10-capture/windows.json        {id: {header, cls, style, html}} for showWindow(1..54)
//   <stage>/v10-capture/blocked.tsv         method  url  reason   (every request refused)
//   <stage>/v10-capture/allowed.tsv         method  url  status   (every request let through)
//   <stage>/v10-capture/capture.json        version, date, viewport, page errors, state list
// Needs chromium-browser (KN_CHROME=...) and ws (KN_NODE_MODULES or net-bridge/node_modules).
'use strict';
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const STAGE = process.argv[2];
if (!STAGE) { process.stderr.write('usage: capture_v10_live.js <stage dir>\n'); process.exit(2); }
const NM = process.env.KN_NODE_MODULES || path.join(__dirname, '../../net-bridge/node_modules');
const WebSocket = require(path.join(NM, 'ws'));
const PROXY = process.env.KN_FETCH_PROXY || 'http://127.0.0.1:8888';
const PORT = Number(process.env.CDP_PORT || 9477);
const W = 1280, H = 720;
const OUT = path.join(path.resolve(STAGE), 'v10-capture');
const PROFILE = path.join(path.resolve(STAGE), 'chrome-profile');
// third-party hosts the page needs to render (fonts / libs it links); everything else is refused
const EXTRA_HOSTS = ['fonts.googleapis.com', 'fonts.gstatic.com', 'cdnjs.cloudflare.com'];
const ALLOW_HOST = (h) => h === 'krunker.io' || h.endsWith('.krunker.io') || EXTRA_HOSTS.includes(h);
const BLOCK_URLS = ['*googletagmanager*', '*google-analytics*', '*doubleclick*', '*googlesyndication*', '*adinplay*',
  '*platform.twitter.com*', '*apis.google.com*', '*unpkg.com*', '*facebook*', '*hotjar*', '*sentry*'];
// guest flags only (the v7 capture's returning-guest state): full menu, no FTUE, consent given
const GUEST_STORAGE = { krunker_haslogin: 'true', consent: '1' };
// menu states: [name, expression run in the page, container whose outerHTML is kept]
const STATES = [
  ['main-menu', 'true', '#uiBase'],
  ['ranked', 'window.openRankedMenu && window.openRankedMenu(), true', '#genericPop, .rankedMenuModal, #popupHolder'],
  ['turf-wars', "(document.getElementById('menuBtnTurfWars') || {}).closest && document.getElementById('menuBtnTurfWars').closest('.menuItem').click(), true", '#genericPop, .defaultModal, #popupHolder'],
  ['leaderboards', 'window.openLeaderboards && window.openLeaderboards(), true', '#genericPop, .defaultModal, #popupHolder'],
  ['guide', 'window.openGuide && window.openGuide(), true', '#genericPop, .tutorialPopup, #popupHolder'],
  ['community', 'window.openCommunityMenu && window.openCommunityMenu(), true', '#genericPop, .communityPopup, #popupHolder'],
  ['settings', 'window.showWindow && window.showWindow(1), true', '#windowHolder'],
  ['customize', 'window.windows && (window.windows[2].tabIndex = 2), window.showWindow && window.showWindow(3), true', '#windowHolder'],
];
const CLOSE_ALL = "(() => { try { window.clearPops && window.clearPops(); } catch (e) {} try { window.closWind && window.closWind(); } catch (e) {} " +
  "document.querySelectorAll('#genericPop,#popupHolder').forEach((e) => { e.style.display = 'none'; }); return true; })()";
const SNAP_WINDOW = `(() => ({ header: (window.windowHeader || {}).innerHTML || '', cls: (window.menuWindow && menuWindow.getAttribute('class')) || '',
  style: (window.menuWindow && menuWindow.getAttribute('style')) || '', html: (window.menuWindow || {}).innerHTML || '',
  open: !!(window.windowHolder && windowHolder.style.display === 'block') }))()`;
// every stylesheet the document uses, as CSSOM text: <style> (Svelte's injected component CSS),
// constructed/adopted sheets; linked stylesheets are recorded by href (their files are mirrored)
const STYLES = `(() => { const out = [], seen = new Set();
  const add = (sh, tag) => { try { const t = [...sh.cssRules].map((r) => r.cssText).join('\\n'); if (!seen.has(t)) { seen.add(t); out.push('/* ' + tag + ' */\\n' + t); } } catch (e) { out.push('/* ' + tag + ': unreadable (' + e.name + ') */'); } };
  for (const sh of document.styleSheets) add(sh, sh.href ? 'link ' + sh.href : 'style #' + ((sh.ownerNode && sh.ownerNode.id) || '?'));
  for (const sh of document.adoptedStyleSheets || []) add(sh, 'adopted');
  return out.join('\\n'); })()`;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function getJSON(url) {
  return new Promise((res, rej) => require('http').get(url, (r) => { let d = ''; r.on('data', (c) => { d += c; }); r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } }); }).on('error', rej));
}
class Cdp {
  constructor(ws) {
    this.ws = ws; this.id = 0; this.pending = new Map(); this.handlers = [];
    ws.on('message', (m) => {
      const msg = JSON.parse(m.toString());
      if (msg.id && this.pending.has(msg.id)) { const { res, rej } = this.pending.get(msg.id); this.pending.delete(msg.id); if (msg.error) rej(new Error(JSON.stringify(msg.error))); else res(msg.result); }
      else if (msg.method) for (const h of this.handlers) h(msg);
    });
  }
  send(method, params = {}) { const id = ++this.id; this.ws.send(JSON.stringify({ id, method, params })); return new Promise((res, rej) => this.pending.set(id, { res, rej })); }
  async eval(expr) {
    const r = await this.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error((r.exceptionDetails.exception || {}).description || r.exceptionDetails.text);
    return r.result.value;
  }
}
(async () => {
  const log = (m) => process.stderr.write('[capture-v10] ' + m + '\n');
  fs.rmSync(OUT, { recursive: true, force: true });
  fs.mkdirSync(path.join(OUT, 'states'), { recursive: true });
  fs.rmSync(PROFILE, { recursive: true, force: true });
  fs.mkdirSync(PROFILE, { recursive: true });
  const env = { ...process.env }; delete env.DISPLAY; delete env.WAYLAND_DISPLAY;
  const chrome = spawn(process.env.KN_CHROME || 'chromium-browser', ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader',
    '--enable-unsafe-swiftshader', '--hide-scrollbars', '--mute-audio', '--no-first-run', '--no-default-browser-check', '--disable-sync',
    '--disable-extensions', '--disable-background-networking', '--disable-component-update', '--disable-domain-reliability',
    '--metrics-recording-only', '--password-store=basic', `--proxy-server=${PROXY}`, '--proxy-bypass-list=<-loopback>',
    `--window-size=${W},${H}`, `--remote-debugging-port=${PORT}`, '--remote-debugging-address=127.0.0.1', `--user-data-dir=${PROFILE}`, 'about:blank'],
  { stdio: 'ignore', detached: true, env });
  const blocked = [], allowed = [], errors = [];
  let c = null;
  const cleanup = () => {
    try { if (c) c.ws.close(); } catch (e) { /* closing */ }
    try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { /* gone */ }
    fs.rmSync(PROFILE, { recursive: true, force: true });
  };
  process.on('SIGINT', () => { cleanup(); process.exit(130); });
  try {
    let targets = [];
    for (let i = 0; i < 60 && !targets.length; i++) { try { targets = (await getJSON(`http://127.0.0.1:${PORT}/json/list`)).filter((t) => t.type === 'page'); } catch (e) { /* starting */ } if (!targets.length) await sleep(500); }
    if (!targets.length) throw new Error('chromium did not start');
    const ws = new WebSocket(targets[0].webSocketDebuggerUrl, { perMessageDeflate: false, maxPayload: 512 * 1024 * 1024 });
    await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
    c = new Cdp(ws);
    c.handlers.push((m) => {
      if (m.method === 'Fetch.requestPaused') {
        const p = m.params; const u = new URL(p.request.url); const meth = p.request.method;
        const why = !['http:', 'https:'].includes(u.protocol) ? 'scheme' : !ALLOW_HOST(u.hostname) ? 'host' : !['GET', 'HEAD'].includes(meth) ? 'method' : '';
        if (why) { blocked.push([meth, p.request.url, why]); c.send('Fetch.failRequest', { requestId: p.requestId, errorReason: 'BlockedByClient' }).catch(() => {}); }
        else c.send('Fetch.continueRequest', { requestId: p.requestId }).catch(() => {});
      } else if (m.method === 'Network.webSocketCreated') {
        blocked.push(['WS', m.params.url, 'websocket']); // Fetch cannot pause ws: the page's socket is closed below
      } else if (m.method === 'Network.responseReceived') {
        allowed.push(['GET', m.params.response.url, m.params.response.status]);
      } else if (m.method === 'Runtime.exceptionThrown') {
        errors.push(((m.params.exceptionDetails || {}).exception || {}).description || (m.params.exceptionDetails || {}).text);
      }
    });
    await c.send('Network.enable');
    await c.send('Network.setBlockedURLs', { urls: BLOCK_URLS });
    await c.send('Network.setCacheDisabled', { cacheDisabled: true });
    await c.send('Fetch.enable', { patterns: [{ urlPattern: '*', requestStage: 'Request' }] });
    await c.send('Runtime.enable'); await c.send('Page.enable');
    await c.send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
    // no WebSocket: neutered before any page script runs (matchmaker, game, queue, social)
    await c.send('Page.addScriptToEvaluateOnNewDocument', { source: `(() => { const s = {${Object.entries(GUEST_STORAGE).map(([k, v]) => JSON.stringify(k) + ':' + JSON.stringify(v)).join(',')}};
      try { for (const k in s) localStorage.setItem(k, s[k]); } catch (e) {}
      const Blocked = function () { throw new DOMException('blocked by capture_v10_live.js', 'SecurityError'); };
      Blocked.CONNECTING = 0; Blocked.OPEN = 1; Blocked.CLOSING = 2; Blocked.CLOSED = 3;
      Object.defineProperty(window, 'WebSocket', { value: Blocked, writable: false, configurable: false });
      navigator.sendBeacon = () => false; })()` });
    await c.send('Page.navigate', { url: 'https://krunker.io/' });
    const t0 = Date.now();
    while (Date.now() - t0 < 60000) { if (await c.eval("!!document.querySelector('#mainMenuUIMount .actionCard, #mainMenuUIMount #menuItemContainer')").catch(() => false)) break; await sleep(500); }
    await sleep(Number(process.env.SETTLE || 6000));
    fs.writeFileSync(path.join(OUT, 'index.html'), await c.eval('document.documentElement.outerHTML'));
    fs.writeFileSync(path.join(OUT, 'styles.css'), await c.eval(STYLES));
    const done = [];
    for (const [name, expr, sel] of STATES) {
      await c.eval(CLOSE_ALL).catch(() => {}); await sleep(500);
      let ok = true;
      try { await c.eval(expr); } catch (e) { ok = false; errors.push(name + ': ' + e.message); }
      await sleep(2500);
      const html = await c.eval(`(() => { for (const s of ${JSON.stringify(sel.split(', '))}) { const e = document.querySelector(s); if (e && e.offsetParent !== null) return e.outerHTML; } return ''; })()`).catch(() => '');
      fs.writeFileSync(path.join(OUT, 'states', name + '.html'), html);
      const shot = await c.send('Page.captureScreenshot', { format: 'png' });
      fs.writeFileSync(path.join(OUT, 'states', name + '.png'), Buffer.from(shot.data, 'base64'));
      done.push({ name, ok, bytes: html.length });
      log(`${name}: ${html.length} bytes`);
    }
    const windows = {};
    const n = await c.eval('(window.windows || []).length').catch(() => 0);
    for (let id = 1; id <= n; id++) {
      await c.eval(CLOSE_ALL).catch(() => {});
      try { await c.eval(`window.showWindow(${id}, true)`); await sleep(400); windows[id] = await c.eval(SNAP_WINDOW); } catch (e) { windows[id] = { error: e.message }; }
    }
    fs.writeFileSync(path.join(OUT, 'windows.json'), JSON.stringify(windows, null, 1));
    // the extra styles windows/popups injected after boot
    fs.writeFileSync(path.join(OUT, 'styles.css'), await c.eval(STYLES));
    fs.writeFileSync(path.join(OUT, 'capture.json'), JSON.stringify({ url: 'https://krunker.io/', date: new Date().toISOString(),
      gameVersion: await c.eval('window.gameVersion || (window.GAME && GAME.version) || null').catch(() => null),
      viewport: [W, H], states: done, windows: n, pageErrors: errors.slice(0, 200) }, null, 1));
  } finally {
    fs.writeFileSync(path.join(OUT, 'blocked.tsv'), blocked.map((r) => r.join('\t')).join('\n') + '\n');
    fs.writeFileSync(path.join(OUT, 'allowed.tsv'), allowed.map((r) => r.join('\t')).join('\n') + '\n');
    cleanup();
  }
  log(`done: ${OUT} (${blocked.length} requests blocked, ${allowed.length} allowed)`);
})().catch((e) => { process.stderr.write('[capture-v10] ' + (e.stack || e) + '\n'); process.exit(1); });
