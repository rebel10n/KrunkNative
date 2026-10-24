#!/usr/bin/env node
// Krunker v10.0.2 references for the server browser and the official-lobby states of
// client/src/ui/servers_ui.c (doc/ref/krunker-v10.0.2/servers/), taken from the OFFLINE v10 sandbox. The
// sandbox rules are capture-v10-popups.js's, kept exactly: a fresh profile per capture, --headless=new, no
// DISPLAY, the proxy on a closed port, every GET to krunker.io answered from the local mirror and the rest
// refused, no WebSocket, no beacons, a seeded Math.random, CSS animations paused at 800 ms. The one
// difference: matchmaker.krunker.io's GET /game-list answers with a fixture's lobby list, the
// "krunkerLobbies" object of a uidiff scenario (--fixture, default tools/uidiff/scenarios/kn-servers-official.json),
// the same list the native scenario renders. v10's default region (localStorage sid-utils:ping-region, the
// Settings > General "Default Region" select, v10:203284) is set to us-ca-sv, absent from the fixture, so no
// group is hoisted as "order 1".
//
//   node tools/uidiff/capture-v10-servers.js [--out doc/ref/krunker-v10.0.2] [--fixture <scenario.json>] [--list] [name...]
//
// What each reference is:
//   servers-official       showWindow(2) once loadData's game list arrived and the window re-rendered
//                          (windows[1].loadedServers, v10:211167-211240): every region group collapsed
//   servers-official-open  the same, then window.setExpandedRegion(i) (v10:211456) on the New York (us-nj) group
//   servers-official-hidefull  Hide Full on (toggleHF(true)) and the Frankfurt group open: its full 8/8 lobby is gone
//   lost-connection-official   ?game=NY:zq9x7 with /seek-game answering 400 {"error":"InvalidGameId"}: v10's own
//                          seek catch (v10:222077-222085) translates matchmaker.InvalidGameId ("Game not found.")
//                          and calls _73848 -> _73828, the Lost Connection box (v10:221852)
//   connecting-official    ?game=NY:b760t with /seek-game never answered: the page while the seek is pending
//   servers-carousel       kn-servers-carousel.json's list (custom games on published maps, c + cm): v10's carousel
//                          (customsInfo, PureJSCarousel), its user-assets thumbnails answered from the fixture's
//                          remoteImages files
//   menu-popular-official  the main menu with that list: Popular Now's top 3 custom maps (krunker.io's games)
//   invite-copied          the menu's match bar Invite button (#inviteButton, copyInviteLink v10:216889):
//                          "Copied URL" for 1250 ms; captured at 300 ms
// The region picker is Settings > General > Localization "Default Region" (v10:203284): covered by the
// settings-general scenario's doc/ref/krunker-v10/settings/settings-basic-0.png, not captured here.
//
// Run: env -u DISPLAY -u WAYLAND_DISPLAY KN_CHROME=/usr/bin/chromium-browser node tools/uidiff/capture-v10-servers.js
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawn } = require('child_process');
const { CDP, sleep } = require('../kref/cdp.js');

const REPO = path.resolve(__dirname, '..', '..');
const HOME = os.homedir();
const OFFLINE = path.resolve(process.env.KN_V10_PUBLIC || path.join(HOME, '.cache/kn-v10/live/site/public'));
const CLIENT_JS = process.env.KN_V10_CLIENT_JS || path.join(HOME, 'jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js');
const V7_PUBLIC = process.env.KN_V7_PUBLIC || '/home/user/jails/kru/src/Krunker_v7.1.4/public';
const ASSET_DIRS = [path.join(HOME, '.cache/kn-v10/live/stage/assets'), path.join(HOME, '.cache/kn-v10/live/web/assets.krunker.io'),
  path.join(REPO, 'assets', 'v10'), path.join(REPO, 'assets'), V7_PUBLIC];
