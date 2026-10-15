#!/usr/bin/env node
// Krunker Strike 1.37.2 (FRVR) mobile UI reference captures for tools/uidiff
// (doc/ref/krunker-strike-1.37.2/), taken from FRVR's own client in an OFFLINE sandbox.
//
//   node tools/uidiff/capture-strike.js [--out <dir>] [--list] [--probe <js>] [name...]
//
// RUNS FRVR'S STRIKE CLIENT (the page's own JS), with the user's approval (2026-10-14, relayed by the
// coordinator: "you are allowed to run the frvr strike, iit's ok. this is approved."), sandboxed:
//   - headless Chromium only (--headless=new, DISPLAY / WAYLAND_DISPLAY removed), a fresh profile per
//     capture under ~/.cache/kn-wt/strike-ui-port-cap/ (deleted on exit), Chrome's process group
//     killed on exit / failure, nice -n 15;
//   - the page is served by a local static server this script starts on a free 127.0.0.1 port and
//     stops on exit (KN_STRIKE_PUBLIC, default /home/user/src/KrunkerStrike_v1.37.2/public);
//   - the proxy is a closed port (127.0.0.1:9), and Fetch-domain interception sees every request:
//     the local server's own are let through, the stubs below are answered locally, everything else
//     (FRVR's gateway / director / CDN / analytics / ads / Discord / Google) is failed with
//     BlockedByClient. Nothing reaches the network;
//   - window.WebSocket throws (before any page script), navigator.sendBeacon is a no-op, RTCPeerConnection
//     and WebTransport are null (index.html already does that), Math.random is seeded, Date is pinned.
// Mobile emulation: 640x360 CSS px landscape at deviceScaleFactor 2 (PNG 1280x720), touch on
// (maxTouchPoints 5), an Android Chrome UA, so the bundle's M1e() (window._isMobile), Ho() and
// ae().useMobileControls are true: the mobile version of the UI, as the user asked.
//
// What is served differently from the snapshot (the only changes):
//   - index.html: its <script src="./game.js"> (a renamed re-print of the bundle) is replaced by FRVR's
//     original bundle assets/index-Cz2uYXJQ.js, into which ONE accessor is inserted before `const swe={`:
//     window.__ks = () => ({...module-scope names...}) (SCOPE below), so the capture can push screens
//     (Pt) and set UI state (Mt, Se) like the game's own code does.
//   - libs/frvr-sdk.es6.min.js: the inert FRVR SDK stub FRVR_STUB below (a logged-out guest on the
//     "web" channel). libs/frvr-channel-web.es6.min.js: empty.
// Stubbed SDK calls (FRVR_STUB): init, bootstrapper.{init,setProgress,complete} resolve;
//   auth: isLoggedIn false, isLoggingIn false, isFRVRLoginEnabled false, getFreshAccessToken null,
//     platforms [], getCurrentPlatform "web"; channel.getId "web"; channelCharacteristics allowNavigation
//     false, allowExternalLinks true, allowThirdPartyDomains false; features.getFeature(name) = index.html's
//     defaultFeatures[name], getFeatures = all of them, getGroups []; localStorage / cloudStorage over
//     window.localStorage; iap: isReady false, getProductById undefined, purchase rejects; ads:
//     isSupported/isReady false, show resolves AdShowResult.Failed; tracker / logger / lifecycle /
//     notifications / shortcut / community / navigation / tournaments / social / challenges: no-ops
//     (every unknown member is a function resolving undefined; accesses are logged to window.__frvrLog).
// Stubbed HTTP routes (GATEWAY_STUBS below; gateway.us-svc.krunker.frvr.com and the directors):
//   listed with their JSON in GATEWAY_STUBS; any other gateway/director route answers 404.
// Textures FRVR's CDN serves (cdn.frvr.com/games/krunkerfrvr/..., the page's ./textures, ./img paths
//   the snapshot lacks): ~/.cache/kn-strike/live/cdn, then KrunkNative's assets/ (stand-ins), else 404
//   (listed in the per-capture log and README.md).
//
// Output per state: <out>/<name>.png, <name>.html.gz (document.documentElement.outerHTML),
// <name>.holder.html (pretty-printed #frvrMenuMobileHolder + #frvrLoadingScreen), <name>.rects.json;
// animated states also frames/<name>/NNNN.png + frames/<name>/index.json (virtual-time stamps).
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const zlib = require('zlib');
const http = require('http');
const net = require('net');
const crypto = require('crypto');
const { spawn } = require('child_process');
const { CDP, sleep } = require('../kref/cdp.js');

const REPO = path.resolve(__dirname, '..', '..');
const HOME = os.homedir();
const PUBLIC = path.resolve(process.env.KN_STRIKE_PUBLIC || '/home/user/src/KrunkerStrike_v1.37.2/public');
const SCRATCH = path.resolve(process.env.KN_STRIKE_SCRATCH || path.join(HOME, '.cache/kn-wt/strike-ui-port-cap'));
const CDN_DIRS = [path.join(HOME, '.cache/kn-strike/live/cdn'), path.join(REPO, 'assets')];
const BUNDLE = 'assets/index-Cz2uYXJQ.js';
const PROXY = 'http://127.0.0.1:9'; // closed: nothing unanswered leaves the machine
const W = 640, H = 360, DPR = 2;
const UA = 'Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Mobile Safari/537.36';
const FTUE_IDS = ['bots_ftue', 'bots_second_ftue', 'controls_ftue', 'customize_ftue', 'hof_ftue', 'name_change_ftue', 'round_end_ftue',
  'round_end_lean_ftue', 'round_end_second_ftue', 'round_end_second_lean_ftue', 'ticket_spin_ftue'];
const FIXED_NOW = Date.UTC(2026, 5, 13, 14, 30, 0); // the snapshot's date
let OUT = path.join(REPO, 'doc', 'ref', 'krunker-strike-1.37.2');
const log = (s) => { if (process.env.KN_CAPTURE_VERBOSE) console.error('[capture-strike] ' + s); };

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp', '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg',
  '.wav': 'audio/wav', '.mp4': 'video/mp4', '.webm': 'video/webm', '.txt': 'text/plain', '.obj': 'text/plain', '.glb': 'model/gltf-binary' };

