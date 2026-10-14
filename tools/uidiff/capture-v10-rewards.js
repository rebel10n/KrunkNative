#!/usr/bin/env node
// The v10-rewards references (doc/ref/krunker-v10.0.2/rewards/, "captured from the offline v10
// sandbox"): tools/ui-import/capture_v10_live.js's OFFLINE mode (KN_V10_PUBLIC=<mirror public/>,
// the proxy on a dead port, no WebSocket, a temporary Chromium profile deleted on exit, every
// request answered from the local mirror, never a login) with these states instead of its own:
//   guest: showSignupRewards(), the "prr" popup (14 cosmetics + premium), showWindow(33) / (6),
//   the end screen's #matchEndSignupIncentives, windows[32].openHome() (the login popup);
//   signed in (v10's own "a" handler with tools/uidiff/season_fixture.json's account and pass,
//   reached through a paused frame of the bundle like capture-v7.js): the menu, the Challenges
//   window (home, Burg, Assault Rifle), the Battle Pass (home, Challenges tab, a claim, VIP).
// Mirror additions: gapi /data/skins = ~/.cache/kn-v10/live/api/skins.jspck, api /maps = the
// official map list (v10 fetches it with FRVR auth), assets.krunker.io = the live assets mirror
// then ui/v10 and ui/ (the images KrunkNative fetched).
//   KN_V10_PUBLIC=~/.cache/kn-v10/live/site/public node tools/uidiff/capture-v10-rewards.js <stage dir>
// Then copy <stage>/v10-capture/states/<name>.png to doc/ref/krunker-v10.0.2/rewards/.
'use strict';
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const STAGE = process.argv[2];
if (!STAGE) { process.stderr.write('usage: capture_v10_live.js <stage dir>\n'); process.exit(2); }
const NM = process.env.KN_NODE_MODULES || path.join(__dirname, '../../net-bridge/node_modules');
const FIXTURE = JSON.parse(fs.readFileSync(process.env.KN_FIXTURE || path.join(__dirname, 'season_fixture.json'), 'utf8'));
const WebSocket = require(path.join(NM, 'ws'));
const OFFLINE = process.env.KN_V10_PUBLIC ? path.resolve(process.env.KN_V10_PUBLIC) : '';
const CLIENT_JS = process.env.KN_V10_CLIENT_JS || path.join(require('os').homedir(), '.cache/kn-v10/v10.js');
const LIBS = process.env.KN_V10_LIBS || '/home/user/jails/kru/src/Krunker_v7.1.4/public/libs';
// offline: a closed port, so anything not fulfilled from disk fails instead of leaving the machine
const PROXY = OFFLINE ? 'http://127.0.0.1:9' : (process.env.KN_FETCH_PROXY || 'http://127.0.0.1:8888');
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp', '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg',
  '.wav': 'audio/wav', '.mp4': 'video/mp4', '.webm': 'video/webm', '.txt': 'text/plain', '.obj': 'text/plain', '.glb': 'model/gltf-binary' };
// offline: FRVR's SDK (libs/frvr-sdk.min.js, a .js the mirror never fetched) as an inert stand-in:
// logged out, no ads, no purchases, no tracking; every other member is a no-op function
const FRVR_STUB = `(() => { const noop = () => Promise.resolve(); const deep = (fixed) => new Proxy(fixed, { get: (t, k) => (k in t ? t[k] : (t[k] = deep(Object.assign(noop.bind(), {})))) });
  const none = () => Promise.resolve(null);
  window.FRVR = deep({ config: Object.assign({ abTests: {} }, window.FRVR && window.FRVR.config), init: () => Promise.resolve(), lifecycle: {},
    channel: deep({ getId: () => 'web' }), channelCharacteristics: { allowNavigation: true },
    auth: deep({ isLoggedIn: () => false, isLoggingIn: false, getFreshAccessToken: none, getCurrentPlatform: () => 'web', logout: none, tokenHandler: null }),
    iap: deep({ getUnconsumedPurchases: () => Promise.resolve([]), purchase: () => Promise.reject(new Error('offline')), consumePurchase: none }),
    ads: deep({ AdType: { Interstitial: 0, Reward: 1, Banner: 2 }, AdShowResult: { Shown: 0, Skipped: 1, Failed: 2 }, show: () => Promise.resolve(2) }),
    tracker: deep({}), bootstrapper: deep({}), abTests: { abTestCohorts: {} } }); })();`;