const PROXY = 'http://127.0.0.1:9'; // closed: nothing unanswered leaves the machine
const ANIM_MS = 800;
let OUT = path.join(REPO, 'doc', 'ref', 'krunker-v10.0.2');
let FIXTURE = path.join(REPO, 'tools', 'uidiff', 'scenarios', 'kn-servers-official.json');
let LOBBIES = null; // the fixture's krunkerLobbies ({games, totalPlayerCount})
let REMOTE = {};    // the fixture's remoteImages ({url: repo-relative file}): user-assets thumbnails
let SEEK = null;    // this capture's /seek-game answer: null = 404, 'hang' = never, else [status, body]
let SEEK_SEEN = 0;  // /seek-game requests this capture
let PING_LIST = null; // this capture's /ping-list answer (region -> ping host), null = 404

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp', '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg',
  '.wav': 'audio/wav', '.mp4': 'video/mp4', '.webm': 'video/webm', '.txt': 'text/plain', '.obj': 'text/plain', '.glb': 'model/gltf-binary',
  '.gltf': 'model/gltf+json' };
const FRVR_STUB = `(() => { const noop = () => Promise.resolve(); const deep = (fixed) => new Proxy(fixed, { get: (t, k) => (k in t ? t[k] : (t[k] = deep(Object.assign(noop.bind(), {})))) });
  const none = () => Promise.resolve(null);
  window.FRVR = deep({ config: Object.assign({ abTests: {} }, window.FRVR && window.FRVR.config), init: () => Promise.resolve(), lifecycle: {},
    channel: deep({ getId: () => 'web' }), channelCharacteristics: { allowNavigation: true },
    auth: deep({ isLoggedIn: () => false, isLoggingIn: false, getFreshAccessToken: none, getCurrentPlatform: () => 'web', logout: none,
      tokenHandler: deep({ getRefreshToken: () => null, getAccessToken: () => null }) }),
    iap: deep({ getUnconsumedPurchases: () => Promise.resolve([]), purchase: () => Promise.reject(new Error('offline')), consumePurchase: none }),
    ads: deep({ AdType: { Interstitial: 0, Reward: 1, Banner: 2 }, AdShowResult: { Shown: 0, Skipped: 1, Failed: 2 }, show: () => Promise.resolve(2) }),
    tracker: deep({}), bootstrapper: deep({}), abTests: { abTestCohorts: {} } }); })();`;
let CURRENT_MAP = 'null'; // the capture's map JSON (openPage)
const isFile = (f) => { try { return fs.statSync(f).isFile(); } catch (e) { return false; } };
// the file (or [content, ext]) an offline request is answered with ('' = 404)
function offlineFile(u) {
  let p = decodeURIComponent(u.pathname).replace(/^\/+/, '');
  if (p === '' || p.endsWith('/')) p += 'index.html';
  if (p.split('/').includes('..')) return '';
  if (p === 'libs/frvr-sdk.min.js') return [FRVR_STUB, '.js'];
  if (p === 'libs/frvr-channel-web.min.js') return ['', '.js'];
  if (/^static\/index-[^/]*\.js$/.test(p)) return [clientScript(), '.js'];
  if (/^(gapi\.svc|api)\.krunker\.io$/.test(u.hostname) && p === 'maps') return ['{"data":[' + CURRENT_MAP + ']}', '.json'];
  // user-assets (the carousel's map thumbnails): the fixture's own files, the ones the native render reads
  if (REMOTE[u.origin + u.pathname]) return path.join(REPO, REMOTE[u.origin + u.pathname]);
  if (u.hostname === 'matchmaker.krunker.io' && p === 'game-list' && LOBBIES) return [JSON.stringify(Object.assign({ success: true }, LOBBIES)), '.json'];
  // loadData re-renders the window only once fetchGameInfo(<current game>) resolves (v10:211235): a
  // guest in no game has none, so the answer is null (no private server row)
  if (u.hostname === 'matchmaker.krunker.io' && p === 'game-info' && LOBBIES) return ['null', '.json'];
  // a seek capture: v10's getRegion pings the listed host (v10:147470-147500); any answer (the mirror's 404
  // too) resolves the ping, so the region is the listed one
  if (u.hostname === 'matchmaker.krunker.io' && p === 'ping-list' && PING_LIST) return [JSON.stringify(PING_LIST), '.json'];
  if (/^(gapi\.svc|api|matchmaker)\.krunker\.io$/.test(u.hostname)) return '';
  const f = path.join(OFFLINE, p);
  if (isFile(f)) return f;
  if (p.startsWith('libs/')) { const l = path.join(V7_PUBLIC, p); if (isFile(l)) return l; }
  for (const d of ASSET_DIRS) { const a = path.join(d, p); if (isFile(a)) return a; }
  return '';
}
const ALLOW_HOST = (h) => h === 'krunker.io' || h.endsWith('.krunker.io');
const BLOCK_URLS = ['*googletagmanager*', '*google-analytics*', '*doubleclick*', '*googlesyndication*', '*adinplay*',
  '*platform.twitter.com*', '*apis.google.com*', '*unpkg.com*', '*facebook*', '*hotjar*', '*sentry*'];