// ---------------------------------------------------------------- served files

// The module-scope names the accessor exposes (identical in the minified bundle and its .deob.js).
const SCOPE = ['O5', 'B5', 'Ly', 'T5', 'fg', 'SA', 'Dy', 'm5', 'KU', 'YC', '_C', 'bC', 'UD', 'OD', 'XB', 'wb', 'KB', 'WB', 'GB', 'YB',
  'qB', 'jB', 'VB', 'zB', 'NB', 'UB', 'EA', 'HB', 'p0', 'UI', 'NU', 'XA', 'In', 'ws', 'vI', 'VC', 'N5', 'nB', 'Hy', 'P5', 'sM', 'jC', 'zU', 'QU',
  'mO', 'gO', 'qm', 'NC', 'xC', 'hO', 'g5', 'v5', 'xH', 'kt', 'Fm', 'RA', 'yz', 'Lk', 'al', 'pR', 'di', 'rn', 'Rt', 'is', 'ga', 'eH', 'k1',
  'Pt', 'swe', 'Yh', 'Kde', 'U5', 'tc', 'n5', 'Yp', 'uhe', 'Xx', 'Zr', 'yn', 'cr', 'Fm', 'RA', 'Se', 'Mt', 'g', 'Al', 'Yw', 'H5', 'a5', 'Pi', 'Tn', 'ae', 'zt', 'Oe', 'Ut', 'en', 'Zs', 'Vi', 'Qn', 'Nt',
  'xe', 'N', 'Ki', 'nu', 'Cg', 'Jn', 'q', 'Me', 'yi', 'yn', 'Dh', 'bn', 'Ad', 'IA', 'Ny', 'pd', 'yg'];
const SCOPE_ANCHOR = 'const swe={';
let bundleCache = null;
function bundle() {
  if (bundleCache) return bundleCache;
  const src = fs.readFileSync(path.join(PUBLIC, BUNDLE), 'utf8');
  const i = src.indexOf(SCOPE_ANCHOR);
  if (i < 0 || src.indexOf(SCOPE_ANCHOR, i + 1) >= 0) throw new Error('capture-strike: scope anchor not unique in ' + BUNDLE);
  const getters = SCOPE.map((n) => `get ${n}(){try{return ${n}}catch(e){return undefined}}`).join(',');
  const hook = `window.__ks=()=>({${getters},set(n,v){return eval(n+'=v')}});`;
  return (bundleCache = Buffer.from(src.slice(0, i) + hook + src.slice(i)));
}
function indexHtml() {
  let s = fs.readFileSync(path.join(PUBLIC, 'index.html'), 'utf8');
  const tag = '<script type="module" crossorigin src="./game.js"></script>';
  if (!s.includes(tag)) throw new Error('capture-strike: game.js script tag not found');
  s = s.replace(tag, `<script type="module" crossorigin src="./${BUNDLE}"></script>`);
  return Buffer.from(s);
}
function defaultFeatures() {
  // index.html's window.FRVR.config.features.defaultFeatures, read by the page itself at runtime
  return null;
}

const FRVR_STUB = `(() => {
  const L = (window.__frvrLog = []);
  const res = (v) => () => Promise.resolve(v);
  const deep = (name, fixed) => new Proxy(fixed, { get(t, k) {
    if (k in t || typeof k === 'symbol') return t[k];
    if (k === 'then') return undefined;
    L.push(name + '.' + String(k));
    return (t[k] = deep(name + '.' + String(k), Object.assign(function () { return Promise.resolve(undefined); }, {})));
  } });
  const feats = () => ((window.FRVR.config || {}).features || {}).defaultFeatures || {};
  // FRVR.localStorage / cloudStorage: JSON values over window.localStorage, getItem(key, default)
  const store = (prefix) => ({
    getItem: (k, d) => { const v = localStorage.getItem(prefix + k); if (v === null) return Promise.resolve(d === undefined ? null : d);
      try { return Promise.resolve(JSON.parse(v)); } catch (e) { return Promise.resolve(v); } },
    setItem: (k, v) => Promise.resolve(localStorage.setItem(prefix + k, JSON.stringify(v))),
    removeItem: (k) => Promise.resolve(localStorage.removeItem(prefix + k)) });
  const config = (window.FRVR && window.FRVR.config) || {};
  const stub = {
    config, init: () => undefined,
    bootstrapper: deep('bootstrapper', { init: res(), setProgress: res(), complete: res() }),
    auth: deep('auth', { isLoggedIn: () => !!window.__frvrIn, isLoggingIn: () => false, isFRVRLoginEnabled: () => false, getFreshAccessToken: res('offline-token'),
      platforms: [], getCurrentPlatform: () => 'web', getFRVRID: () => null, getCredentials: res(null), isPlatformAvailable: () => false,
      isLogoutSupported: () => false, addStatusChangeListener: () => undefined, login: () => { window.__frvrIn = 1; return Promise.resolve(); } }),
    channel: deep('channel', { getId: () => 'web' }),
    channelCharacteristics: { allowNavigation: false, allowExternalLinks: true, allowThirdPartyDomains: false },
    features: deep('features', { getFeature: (n) => Promise.resolve(feats()[n]), getFeatures: () => Promise.resolve(feats()), getGroups: () => [] }),
    localStorage: store(''), cloudStorage: store('__cloud_'),
    iap: deep('iap', { isReady: () => false, getProductById: () => undefined, purchase: () => Promise.reject(new Error('offline')),
      restorePurchases: res([]), consumePurchase: res(), configure: () => undefined, errors: {} }),
    ads: deep('ads', { AdType: { INTERSTITIAL: 'interstitial', REWARD: 'reward', BANNER: 'banner' }, AdShowResult: { SHOWN: 'shown', SKIPPED: 'skipped', FAILED: 'failed' },
      isSupported: () => false, isReady: () => false, show: res('failed'), hide: () => undefined, init: () => undefined, configure: () => undefined }),
    tracker: deep('tracker', { ftueUnordered: res(), logEvent: () => undefined, logValuedEvent: () => undefined, addExtraFieldFunction: () => undefined }),
    logger: deep('logger', { debug: () => undefined, error: () => undefined, info: () => undefined, warn: () => undefined }),
    abTests: deep('abTests', { cohort: () => undefined, abTestCohorts: {} }),
    profile: { id: () => 'kn-guest-0001', name: () => '', nickname: () => '', image: () => null },
    social: deep('social', { platform: deep('social.platform', { getContextId: res(null), getContextData: res(null), getFriends: res([]), canInvite: res(false),
      isSupportedAPI: (a) => !!(window.__frvrApis && window.__frvrApis[a]), API: new Proxy({}, { get: (t, k) => String(k) }) }), live: deep('social.live', {}), syncFriends: res([]) }),
    tournaments: deep('tournaments', { platform: deep('tournaments.platform', {}) }),
    navigation: deep('navigation', { canNavigate: () => false, canOpenChannelAppStore: () => false }),
    shortcut: deep('shortcut', { canCreateShortcutAsync: res(false) }),
    community: deep('community', { canFollowOfficialPage: res(false) }),
    notifications: deep('notifications', { canScheduleMessages: () => false }),
    entrypoint: deep('entrypoint', { getData: () => ({}) }),
    lifecycle: deep('lifecycle', {}), challenges: deep('challenges', {}),
  };
  window.FRVR = deep('FRVR', stub);
})();`;