// the file (or [content]) an offline request is answered with ('' = 404)
const MAPS_JSON = JSON.stringify({ data: ['Burg', 'Littletown', 'Sandstorm', 'Subzero', 'Undergrowth', 'Shipment', 'Freight', 'Lostworld', 'Citadel',
  'Oasis', 'Kanji', 'Industry', 'Lumber', 'Evacuation', 'Site', 'SkyTemple', 'Lagoon', 'Bureau', 'Tortuga', 'Tropicano', 'Krunk_Plaza', 'Arena',
  'Habitat', 'Atomic', 'Old_Burg', 'Throwback', 'Stockade', 'Facility', 'Clockwork', 'Laboratory', 'Shipyard', 'Soul Sanctum'].map((name, i) => ({ id: i, name })) });
const HOME = require('os').homedir();
function offlineFile(u) {
  if (u.hostname === 'gapi.svc.krunker.io' && u.pathname === '/data/skins') return path.join(HOME, '.cache/kn-v10/live/api/skins.jspck');
  if (u.hostname === 'api.krunker.io' && u.pathname === '/maps') return [MAPS_JSON, '.json'];
  if (u.hostname === 'assets.krunker.io') {
    const a = path.join(HOME, '.cache/kn-v10/live/web/assets.krunker.io', decodeURIComponent(u.pathname));
    if (!a.includes('..') && fs.existsSync(a) && fs.statSync(a).isFile()) return a;
    const k = path.join(__dirname, '../../ui/v10', decodeURIComponent(u.pathname));
    if (!k.includes('..') && fs.existsSync(k) && fs.statSync(k).isFile()) return k;
    const k7 = path.join(__dirname, '../../ui', decodeURIComponent(u.pathname));
    if (!k7.includes('..') && fs.existsSync(k7) && fs.statSync(k7).isFile()) return k7;
  }
  let p = decodeURIComponent(u.pathname).replace(/^\/+/, '');
  if (p === '' || p.endsWith('/')) p += 'index.html';
  if (p.split('/').includes('..')) return '';
  if (p === 'libs/frvr-sdk.min.js') return [FRVR_STUB, '.js'];
  if (p === 'libs/frvr-channel-web.min.js') return ['', '.js'];
  if (/^static\/index-[^/]*\.js$/.test(p)) return CLIENT_JS;
  const f = path.join(OFFLINE, p);
  if (fs.existsSync(f) && fs.statSync(f).isFile()) return f;
  if (p.startsWith('libs/')) { const l = path.join(LIBS, p.slice(5)); if (fs.existsSync(l) && fs.statSync(l).isFile()) return l; }
  return '';
}
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
const STATES = [];
// v10-rewards: the guest and signed-in states of the signup rewards, challenges and battle pass
// (captured from the offline v10 sandbox)
const REWARD_IDS = [6316, 6315, 6268, 6259, 6260, 6288, 6304, 6285, 6287, 6305, 6286, 6284, 6303, 6302];
const GUEST_STATES = [
  ['signup-rewards', 'window.showSignupRewards(), true', '#genericPop'],
  ['register-rewards', `__kn.prr(${JSON.stringify(REWARD_IDS)}, true), true`, '#genericPop'],
  ['chall-guest', 'window.showWindow(33), true', '#windowHolder'],
  ['bp-guest', 'window.showWindow(6), true', '#windowHolder'],
  ['endscreen-guest', "(() => { __kn.mount(matchEndSignupIncentives, { origin: 'match_end' }); matchEndSignupIncentives.style.display = 'block'; " +
    "endUI.style.display = ''; endInfo.style.display = 'block'; uiBase.classList.add('onEndScrn'); menuHider.style.display = 'none'; uiBase.classList.remove('onMenu'); return true; })()", '#matchEndSignupIncentives'],
  ['chall-openhome-guest', 'windows[32].openHome(), true', '#genericPop, #popupHolder, body'],
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
  process.on('SIGTERM', () => { cleanup(); process.exit(143); });
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
        else if (OFFLINE) {
          const f = offlineFile(u);
          const stub = Array.isArray(f);
          const body = stub ? Buffer.from(f[0]) : f ? fs.readFileSync(f) : Buffer.from('');
          const ext = stub ? f[1] : f ? path.extname(f).toLowerCase() : '';
          const status = f ? 200 : 404;
          allowed.push([meth, p.request.url, status, stub ? 'stub' : f ? path.relative(OFFLINE, f).startsWith('..') ? f : 'mirror' : '']);
          c.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: status,
            responseHeaders: [{ name: 'Content-Type', value: MIME[ext] || 'application/octet-stream' },
              { name: 'Access-Control-Allow-Origin', value: '*' }],
            body: meth === 'HEAD' ? '' : body.toString('base64') }).catch(() => {});
        } else c.send('Fetch.continueRequest', { requestId: p.requestId }).catch(() => {});
      } else if (m.method === 'Network.webSocketCreated') {
        blocked.push(['WS', m.params.url, 'websocket']); // Fetch cannot pause ws: the page's socket is closed below
      } else if (m.method === 'Network.responseReceived') {
        if (!OFFLINE) allowed.push(['GET', m.params.response.url, m.params.response.status]);
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
    // the bundle is one IIFE: pause in one of its frames and keep closures over its scope (window.__kn)
    const exposeBundle = async () => {
      await c.send('Debugger.enable');
      const paused = new Promise((res) => c.handlers.push((m) => { if (m.method === 'Debugger.paused') res(m.params); }));
      await c.send('Debugger.pause');
      const p = await paused;
      const frame = p.callFrames.find((f) => /static\/index-[^/]*\.js/.test(f.url)) || p.callFrames[0];
      const expr = `(window.__kn = { login: function () { return _70182.apply(null, arguments); }, prr: function (a, b) { return _69816(a, b); },
        acc: function () { return _67362; }, mount: function (t, props) { return _4495(_39408, { target: t, props: props }); },
        syncBP: function (bp) { return _72494(bp); }, maps: function () { return _67367; } }, String(typeof _70182) + typeof _67362)`;
      const r = await c.send('Debugger.evaluateOnCallFrame', { callFrameId: frame.callFrameId, expression: expr, returnByValue: true });
      await c.send('Debugger.resume');
      await c.send('Debugger.disable');
      log('exposeBundle: ' + JSON.stringify(r && r.result ? r.result.value : r) + ' in ' + frame.url);
    };
    await exposeBundle();
    const shot = async (name, sel) => {
      await sleep(2500);
      const html = await c.eval(`(() => { for (const s of ${JSON.stringify(sel.split(', '))}) { const e = document.querySelector(s); if (e && e.offsetParent !== null) return e.outerHTML; } return ''; })()`).catch(() => '');
      fs.writeFileSync(path.join(OUT, 'states', name + '.html'), html);
      const win = await c.eval(SNAP_WINDOW).catch(() => null);
      if (win && win.open) fs.writeFileSync(path.join(OUT, 'states', name + '.window.json'), JSON.stringify(win, null, 1));
      const png = await c.send('Page.captureScreenshot', { format: 'png' });
      fs.writeFileSync(path.join(OUT, 'states', name + '.png'), Buffer.from(png.data, 'base64'));
      done.push({ name, bytes: html.length });
      log(`${name}: ${html.length} bytes`);
    };
    const run = async (name, expr, sel) => {
      await c.eval(CLOSE_ALL).catch(() => {}); await sleep(500);
      try { const v = await c.eval(`(() => { try { return ${expr}; } catch (e) { return 'ERR ' + e.stack; } })()`); if (v !== true && v !== 1) errors.push(name + ': ' + v); } catch (e) { errors.push(name + ': ' + e.message); }
      await shot(name, sel);
    };
    // the official map list (v10 fetches it from api.krunker.io/maps, which needs FRVR auth offline)
    log('maps: ' + await c.eval(`(() => { const m = __kn.maps(); if (!m.length) JSON.parse(${JSON.stringify(MAPS_JSON)}).data.forEach((x) => m.push(x)); return m.length; })()`));
    for (const [name, expr, sel] of GUEST_STATES) await run(name, expr, sel);
    await c.eval("(() => { matchEndSignupIncentives.style.display = 'none'; endUI.style.display = 'none'; endInfo.style.display = 'none'; uiBase.classList.remove('onEndScrn'); uiBase.classList.add('onMenu'); menuHider.style.display = 'block'; return 1; })()").catch(() => {});
    // signed in: v10's own "a" handler (_70182) with the Account data tuple (setData indices) and
    // the season fixture; the pass's dates keep their distance to the fixture's "now"
    const acc = FIXTURE, bp = acc.battlepass;
    const nowMs = await c.eval('Date.now()');
    const shift = nowMs - bp.now * 1000;
    const data = Object.assign({}, bp.data, { startdate: new Date(Date.parse(bp.data.startdate) + shift).toISOString(),
      enddate: new Date(Date.parse(bp.data.enddate) + shift).toISOString() });
    const tuple = new Array(56).fill(null);
    Object.assign(tuple, { 0: 1, 1: 0, 2: 0, 3: 0, 4: 0, 5: acc.funds || 0, 6: acc.score || 0, 7: acc.clan || null, 8: 0, 9: 0, 10: [], 11: 0,
      12: 0, 13: 0, 14: 0, 15: acc.stats || {}, 16: 0, 19: acc.premium || 0, 21: acc.name, 37: 0, 38: acc.challenges || [], 39: acc.badges || [],
      41: bp.progress.bpp, 47: 0, 55: [] });
    const login = await c.eval(`(() => { try { closWind(); __kn.login(null, 1, ${JSON.stringify(acc.name)}, ${JSON.stringify(tuple)}, null, {}); } catch (e) { return 'login ' + e.stack; }
      try { windows[5].dataLoaded(${JSON.stringify(data)}); __kn.syncBP(${JSON.stringify(bp.progress)}); } catch (e) { return 'bp ' + e.stack; } return 1; })()`);
    log('login: ' + login);
    fs.writeFileSync(path.join(OUT, 'now.json'), JSON.stringify({ captureNowMs: nowMs, fixtureNow: bp.now, shiftMs: shift }));
    const vip = JSON.stringify(Object.assign({}, bp.progress, { tier: 1, items_claimed: '20001,21001,21004' }));
    const ACC_STATES = [
      ['menu-account', 'true', '#uiBase'],
      ['chall-home', 'windows[32].openChallenge = null, showWindow(33), true', '#windowHolder'],
      ['chall-map', "showWindow(33), windows[32].openChallengeCat('map', 0), true", '#windowHolder'],
      ['chall-weapon', "showWindow(33), windows[32].openChallengeCat('weapon', 1), true", '#windowHolder'],
      ['chall-openhome', 'windows[32].openHome(), true', '#windowHolder'],
      ['bp-home', 'windows[5].tabIndex = 0, showWindow(6), true', '#windowHolder'],
      ['bp-chal', 'windows[5].tabIndex = 1, showWindow(6), true', '#windowHolder'],
      ['bp-claimed', "windows[5].tabIndex = 0, showWindow(6), windows[5].claimItemRes(null, 2500, null, null, 0, [{ id: '20004', tier: '0', col: '3', sk: 6315 }]), true", '#genericPop, #popupHolder, #windowHolder'],
      ['bp-vip', `__kn.syncBP(${vip}), windows[5].tabIndex = 0, showWindow(6), true`, '#windowHolder'],
    ];
    for (const [name, expr, sel] of ACC_STATES) await run(name, expr, sel);
    // the extra styles windows/popups injected after boot
    fs.writeFileSync(path.join(OUT, 'styles.css'), await c.eval(STYLES));
    fs.writeFileSync(path.join(OUT, 'capture.json'), JSON.stringify({ url: 'https://krunker.io/', date: new Date().toISOString(),
      mode: OFFLINE ? 'offline' : 'live', mirror: OFFLINE || null, clientJs: OFFLINE ? CLIENT_JS : null,
      gameVersion: await c.eval('window.gameVersion || (window.GAME && GAME.version) || null').catch(() => null),
      viewport: [W, H], states: done, windows: 0, pageErrors: errors.slice(0, 200) }, null, 1));
  } finally {
    fs.writeFileSync(path.join(OUT, 'blocked.tsv'), blocked.map((r) => r.join('\t')).join('\n') + '\n');
    fs.writeFileSync(path.join(OUT, 'allowed.tsv'), allowed.map((r) => r.join('\t')).join('\n') + '\n');
    cleanup();
  }
  log(`done: ${OUT} (${blocked.length} requests blocked, ${allowed.length} allowed)`);
})().catch((e) => { process.stderr.write('[capture-v10] ' + (e.stack || e) + '\n'); process.exit(1); });