// the map the sandbox boots with (capture-v10.js's default: v7.1.4's kanji.json narrowed to Free for All)
function mapJson() {
  const m = JSON.parse(fs.readFileSync(path.join(V7_PUBLIC, 'maps', 'kanji.json'), 'utf8'));
  m.config = Object.assign({}, m.config, { modes: [0] });
  return JSON.stringify(m);
}

function preScript(storage) {
  return `(() => { const s = ${JSON.stringify(storage)};
    try { for (const k in s) { if (s[k] === null) localStorage.removeItem(k); else localStorage.setItem(k, s[k]); } } catch (e) {}
    window.__knMap = s.custMap;
    const Blocked = function () { throw new DOMException('blocked by capture-v10-servers.js', 'SecurityError'); };
    Blocked.CONNECTING = 0; Blocked.OPEN = 1; Blocked.CLOSING = 2; Blocked.CLOSED = 3;
    Object.defineProperty(window, 'WebSocket', { value: Blocked, writable: false, configurable: false });
    navigator.sendBeacon = () => false;
    let a = 0x4b4e; Math.random = function () { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a);
      t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };
    Object.defineProperty(document, 'pointerLockElement', { configurable: true, get: () => window.__knLock || null });
    HTMLCanvasElement.prototype.requestPointerLock = function () { window.__knLock = this; setTimeout(() => document.dispatchEvent(new Event('pointerlockchange')), 0); };
    document.exitPointerLock = function () { window.__knLock = null; setTimeout(() => document.dispatchEvent(new Event('pointerlockchange')), 0); };
  })()`;
}

function launchChrome(port, w, h, profile) {
  const env = { ...process.env }; delete env.DISPLAY; delete env.WAYLAND_DISPLAY;
  return spawn(process.env.KN_CHROME || 'chromium-browser', ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader',
    '--enable-unsafe-swiftshader', '--hide-scrollbars', '--mute-audio', '--no-first-run', '--no-default-browser-check', '--disable-sync',
    '--disable-extensions', '--disable-background-networking', '--disable-component-update', '--disable-domain-reliability',
    '--metrics-recording-only', '--password-store=basic', '--font-render-hinting=none', `--proxy-server=${PROXY}`, '--proxy-bypass-list=<-loopback>',
    '--autoplay-policy=no-user-gesture-required', `--window-size=${w},${h}`, `--remote-debugging-port=${port}`, '--remote-debugging-address=127.0.0.1',
    `--user-data-dir=${profile}`, 'about:blank'], { stdio: 'ignore', detached: true, env });
}

// ---------------------------------------------------------------- page helpers

const flatPage = (bg) => `(() => { let best = null; for (const cv of document.querySelectorAll('canvas')) {
  if (['game-overlay', 'miniMapCanvas'].includes(cv.id) || cv.closest('#uiBase')) continue;
  if (!best || cv.width * cv.height > best.width * best.height) best = cv; }
  if (best) best.style.opacity = '0';
  document.documentElement.style.background = '${bg}'; document.body.style.background = '${bg}'; return 1; })()`;
// pause every animation at `ms` (the reference convention: capture-v10.js's PAUSE_ANIMS at 800 ms)
const PAUSE_AT = (ms) => `(() => { let n = 0; for (const a of document.getAnimations()) { try { a.pause(); a.currentTime = ${ms}; n++; } catch (e) {} } return n; })()`;
async function settle(c, ms = 600) {
  await sleep(ms);
  await c.eval(PAUSE_AT(ANIM_MS));
  await sleep(150);
}