// Gateway / director routes the menu reads at boot, answered locally (a logged-out guest).
// Shapes from the bundle's callers (index-Cz2uYXJQ.deob.js) and ~/.cache/kn-strike/live/strike.json 'gateway'.
// the guest account (players/me): a returning level-5 guest with every Strike class unlocked
const ACCOUNT = { _id: 'kn-guest-0001', frvrId: 'kn-guest-0001', name: 'Guest_4821', isDev: false, moneySpent: 0, goldenSpinCount: 0,
  wallet: { xp: 2600, kr: 1250, spin_ticket: 1, spin_golden_ticket: 0, name_change: 1, daily_kr_gained: 0, seasonal_event_tokens: 0 },
  classes: [0, 1, 2, 4, 5, 7].map((id) => ({ id })), items: [], features: {}, channelData: {}, progressionClaimedItems: [], stats: {} };
// accountProgression (the level track): level -> slot; KR every level, a spin ticket every 5th
const PROGRESSION = Object.fromEntries(Array.from({ length: 50 }, (_, i) => [String(i + 1), { slotId: (i + 1) + '_00', isAutoClaimable: false,
  isPlayerChoice: false, items: [(i + 1) % 5 ? { type: 'currency', currency: 'kr', amount: 25 + 5 * i } : { type: 'currency', currency: 'spin_ticket', amount: 1 }] }]));
// the shop's spin wheels (a guest sees the paid ones: spin tickets / KR; ad wheels need ads)
const ODDS = [['Common', '#a7b1c2', 0.5], ['Uncommon', '#79d94a', 0.28], ['Rare', '#3aa0ff', 0.14], ['Epic', '#b45cff', 0.06], ['Legendary', '#ffb800', 0.02]];
const wheel = (id, name, price) => ({ _id: id, id, name, price, category: 'default', hideInShop: false, imageUrl: '',
  rewards: ODDS.map(([poolName, color, weight]) => ({ poolName, color, weight })), maxAccumulatedSpins: 0, lastSpinTime: undefined,
  featuredItems: [] });
const SPIN_WHEELS = [wheel('wheel-ticket', 'Ticket Spin', { currency: 'spin_ticket', amount: 1 }), wheel('wheel-kr', 'KR Spin', { currency: 'kr', amount: 250 }),
  wheel('lucky-draw', 'Lucky Draw', { currency: 'kr', amount: 500 })];
const GATEWAY_STUBS = {
  'GET /live': 'ok', // director latency probes
  'GET /v1/game-config': { accountProgression: PROGRESSION, accountProgressionPremium: {},
    versionControl: { minimum: '1.0.0', recommended: '1.0.0', nonLegacy: '1.37.2' }, events: [], remoteFeatures: {},
    krRewardLimits: { maxDailyKR: 200 }, spinWheels: { spinsToGetGoldenSpin: 10, luckyDraw: 'lucky-draw' }, hostedTournament: {} },
  'GET /v1/players/me': ACCOUNT,
  'GET /v1/shop': { products: [] },
  'GET /v2/shop': { products: {}, front: { sections: [] } },
  'GET /v1/missions/me': { missions: [] },
  // item definitions: only the class faces (classConfig.face ids) are asked at boot; a bare default def each
  'POST /v1/item-definitions/itemList': (u, req) => { let ids = []; try { ids = JSON.parse(req.postData || '{}').itemIDs || []; } catch (e) {}
    return ids.map((id) => ({ _id: id, id, type: 7, name: 'Default', rarity: 0, data: {} })); },
  'GET /v2/spin-wheels/firstSpinDate': { firstSpinDate: null },
  'GET /v2/spin-wheels/list': SPIN_WHEELS,
};
// /v1/spin-wheels/<id> (SPIN: the wheel, its odds and featured items)
const GATEWAY_PREFIX_STUBS = [['/v1/spin-wheels/', (u) => SPIN_WHEELS.find((w) => u.pathname.endsWith('/' + w._id)) || SPIN_WHEELS[0]]];

// ---------------------------------------------------------------- local static server

function freePort() {
  return new Promise((res, rej) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); s.on('error', rej); });
}
const isFile = (f) => { try { return fs.statSync(f).isFile(); } catch (e) { return false; } };
// the file / [content, ext] a request path is answered with ('' = 404)
function localFile(p) {
  p = p.replace(/^\/+/, '');
  if (p === '' || p.endsWith('/')) p += 'index.html';
  if (p.split('/').includes('..')) return '';
  if (p === 'index.html') return [indexHtml(), '.html'];
  if (p === BUNDLE) return [bundle(), '.js'];
  if (p === 'libs/frvr-sdk.es6.min.js') return [FRVR_STUB, '.js'];
  if (p === 'libs/frvr-channel-web.es6.min.js') return ['', '.js'];
  if (p.startsWith('libs/')) return ['', '.js']; // discord SDK etc.: never needed offline
  const f = path.join(PUBLIC, p);
  if (isFile(f)) return f;
  return cdnFile(p);
}
function cdnFile(p) {
  p = p.replace(/^\/+/, '').replace(/^games\/krunkerfrvr\//, '').replace(/^cdnfrvr\/games\/krunkerfrvr\//, '');
  for (const d of CDN_DIRS) { const a = path.join(d, p); if (isFile(a)) return a; }
  return '';
}
const MISSING = new Set();
function startServer(port) {
  const srv = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://127.0.0.1');
    const f = localFile(decodeURIComponent(u.pathname));
    if (!f) { MISSING.add(u.pathname); res.writeHead(404, { 'Content-Type': 'text/plain' }); res.end(); return; }
    const stub = Array.isArray(f);
    const body = stub ? Buffer.from(f[0]) : fs.readFileSync(f);
    const ext = stub ? f[1] : path.extname(f).toLowerCase();
    res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream', 'Cache-Control': 'no-store', 'Access-Control-Allow-Origin': '*' });
    res.end(req.method === 'HEAD' ? undefined : body);
  });
  return new Promise((r) => srv.listen(port, '127.0.0.1', () => r(srv)));
}

// ---------------------------------------------------------------- chrome

function launchChrome(port, profile) {
  const env = { ...process.env }; delete env.DISPLAY; delete env.WAYLAND_DISPLAY;
  return spawn('nice', ['-n', '15', process.env.KN_CHROME || 'chromium-browser', '--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader',
    '--enable-unsafe-swiftshader', '--hide-scrollbars', '--mute-audio', '--no-first-run', '--no-default-browser-check', '--disable-sync',
    '--disable-extensions', '--disable-background-networking', '--disable-component-update', '--disable-domain-reliability',
    '--metrics-recording-only', '--password-store=basic', '--font-render-hinting=none', `--proxy-server=${PROXY}`, '--proxy-bypass-list=127.0.0.1' /* only the local static server bypasses the closed proxy */,
    '--autoplay-policy=no-user-gesture-required', `--window-size=${W * DPR},${H * DPR}`, `--remote-debugging-port=${port}`, '--remote-debugging-address=127.0.0.1',
    `--user-data-dir=${profile}`, 'about:blank'], { stdio: 'ignore', detached: true, env });
}

function preScript() {
  return `(() => {
    const Blocked = function () { throw new DOMException('blocked by capture-strike.js', 'SecurityError'); };
    Blocked.CONNECTING = 0; Blocked.OPEN = 1; Blocked.CLOSING = 2; Blocked.CLOSED = 3;
    Object.defineProperty(window, 'WebSocket', { value: Blocked, writable: false, configurable: false });
    navigator.sendBeacon = () => false;
    let a = 0x4b4e; Math.random = function () { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a);
      t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };
    const RD = Date, t0 = RD.now(), base = ${FIXED_NOW};
    const now = () => base + (RD.now() - t0);
    class D extends RD { constructor(...a) { if (a.length) super(...a); else super(now()); } static now() { return now(); } }
    window.Date = D;
    try { localStorage.setItem('consent', '1');
      // a returning player: every first-time-user flow done (FRVR ftue system storage, local and cloud)
      const done = JSON.stringify(Object.fromEntries(${JSON.stringify(FTUE_IDS)}.map((k) => [k, true])));
      if (!window.__knKeepFtue) { localStorage.setItem('__frvr_krunker_ftue_system', done); localStorage.setItem('__cloud___frvr_krunker_ftue_system', done);
        // Ut.hasAlreadyPlayedBefore() (key Q2): a returning player boots to MAIN_MENU (lwe), not straight into a game
        localStorage.setItem('ftue_already_played_before', 'true'); localStorage.setItem('__cloud_ftue_already_played_before', 'true'); }
    } catch (e) {}
  })()`;
}

// ---------------------------------------------------------------- page helpers

const HOLDER = `(() => {
  const pretty = (el, d) => { const pad = '  '.repeat(d);
    if (el.nodeType === 3) { const t = el.textContent.replace(/\\s+/g, ' ').trim(); return t ? pad + t + '\\n' : ''; }
    if (el.nodeType !== 1) return '';
    const attrs = [...el.attributes].map((a) => ' ' + a.name + '="' + a.value.replace(/"/g, '&quot;') + '"').join('');
    const tag = el.tagName.toLowerCase();
    if (['img', 'input', 'br', 'hr'].includes(tag)) return pad + '<' + tag + attrs + '>\\n';
    const kids = [...el.childNodes].map((k) => pretty(k, d + 1)).join('');
    return pad + '<' + tag + attrs + '>\\n' + kids + pad + '</' + tag + '>\\n'; };
  return ['frvrLoadingScreen', 'frvrMenuMobileHolder'].map((id) => { const e = document.getElementById(id); return e ? pretty(e, 0) : '<!-- no #' + id + ' -->\\n'; }).join('\\n'); })()`;
const RECTS = `(() => { const o = {}; const sels = ['#frvrLoadingScreen', '#frvrMenuMobileHolder', '#frvrMenuMobile', '#frvrTapToPlayMobile',
  '#frvrEndScreenMobile', '.loading-screen', '.frvr-main-menu', '.frvr-in-game-ui', '.class-selection__blue-blur', '.frvr-store-container',
  '.frvr-settings-screen', '.frvr-lboard-end-screen', '.frvr-end-screen', '.frvr-tap-to-play--top-center', '.mobile-controls', '#InGameUIMobile'];
  for (const s of sels) { const e = document.querySelector(s); if (!e) continue; const cs = getComputedStyle(e);
    if (cs.display === 'none' || cs.visibility === 'hidden') continue; const r = e.getBoundingClientRect(); if (r.width < 1 || r.height < 1) continue;
    o[s] = [Math.round(r.x), Math.round(r.y), Math.round(r.width), Math.round(r.height)]; } return o; })()`;
const PAUSE_ANIMS = (ms) => `(() => { let n = 0; for (const a of document.getAnimations()) { try { a.pause(); a.currentTime = ${ms}; n++; } catch (e) {} } return n; })()`;
const FINISH_ANIMS = `(() => { let n = 0; for (const a of document.getAnimations()) { try { const t = a.effect.getComputedTiming();
  if (t.endTime !== Infinity) { a.finish(); n++; } else { a.pause(); a.currentTime = 800; } } catch (e) {} } return n; })()`;