// the client script with the scope accessor (capture-v10.js's SCOPE_HOOK, the same anchor): window.__knScope()
// reaches the sandbox game and setAcc(a), which stands a signed-in account in for the guest
const SCOPE_ANCHOR = '  var _69422 = null;\n  var _69423 = null;\n';
const SCOPE_HOOK = '  window.__knScope = function () { return { sg: _69422, sp: _69423, cg: _67298, me: _69415, sock: _33978,'
  + ' setAcc: function (a) { _67362 = a; }, boot: function (x) { return _73861(x); }, survey: function (p) { return _70256(p); },'
  + ' ui: _57154, clearError: function () { return _73837(); } }; };\n';
let clientJs = null;
function clientScript() {
  if (clientJs) return clientJs;
  const src = fs.readFileSync(CLIENT_JS, 'utf8');
  if (!src.includes(SCOPE_ANCHOR)) throw new Error('capture-v10-servers: the scope anchor is not in ' + CLIENT_JS);
  return (clientJs = Buffer.from(src.replace(SCOPE_ANCHOR, SCOPE_ANCHOR + SCOPE_HOOK)));
}

// the guest menu (no custMap: no sandbox game), as capture_v10_live.js's offline mode boots it
async function waitMenu(c) {
  if (process.env.KN_SANDBOX) return waitSandbox(c);
  for (let i = 0; i < 60; i++) {
    if (await c.eval(`!!document.querySelector('#mainMenuUIMount .actionCard, #mainMenuUIMount #menuItemContainer')`).catch(() => false)) { await sleep(1500); return 1; }
    await sleep(500);
  }
  throw new Error('the v10 menu did not boot');
}

// The offline boot's own seek fails (no matchmaker: "Unknown Join Issue" on the Lost Connection box). The
// menu references want the plain menu behind: _73837 (v10:221899) is v10's own reset of that box, and
// the menu its hideUI / _73828 hid comes back (toggleMenu(true), #menuHolder).
async function clearBootError(c) {
  if (process.env.KN_SANDBOX) return;
  const r = await c.eval(`(() => { try { const k = window.__knScope(); k.clearError(); k.ui.toggleMenu(!0); menuHolder.style.display = 'block'; return 1; } catch (e) { return String(e && e.message || e); } })()`);
  if (r !== 1) console.error('[capture-v10-servers] clearError: ' + r);
}

async function waitSandbox(c) {
  for (let i = 0; i < 60; i++) {
    if (await c.eval(`!!(window.__knScope && window.__knScope().sg && /CLICK TO PLAY/i.test(instructions.innerText))`).catch(() => false)) return 1;
    await sleep(500);
  }
  throw new Error('the v10 sandbox did not start');
}

const log = (m) => { if (process.env.KN_CAPTURE_DEBUG) console.error('[capture-v10-servers] ' + m); };
const CLOSERS = new Set();
for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) process.on(sig, () => { for (const f of CLOSERS) f(); process.exit(130); });
process.on('exit', () => { for (const f of CLOSERS) f(); });