// the 3D world (three.js renderer.domElement#game-canvas) hidden over a flat page; the menu's own canvases (the avatar viewer) stay
const GAME_CANVAS_FLAT = (bg) => `(() => { const cv = document.getElementById('game-canvas'); if (cv) cv.style.opacity = '0';
  document.documentElement.style.background = '${bg}'; document.body.style.background = '${bg}'; return 1; })()`;

const CLOSERS = new Set();
process.on('exit', () => { for (const c of [...CLOSERS]) c(); });
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => process.exit(130));

async function openPage(o) {
  fs.mkdirSync(SCRATCH, { recursive: true });
  const profile = fs.mkdtempSync(path.join(SCRATCH, 'chrome-'));
  const httpPort = await freePort();
  const srv = await startServer(httpPort);
  const cdpPort = await freePort();
  const chrome = launchChrome(cdpPort, profile);
  const origin = `http://127.0.0.1:${httpPort}`;
  let c = null;
  const close = () => { CLOSERS.delete(close); try { c && c.close(); } catch (e) { } try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { }
    try { srv.close(); } catch (e) { } fs.rmSync(profile, { recursive: true, force: true }); };
  CLOSERS.add(close);
  const blocked = [], errors = [], stubbed = [], consoleLines = [], held = [];
  try {
    c = await CDP.connect(cdpPort);
    c.on((m) => {
      if (m.method === 'Fetch.requestPaused') {
        const p = m.params; let u; try { u = new URL(p.request.url); } catch (e) { u = null; }
        if (u && u.origin === origin) { c.send('Fetch.continueRequest', { requestId: p.requestId }).catch(() => {}); return; }
        if (u && p.request.method === 'OPTIONS' && /krunker\.frvr\.com$/.test(u.hostname)) {
          c.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: 204, responseHeaders: corsHeaders(), body: '' }).catch(() => {}); return;
        }
        if (u && /\/v1\/director\/allocate/.test(u.pathname)) { held.push(p.request.url); return; } // never answered: no game server
        const ans = u ? answerRemote(u, p.request) : null;
        if (ans) {
          stubbed.push(p.request.method + ' ' + p.request.url + (p.request.postData ? ' ' + p.request.postData.slice(0, 300) : ''));
          c.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: ans.code, responseHeaders: [{ name: 'Content-Type', value: ans.type }, ...corsHeaders()],
            body: Buffer.from(ans.body).toString('base64') }).catch(() => {});
          return;
        }
        blocked.push(p.request.method + ' ' + p.request.url.slice(0, 200));
        c.send('Fetch.failRequest', { requestId: p.requestId, errorReason: 'BlockedByClient' }).catch(() => {});
      } else if (m.method === 'Runtime.exceptionThrown') {
        errors.push(((m.params.exceptionDetails || {}).exception || {}).description || (m.params.exceptionDetails || {}).text);
      } else if (m.method === 'Runtime.consoleAPICalled' && ['error', 'warning'].includes(m.params.type)) {
        consoleLines.push(m.params.type + ': ' + m.params.args.map((a) => a.value !== undefined ? String(a.value) : (a.description || a.type)).join(' ').slice(0, 300));
      }
    });
    await c.send('Network.enable');
    await c.send('Network.setCacheDisabled', { cacheDisabled: true });
    await c.send('Fetch.enable', { patterns: [{ urlPattern: '*', requestStage: 'Request' }] });
    await c.send('Runtime.enable');
    await c.send('Page.enable');
    await c.send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: DPR, mobile: true,
      screenOrientation: { type: 'landscapePrimary', angle: 90 } });
    await c.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
    await c.send('Emulation.setUserAgentOverride', { userAgent: UA, platform: 'Linux armv8l' });
    await c.send('Page.addScriptToEvaluateOnNewDocument', { source: preScript() });
    if (o.beforeNavigate) await o.beforeNavigate(c);
    await c.send('Page.navigate', { url: origin + '/' });
  } catch (e) { close(); throw e; }
  return { c, origin, errors, blocked, stubbed, consoleLines, close };
}
function corsHeaders() {
  return [{ name: 'Access-Control-Allow-Origin', value: '*' }, { name: 'Access-Control-Allow-Headers', value: '*' },
    { name: 'Access-Control-Allow-Methods', value: 'GET,POST,PUT,DELETE,OPTIONS' }];
}
// a request to another origin: a local stub ({code, type, body}) or null (refused)
function answerRemote(u, req) {
  if (/^gateway\.(us-svc|staging)\.krunker\.frvr\.com$/.test(u.hostname) || /^director\.[a-z0-9-]+\.krunker\.frvr\.com$/.test(u.hostname)) {
    const key = req.method + ' ' + u.pathname;
    let s = GATEWAY_STUBS[key] !== undefined ? GATEWAY_STUBS[key] : GATEWAY_STUBS['* ' + u.pathname];
    if (s === undefined && req.method === 'GET') { const pre = GATEWAY_PREFIX_STUBS.find(([x]) => u.pathname.startsWith(x)); if (pre) s = pre[1]; }
    if (s === undefined) return { code: 404, type: 'application/json', body: '{"error":"not stubbed"}' };
    return { code: 200, type: 'application/json', body: typeof s === 'function' ? JSON.stringify(s(u, req)) : JSON.stringify(s) };
  }
  if (/^(cdn\.frvr\.com|assets\.frvr\.com)$/.test(u.hostname) && ['GET', 'HEAD'].includes(req.method)) {
    const f = cdnFile(decodeURIComponent(u.pathname));
    if (f) return { code: 200, type: MIME[path.extname(f).toLowerCase()] || 'application/octet-stream', body: fs.readFileSync(f) };
    MISSING.add(u.href);
    return { code: 404, type: 'text/plain', body: '' };
  }
  return null;
}

// ---------------------------------------------------------------- output

async function writeState(c, name, opts = {}) {
  fs.mkdirSync(OUT, { recursive: true });
  await c.shot(path.join(OUT, name + '.png'));
  fs.writeFileSync(path.join(OUT, name + '.html.gz'), zlib.gzipSync(Buffer.from(await c.eval('document.documentElement.outerHTML'), 'utf8'), { level: 9 }));
  fs.writeFileSync(path.join(OUT, name + '.holder.html'), await c.eval(HOLDER));
  fs.writeFileSync(path.join(OUT, name + '.rects.json'), JSON.stringify(await c.eval(RECTS)) + '\n');
  console.error(`[capture-strike] ${name}`);
}
// frames: the page's animations seeked to t = start..end step ms (Web Animations currentTime)
async function writeFrames(c, name, endMs, stepMs = 50, setup) {
  const dir = path.join(OUT, 'frames', name);
  fs.mkdirSync(dir, { recursive: true });
  const index = [];
  let i = 0;
  for (let t = 0; t <= endMs; t += stepMs, i++) {
    if (setup) await setup(t);
    await c.eval(PAUSE_ANIMS(t));
    await sleep(60);
    const f = String(i).padStart(4, '0') + '.png';
    await c.shot(path.join(dir, f));
    index.push({ frame: f, t_ms: t });
  }
  fs.writeFileSync(path.join(dir, 'index.json'), JSON.stringify({ name, step_ms: stepMs, frames: index }, null, 1) + '\n');
  console.error(`[capture-strike] frames/${name} (${index.length})`);
}

module.exports = { openPage, writeState, writeFrames, PAUSE_ANIMS, FINISH_ANIMS, GAME_CANVAS_FLAT, GATEWAY_STUBS, MISSING };

// ---------------------------------------------------------------- driving the page

const K = 'window.__ks()';
// the booted menu: logged-in guest, boot loading overlay gone, the allocate held (no game server)
async function boot(o = {}) {
  const pg = await openPage(o);
  await pg.c.waitFor(`!!(window.__ks && ${K}.Se.isLoggedIn && ${K}.Se.loadingScreenDiv && ${K}.Se.loadingScreenDiv.style.display === 'none')`, 60000, 250);
  await sleep(o.settle || 4000);
  return pg;
}
// clear the screen stack and the loading overlay (the held allocate's "Loading" curtain)
const CLEAN = `(() => { const k = ${K}; k.Pi.length = 0; k.set('al', undefined); k.Se.loadingComplete = false; k.g.redraw.sync(); return 1; })()`;
// push one screen: Pt(component, attrs, opts) like the game's own code
async function push(c, comp, attrs = '{}', opts = '{}', wait = 2500) {
  await c.eval(`(() => { const k = ${K}; k.Pt(k.${comp}, ${attrs}, ${opts}); k.g.redraw.sync(); return 1; })()`);
  await sleep(wait);
}
async function still(c, name, o = {}) {
  await c.eval(GAME_CANVAS_FLAT(o.bg || '#000'));
  if (o.at !== undefined) await c.eval(PAUSE_ANIMS(o.at)); else await c.eval(FINISH_ANIMS);
  await sleep(300);
  await writeState(c, name);
}
function report(pg, name) {
  const errs = pg.errors.concat(pg.consoleLines.filter((l) => l.startsWith('error')));
  if (errs.length) console.error(`  ${name}: ${errs.length} page errors; first: ${String(errs[0]).split('\n')[0].slice(0, 200)}`);
}

// ---------------------------------------------------------------- scenarios
const S = {};
// every state is one fresh boot (a view that throws leaves Mithril's tree broken for the next one)
async function fresh(name, fn, o = {}) {
  const pg = await boot(o);
  try { await fn(pg.c, pg); report(pg, name); } finally { pg.close(); }
}
// the in-game state: the local player (module var Me = Game.tmpPlayer) and the UI switched to the game (Yh(true)),
// like the bundle's spawn handler does; G.controls.keys is what the mobile controls read (a5 / bh)
const INGAME = (extra = '') => `(() => { const k = ${K}; k.Pi.length = 0; k.set('al', undefined); k.Se.loadingComplete = false;
  const A = k.ae(); const G = A.Game; const wep = (G.weapons && G.weapons[0]) || {};
  const p = { sid: 1, id: 'kn', name: 'Guest_4821', health: 140, maxHealth: 180, classIndex: 0, team: null, loadout: [0, -1], loadoutIndex: 0,
    weaponIndex: 0, weapon: wep, weapons: [wep], ammos: [24, 0], reserveAmmos: [0, 0], isYou: true, active: true, score: 350, kills: 3, deaths: 1,
    streak: 2, level: 12, x: 0, y: 0, z: 0, skins: [], reloadTimer: 0, inView: true, procInputs() {} };
  k.set('Me', p); A.player = p; if (G.controls && !G.controls.keys) G.controls.keys = {};
  ${extra}
  k.Yh(true); k.g.redraw.sync(); return 1; })()`;
const KILLFEED = `k.Mt.killLogStack = [
    { killer: 'Guest_4821', killerIsPlayer: true, victim: 'Botman', weapon: 'textures/weapons/icon_1.png', headshot: true },
    { killer: 'Krunky', victim: 'Guest_4821', victimIsPlayer: true, weapon: 'textures/weapons/icon_2.png' }];`;