async function openPage(o) {
  const profile = fs.mkdtempSync(path.join(HOME, '.cache', 'kn-v10ref-chrome-'));
  const chrome = launchChrome(o.port, o.w, o.h, profile);
  let c = null;
  const close = () => { CLOSERS.delete(close); try { c && c.close(); } catch (e) { } try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { } fs.rmSync(profile, { recursive: true, force: true }); };
  CLOSERS.add(close);
  const errors = [];
  try {
    log('chrome ' + chrome.pid); c = await CDP.connect(o.port); log('cdp connected');
    c.on((m) => {
      if (m.method === 'Fetch.requestPaused') {
        const p = m.params; const u = new URL(p.request.url); const meth = p.request.method;
        const why = !['http:', 'https:'].includes(u.protocol) ? 'scheme' : !ALLOW_HOST(u.hostname) ? 'host' : !['GET', 'HEAD'].includes(meth) ? 'method' : '';
        if (why) { c.send('Fetch.failRequest', { requestId: p.requestId, errorReason: 'BlockedByClient' }).catch(() => {}); return; }
        if (u.hostname === 'matchmaker.krunker.io') log('matchmaker ' + u.pathname + u.search);
        if (u.hostname === 'matchmaker.krunker.io' && u.pathname === '/seek-game') SEEK_SEEN++;
        if (u.hostname === 'matchmaker.krunker.io' && u.pathname === '/seek-game' && SEEK) {
          if (SEEK === 'hang') return; // never answered: the seek stays pending
          c.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: SEEK[0], responseHeaders: [{ name: 'Content-Type', value: 'application/json' },
            { name: 'Access-Control-Allow-Origin', value: '*' }], body: Buffer.from(SEEK[1]).toString('base64') }).catch(() => {});
          return;
        }
        const f = offlineFile(u);
        const stub = Array.isArray(f);
        const body = stub ? Buffer.from(f[0]) : f ? fs.readFileSync(f) : Buffer.from('');
        const ext = stub ? f[1] : f ? path.extname(f).toLowerCase() : '';
        c.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: f ? 200 : 404,
          responseHeaders: [{ name: 'Content-Type', value: MIME[ext] || 'application/octet-stream' }, { name: 'Access-Control-Allow-Origin', value: '*' }],
          body: meth === 'HEAD' ? '' : body.toString('base64') }).catch(() => {});
      } else if (m.method === 'Runtime.exceptionThrown') {
        errors.push(((m.params.exceptionDetails || {}).exception || {}).description || (m.params.exceptionDetails || {}).text);
      }
    });
    await c.send('Network.enable');
    await c.send('Network.setBlockedURLs', { urls: BLOCK_URLS });
    await c.send('Network.setCacheDisabled', { cacheDisabled: true });
    await c.send('Fetch.enable', { patterns: [{ urlPattern: '*', requestStage: 'Request' }] });
    await c.send('Runtime.enable');
    await c.send('Page.enable');
    await c.send('Emulation.setDeviceMetricsOverride', { width: o.w, height: o.h, deviceScaleFactor: 1, mobile: false });
    CURRENT_MAP = mapJson();
    const storage = Object.assign({ krunker_haslogin: 'true', consent: '1', custMap: CURRENT_MAP }, o.storage || {});
    await c.send('Page.addScriptToEvaluateOnNewDocument', { source: preScript(storage) });
    log('navigate'); await c.send('Page.navigate', { url: 'https://krunker.io/' + (o.query || '') });
    await sleep(o.settle);
  } catch (e) { close(); throw e; }
  return { c, errors, close };
}

// ---------------------------------------------------------------- scenarios

const S = {};
function scenario(name, run, opts = {}) { S[name] = Object.assign({ name, run, size: [1280, 720] }, opts); }
const STORAGE = { 'sid-utils:ping-region': 'us-ca-sv', custMap: process.env.KN_SANDBOX ? undefined : null };

// showWindow(2), then wait for loadData's list (loadedServers) and the window's re-render with the groups
async function openServers(c) {
  await waitMenu(c);
  await clearBootError(c);
  await c.eval(flatPage('#000000'));
  await c.eval('(() => { showWindow(2); return 1; })()');
  for (let i = 0; i < 40; i++) {
    const ok = await c.eval(`!!(windows[1].loadedServers && document.querySelector('#serverHolder .setHed'))`).catch(() => false);
    if (ok) break;
    await sleep(250);
  }
  await c.eval('(() => { showWindow(2, !0); return 1; })()'); // gen() within serverBrowserRate: genList()
  await sleep(600);
  const groups = await c.eval(`JSON.stringify([...document.querySelectorAll('#serverHolder .setHed')].map((e) => e.innerText.replace(/\\s+/g, ' ').trim()))`);
  console.error('[capture-v10-servers] groups: ' + groups + ' (default region ' + await c.eval(`String(localStorage.getItem('sid-utils:ping-region'))`) + ')');
  return JSON.parse(groups);
}
async function expandNY(c, groups) {
  const i = groups.findIndex((g) => /New York/.test(g));
  if (i < 0) throw new Error('no New York group: ' + groups.join(' | '));
  await c.eval(`(() => { setExpandedRegion(${i}); return 1; })()`);
  await sleep(500);
  console.error('[capture-v10-servers] New York = group ' + i + '; rows: ' + await c.eval(`JSON.stringify([...document.querySelectorAll('#serverHolder .settNameIn')].map((e) => e.innerText.replace(/\\s+/g, ' ').trim()))`));
}