const MENU = {
  'main-menu': null, 'class-selection': 'B5', 'customization': 'Ly', 'settings': 'Dy', 'tap-to-play': 'Yw', 'profile': 'YC',
  'create-match': 'm5', 'battlepass': '_C',
};
for (const [n, comp] of Object.entries(MENU)) {
  S['strike-' + n] = () => fresh(n, async (c) => {
    if (comp) { await c.eval(CLEAN); await push(c, comp); }
    await still(c, 'strike-' + n);
  });
}
// the settings tabs: FRVR's own tab selectors clicked (tabSelectorItem n)
['display', 'game', 'sound', 'other'].forEach((tab, i) => {
  S['strike-settings-' + tab] = () => fresh('settings-' + tab, async (c) => {
    await c.eval(CLEAN); await push(c, 'Dy');
    await c.eval(`(() => { const t = document.querySelectorAll('.tabSelectorItem')[${i + 1}]; t.dispatchEvent(new MouseEvent('click', { bubbles: true })); return 1; })()`);
    await sleep(1500);
    await still(c, 'strike-settings-' + tab);
  });
});
// FRVR's Mithril loading screen (vb / LOADING_SCREEN) over the menu: Fm(Al, {info}) at t = 0.6 s / complete
S['strike-loading'] = () => fresh('loading', async (c) => {
  await c.eval(`(() => { const k = ${K}; k.Pi.length = 0; k.Fm(k.Al, { info: 'Loading Game' }); k.g.redraw.sync(); return 1; })()`);
  await sleep(800);
  await still(c, 'strike-loading', { at: 1500 });
  await writeFrames(c, 'strike-loading', 1200, 100);
});
// index.html's static loading screen (the first load: "Loading Assets", the 10 s bar)
S['strike-boot'] = () => fresh('boot', async (c) => {
  await c.eval(`(() => { const k = ${K}; k.Pi.length = 0; k.g.redraw.sync(); k.Se.loadingScreenDiv.style.display = '';
    document.getElementById('frvrMenuMobileHolder').style.display = 'none'; return 1; })()`);
  await sleep(300);
  await still(c, 'strike-boot', { at: 2000 });
});
S['strike-ingame'] = () => fresh('ingame', async (c) => { await c.eval(INGAME()); await sleep(1500); await still(c, 'strike-ingame'); });
S['strike-ingame-killfeed'] = () => fresh('ingame-killfeed', async (c) => { await c.eval(INGAME(KILLFEED)); await sleep(1500); await still(c, 'strike-ingame-killfeed'); });
S['strike-killcard'] = () => fresh('killcard', async (c) => {
  await c.eval(INGAME(`k.Mt.killCardInfo = { killedBy: true, pfp: 'textures/classes/icon_1.png', nickname: 'Krunky', color: '#fff', weapon: '[Sniper Rifle]', lvl: 7, score: 100 };
    k.Mt.showKillCard = true; k.Mt.mobileControls.visible = false;`));
  await sleep(1500); await still(c, 'strike-killcard');
});
// the round end: LEAN_END_SCREEN (WB / Ad), victory, rank 1 of an FFA
S['strike-end'] = () => fresh('end', async (c) => {
  await c.eval(`(() => { const k = ${K}; k.Pi.length = 0; k.set('al', undefined);
    const ped = { rewards: [{ currencyId: 'xp', quantity: 350, origin: 'game' }, { currencyId: 'kr', quantity: 25, origin: 'game' }] };
    k.Mt.victoryState = '--victory';
    k.nu(k.Ad, { endTimer: 15, account: k.Se.account, playerEndData: ped, isRewardedAdSupported: false,
      matchDetails: { rank: 1, isFFA: true, victoryState: '--victory' },
      legacyData: { player: { name: 'Guest_4821', score: 350, kills: 3, deaths: 1 }, rank: '#1', tms: 0 } }, { keepAlive: true });
    k.g.redraw.sync(); return 1; })()`);
  await sleep(4000); await still(c, 'strike-end');
});

// ---- the menu screens of FRVR's own push sites (the attrs each one is pushed with in the bundle):
// STORE (fg), SPIN (SA: nu/Pt(SA, {spinnerIndex})), MISSION_TAB_SCREEN (UD: the account's activeMissions),
// REWARD_TO_CLAIM (XB: the tournament reward row), CUSTOMIZE_CONTROLS (wb: Pt(ZC, {onHide})), MATCH_DETAILS_SCREEN
// (GB / Ny: Pt(Ny, {legacyData, hasBanner})), LEADERBOARD_PLAYERS (YB / QM: the tournament board rows), POPUP (In:
// Pt(In, {builder})) with an empty builder: the frame the popups' own content goes into.
const MISSIONS = [
  { T: 'daily', N: 'Get 10 kills', IC: 1, completed: false, PV: 4, PT: 3, TR: 3600, details: { requiredValue: 10, boss: false },
    reward: { stat: { kr: 25, bpp: 0, junk: 0 }, skins: [] } },
  { T: 'daily', N: 'Win 1 match', IC: 2, completed: true, PV: 1, PT: 2, TR: 3600, details: { requiredValue: 1, boss: false },
    reward: { stat: { kr: 50, bpp: 1, junk: 0 }, skins: [] } },
];
const LEADERS = [
  { stats: { trophies: 350, reward: '+25' }, rank: 1, pfp: null, alias: 'Guest_4821', sid: 1, classIndex: 0 },
  { stats: { trophies: 120, reward: '+10' }, rank: 2, pfp: null, alias: 'Krunky', sid: 2, classIndex: 1 },
  { stats: { trophies: 40, reward: 'NONE' }, rank: 3, pfp: null, alias: 'Botman', sid: 3, classIndex: 4 },
];
S['strike-store'] = () => fresh('store', async (c) => { await c.eval(CLEAN); await push(c, 'fg', '{}', '{}', 3000); await still(c, 'strike-store'); });
S['strike-spin'] = () => fresh('spin', async (c) => { await c.eval(CLEAN); await push(c, 'SA', "{spinnerIndex:'wheel-ticket'}", '{}', 3000); await still(c, 'strike-spin'); });
// a component inside POPUP's builder (g(comp, attrs)): the game's own call sites host these in popups
const pushPopup = async (c, comp, attrs = '{}', wait = 2500) => {
  await c.eval(`(() => { const k = ${K}; k.Pt(k.In, {builder: () => [k.g(k.${comp}, ${attrs})]}, {}); k.g.redraw.sync(); return 1; })()`);
  await sleep(wait);
};
S['strike-missions'] = () => fresh('missions', async (c) => {
  // UD reads ae().account.activeMissions. ae().account is a getter that is undefined here, so the capture
  // gives the app object the guest account (Se.account) with the missions on it (capture only)
  await c.eval(`(() => { const k = ${K}; const A = k.ae(); const acc = k.Se.account;
    acc.activeMissions = ${JSON.stringify(MISSIONS)};
    Object.defineProperty(A, 'account', { value: acc, writable: true, configurable: true, enumerable: true });
    return A.account.activeMissions.length; })()`);
  await c.eval(CLEAN); await pushPopup(c, 'UD', '{}', 2500); await still(c, 'strike-missions');
});
S['strike-reward-to-claim'] = () => fresh('reward-to-claim', async (c) => {
  await c.eval(CLEAN); await pushPopup(c, 'XB', "{reward:{skins:[],stat:{kr:250,bpp:0,junk:0}}}", 2500); await still(c, 'strike-reward-to-claim');
});
S['strike-customize-controls'] = () => fresh('customize-controls', async (c) => {
  await c.eval(CLEAN); await push(c, 'wb', '{onHide:()=>{}}', '{}', 2500); await still(c, 'strike-customize-controls');
});
S['strike-match-details'] = () => fresh('match-details', async (c) => {
  await c.eval(CLEAN);
  // the round's legacy end data (computeMatchDetailsData reads endData.tms / players and endStats)
  const legacy = { player: { name: 'Guest_4821', score: 350, kills: 3, deaths: 1, team: 1, sid: 1, classIndex: 0, victory: true, funds: 25, rank: '#1' },
    rank: '#1', tms: 0, endStats: ['', '', 'Score', 'Kills', 'Deaths'],
    endData: { tms: [[1, 3], [2, 0]], players: { 1: { rewards: [{ currencyId: 'xp', quantity: 350 }, { currencyId: 'kr', quantity: 25 }] } } } };
  // the round's own player (ae().player, as INGAME sets it): the card reads its skins
  await c.eval(`(() => { const k = ${K}; const A = k.ae(); if (!A.player) A.player = { sid: 1, team: 1, classIndex: 0, skinIndexes: [0, 0, 0, 0, 0, 0, 0, 0], name: 'Guest_4821' }; return 1; })()`);
  await push(c, 'GB', `{legacyData:${JSON.stringify(legacy)},hasBanner:false}`, '{}', 2500);
  await still(c, 'strike-match-details');
});
S['strike-leaderboard-players'] = () => fresh('leaderboard-players', async (c) => {
  await c.eval(CLEAN);
  await push(c, 'YB', `{enemyPlayers:${JSON.stringify(LEADERS)},playerData:{sid:1},stats:['Score','Kills','reward']}`, '{}', 2500);
  await still(c, 'strike-leaderboard-players');
});
S['strike-popup'] = () => fresh('popup', async (c) => { await c.eval(CLEAN); await push(c, 'In', '{builder:()=>[]}', '{}', 2000); await still(c, 'strike-popup'); });