scenario('servers-official', async (c) => { await openServers(c); await settle(c, 600); });
scenario('servers-official-open', async (c) => { await expandNY(c, await openServers(c)); await settle(c, 600); });
scenario('servers-official-hidefull', async (c) => {
  const groups = await openServers(c);
  await c.eval(`(() => { toggleHF(true); showWindow(2, !0); return 1; })()`); // the header's switch follows krk_hideFull
  await sleep(400);
  const i = groups.findIndex((g) => /Frankfurt/.test(g)); // FRA:k8m2p is 8/8
  await c.eval(`(() => { setExpandedRegion(${i}); return 1; })()`);
  await sleep(500);
  console.error('[capture-v10-servers] Frankfurt, Hide Full: ' + await c.eval(`JSON.stringify([...document.querySelectorAll('#serverHolder .settNameIn')].map((e) => e.innerText.replace(/\\s+/g, ' ').trim()))`));
  await settle(c, 600);
});
// krunker.io's custom games on published maps (c + cm): loadData's customsInfo (v10:211196-211234) and post()'s
// PureJSCarousel (v10:211514) over the slides, the thumbnails from user-assets (the fixture's remoteImages)
scenario('servers-carousel', async (c) => {
  await openServers(c);
  for (let i = 0; i < 40; i++) {
    if (await c.eval(`!!(document.querySelector('#carousel-infinite.purejscarousel') && [...document.querySelectorAll('#carousel-infinite .slideImg')].every((im) => im.complete && im.naturalWidth > 0))`).catch(() => false)) break;
    await sleep(250);
  }
  console.error('[capture-v10-servers] carousel: ' + await c.eval(`JSON.stringify({ slides: document.querySelectorAll('#carousel-infinite .slide').length,
    names: [...document.querySelectorAll('#carousel-infinite .purejscarousel-slide:not(.purejscarousel-slide-clone) .slideName')].map((e) => e.textContent),
    container: (document.querySelector('.purejscarousel-slides-container') || {}).getAttribute && document.querySelector('.purejscarousel-slides-container').getAttribute('style') })`));
  await settle(c, 600);
}, { fixture: path.join(REPO, 'tools', 'uidiff', 'scenarios', 'kn-servers-carousel.json') });
// the main menu with krunker.io's custom games in the game list: Popular Now (_52548 v10:147200, fetchMatches
// v10:215632) over the same list as servers-carousel
scenario('menu-popular-official', async (c) => {
  await waitMenu(c);
  await clearBootError(c);
  await c.eval(flatPage('#222222'));
  for (let i = 0; i < 40; i++) {
    if (await c.eval(`!!document.querySelector('#subLogoButtons > .popRail')`).catch(() => false)) break;
    await sleep(250);
  }
  console.error('[capture-v10-servers] popular: ' + await c.eval(`JSON.stringify([...document.querySelectorAll('.popRailCard')].map((e) => e.innerText.replace(/\\s+/g, ' ').trim()))`));
  await settle(c, 600);
}, { fixture: path.join(REPO, 'tools', 'uidiff', 'scenarios', 'kn-servers-carousel.json') });
// ?game=<expired id>: v10's boot seek fails with InvalidGameId -> "Game not found." on the Lost Connection box
scenario('lost-connection-official', async (c) => {
  for (let i = 0; i < 60; i++) {
    if (await c.eval(`!!(window.instructionsUpdate && instructionsUpdate.style.display === 'block' && /Lost Connection/.test(instructionsUpdate.innerText))`).catch(() => false)) break;
    await sleep(250);
  }
  console.error('[capture-v10-servers] error box: ' + JSON.stringify(await c.eval(`instructionsUpdate.innerText`).catch(() => '?')));
  await c.eval(flatPage('#222222'));
  await settle(c, 600);
}, { query: '?game=NY:zq9x7', seek: [400, '{"error":"InvalidGameId"}'], pingList: { 'us-nj': 'ping-nj.krunker.io:443' } });
// ?game=<id> with the seek pending: what v10 shows while it waits for the matchmaker
scenario('connecting-official', async (c) => {
  for (let i = 0; i < 80 && !SEEK_SEEN; i++) await sleep(250);
  if (!SEEK_SEEN) throw new Error('connecting-official: the seek never started');
  await sleep(1500);
  console.error('[capture-v10-servers] pending: ' + JSON.stringify(await c.eval(`JSON.stringify({ instr: (window.instructions || {}).innerText, load: (window.loadGamNm || {}).innerHTML,
    loadingBg: window.loadingBg && getComputedStyle(loadingBg).display, menu: window.menuHolder && getComputedStyle(menuHolder).display })`).catch(() => '?')));
  await c.eval(flatPage('#222222'));
  await settle(c, 600);
}, { query: '?game=NY:b760t', seek: 'hang', pingList: { 'us-nj': 'ping-nj.krunker.io:443' }, settle: 1000 });
// the match bar's Invite: copyInviteLink() relabels the button "Copied URL" for 1250 ms
scenario('invite-copied', async (c) => {
  await waitMenu(c);
  await clearBootError(c);
  await c.eval(flatPage('#222222'));
  await settle(c, 600);
  await c.eval(`(() => { inviteButton.click(); return 1; })()`);
  await sleep(300);
  console.error('[capture-v10-servers] invite: ' + JSON.stringify(await c.eval(`inviteButton.textContent`)));
}, {});