(async () => {
  const args = process.argv.slice(2);
  const names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
    else if (args[i] === '--list') { console.log(Object.keys(S).join('\n')); return; }
    else if (args[i] === '--probe') { // debugging: load the page, wait, print <js>
      const js = args[++i], wait = +(args[i + 1] && /^\d+$/.test(args[i + 1]) ? args[++i] : 15000);
      const pg = await openPage({});
      try {
        await sleep(wait);
        let v; try { v = await pg.c.eval(js); } catch (e) { v = 'EVAL ERROR ' + e.message; }
        console.log(typeof v === 'string' ? v : JSON.stringify(v, null, 1));
        if (process.env.KN_PROBE_SHOT) await pg.c.shot(process.env.KN_PROBE_SHOT);
        console.error('errors:\n  ' + pg.errors.slice(0, 25).map((e) => String(e).split('\n').slice(0, 2).join(' | ')).join('\n  '));
        console.error('console:\n  ' + pg.consoleLines.slice(0, 30).join('\n  '));
        console.error('stubbed:\n  ' + pg.stubbed.slice(0, 60).join('\n  '));
        console.error('blocked:\n  ' + pg.blocked.slice(0, 60).join('\n  '));
        console.error('404:\n  ' + [...MISSING].slice(0, 60).join('\n  '));
      } finally { pg.close(); }
      return;
    } else if (args[i] === '--shots') { // debugging: one boot, each remaining arg COMP or COMP=attrs-js pushed on a clean stack
      const list = args.slice(i + 1); i = args.length;
      const pg = await boot();
      try { for (const a of list) { const [comp, attrs] = a.includes('=') ? [a.slice(0, a.indexOf('=')), a.slice(a.indexOf('=') + 1)] : [a, '{}'];
        const n0 = pg.errors.length + pg.consoleLines.length;
        try { await pg.c.eval(CLEAN); await push(pg.c, comp, attrs); await still(pg.c, '_shot-' + comp); } catch (e) { console.error('  FAIL ' + comp + ' ' + e.message.slice(0, 200)); }
        for (const e of pg.errors.concat(pg.consoleLines).slice(n0, n0 + 3)) console.error('  ' + comp + ': ' + String(e).split('\n').slice(0, 2).join(' | ').slice(0, 250)); }
      } finally { pg.close(); }
      return;
    } else if (args[i] === '--each') { // debugging: like --shots, but a fresh boot per screen (a failing view breaks Mithril's tree)
      const list = args.slice(i + 1); i = args.length;
      for (const a of list) {
        const [comp, attrs] = a.includes('=') ? [a.slice(0, a.indexOf('=')), a.slice(a.indexOf('=') + 1)] : [a, '{}'];
        const pg = await boot();
        try { await pg.c.eval(CLEAN); await push(pg.c, comp, attrs); await still(pg.c, '_shot-' + comp);
          for (const e of pg.errors.concat(pg.consoleLines).slice(0, 4)) console.error('  ' + comp + ': ' + String(e).split('\n').slice(0, 2).join(' | ').slice(0, 250));
        } catch (e) { console.error('  FAIL ' + comp + ' ' + e.message.slice(0, 200)); } finally { pg.close(); }
      }
      return;
    } else if (args[i] === '--shot') { // debugging: boot, push <component> <attrs-js>, still to <out>/_shot-<component>
      const comp = args[++i], attrs = args[++i] || '{}';
      const pg = await boot();
      try { await pg.c.eval(CLEAN); await push(pg.c, comp, attrs); await still(pg.c, '_shot-' + comp); report(pg, comp);
        for (const e of pg.errors.concat(pg.consoleLines).slice(0, 8)) console.error('  ' + String(e).split('\n').slice(0, 2).join(' | '));
      } finally { pg.close(); }
      return;
    } else names.push(args[i]);
  }
  const list = names.length ? names : Object.keys(S);
  for (const n of list) { if (!S[n]) throw new Error('unknown state ' + n); await S[n](); }
})().catch((e) => { console.error(e); process.exit(1); });