// v10's Host Game window on its Modes tab (windows[7].gen, v10:212909-212918): the hostOpt tiles of every
// hostable mode, Free for All checked (no preset), with the Toggle: the markup the Settings > Quickplay
// Seek Modes / Seek Maps pickers reuse (client/src/ui/seek_prefs.c; uidiff kn-settings-quickplay).
scenario('host-modes', async (c) => {
  await waitMenu(c);
  await clearBootError(c);
  await c.eval(flatPage('#000000'));
  await c.eval('(() => { showWindow(8); return 1; })()');
  await sleep(800);
  const r = await c.eval(`(() => { try { windows[7].switchTab(1); return document.querySelectorAll('.hostTb1 .hostOpt').length; } catch (e) { return String(e && e.message || e); } })()`);
  console.error('[capture-v10-servers] host modes tab: ' + r + ' tiles');
  await settle(c, 600);
}, { dir: 'host' });

// ---------------------------------------------------------------- main

function loadFixture(file) {
  const state = JSON.parse(fs.readFileSync(file, 'utf8')).state;
  LOBBIES = state.krunkerLobbies;
  REMOTE = state.remoteImages || {};
  if (!LOBBIES || !Array.isArray(LOBBIES.games)) throw new Error(file + ': no state.krunkerLobbies.games');
}

async function capture(sc, idx) {
  const base = +process.env.KN_CAPTURE_PORT_BASE || 9580;
  loadFixture(sc.fixture || FIXTURE);
  SEEK = sc.seek || null;
  SEEK_SEEN = 0;
  PING_LIST = sc.pingList || null;
  const { c, close, errors } = await openPage({ w: sc.size[0], h: sc.size[1], port: base + (idx % 8), storage: Object.assign({}, STORAGE, sc.storage || {}),
    settle: sc.settle || 10000, query: sc.query });
  try {
    await sc.run(c);
    const dir = path.join(OUT, sc.dir || 'servers');
    fs.mkdirSync(dir, { recursive: true });
    await c.shot(path.join(dir, sc.name + '.png'));
    const html = await c.eval(`document.documentElement.outerHTML`).catch(() => null);
    if (html) fs.writeFileSync(path.join(dir, sc.name + '.html.gz'), require('zlib').gzipSync(html));
    console.error(`[capture-v10-servers] ${sc.dir || 'servers'}/${sc.name}.png (${errors.length} page errors)`);
    if (process.env.KN_CAPTURE_ERRORS) for (const e of errors.slice(0, 20)) console.error('  ' + String(e).split('\n')[0]);
  } finally { close(); }
}

(async () => {
  const args = process.argv.slice(2);
  const names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
    else if (args[i] === '--fixture') FIXTURE = path.resolve(args[++i]);
    else if (args[i] === '--list') { console.log(Object.keys(S).join('\n')); return; }
    else names.push(args[i]);
  }
  const list = names.length ? names.map((n) => { if (!S[n]) throw new Error('unknown scenario ' + n); return S[n]; }) : Object.values(S);
  for (let i = 0; i < list.length; i++) await capture(list[i], i);
})().catch((e) => { console.error(e); process.exit(1); });
