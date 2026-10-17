#!/usr/bin/env node
// Krunker v10.0.2 references for the Svelte popups of client/src/ui/v10_misc_popups.c and the ranked
// message overlay (client/src/ui/ranked_ui.c), taken from the OFFLINE v10 sandbox (doc/ref/krunker-v10/popups/).
// The sandbox rules are capture-v10.js's, kept exactly: a fresh profile per capture, --headless=new, no
// DISPLAY, the proxy on a closed port, every GET to krunker.io answered from the local mirror and the rest
// refused, no WebSocket, no beacons, a seeded Math.random, CSS animations paused at a chosen time.
//
//   node tools/uidiff/capture-v10-popups.js [--out doc/ref/krunker-v10] [--list] [name...]
// (also the Game Settings advanced tabs settings-advanced-{general,controls,game,sound}, into settings/)
//
// What each reference is (v10 reaches it, or the popup is named where it is not):
//   popup-kid        the k-ID dialog: the saved display mode gdc-kid (localStorage krunker-display-mode,
//                    v10:118600-118630) on a guest; the dialog mounts at boot (showingDisplayIntro, v10:222205)
//   popup-lottery    openLottery(3) (v10:215906): the lottery's API fails offline, so the window shows v10's
//                    failed-request box
//   popup-usernames  manageAccNames() with a signed-in account (the capture's setAcc hook)
//   popup-survey     the GAME_SURVEY notification, sent through the sandbox socket as the server sends it
//   ranked/ranked-home  openRankedMenu() signed in (FRVR's isLoggedIn, the $account store) with the ranked API
//                    answered from a fixture: the Ranked popup's Home tab
//   (the ranked message overlay's open sequence is capture-v10-seq-list.js's ranked-overlay-open)
// Not captured: popup-premium (the subscription branch sits in the else of a constant `() => !0x0` condition,
// v10:129547, so v10 never renders it) and popup-mission-briefing (the guest hook manager has config.enabled
// false, v10:118812). Those have no v10 reference: their scenarios are regression baselines only.
//
// Run: env -u DISPLAY -u WAYLAND_DISPLAY KN_CHROME=/usr/bin/chromium-browser node tools/uidiff/capture-v10-popups.js
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawn } = require('child_process');
const { CDP, sleep } = require('../kref/cdp.js');

const REPO = path.resolve(__dirname, '..', '..');
const HOME = os.homedir();
const OFFLINE = path.resolve(process.env.KN_V10_PUBLIC || path.join(HOME, '.cache/kn-v10/live/site/public'));
const CLIENT_JS = process.env.KN_V10_CLIENT_JS || path.join(HOME, '.cache/kn-v10/v10.js');
const V7_PUBLIC = process.env.KN_V7_PUBLIC || '/home/user/jails/kru/src/Krunker_v7.1.4/public';
const ASSET_DIRS = [path.join(HOME, '.cache/kn-v10/live/stage/assets'), path.join(HOME, '.cache/kn-v10/live/web/assets.krunker.io'),
  path.join(REPO, 'assets', 'v10'), path.join(REPO, 'assets'), V7_PUBLIC];
const PROXY = 'http://127.0.0.1:9'; // closed: nothing unanswered leaves the machine
const ANIM_MS = 800;
let OUT = path.join(REPO, 'doc', 'ref', 'krunker-v10');

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp', '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon', '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg',
  '.wav': 'audio/wav', '.mp4': 'video/mp4', '.webm': 'video/webm', '.txt': 'text/plain', '.obj': 'text/plain', '.glb': 'model/gltf-binary',
  '.gltf': 'model/gltf+json' };
const FRVR_STUB = `(() => { const noop = () => Promise.resolve(); const deep = (fixed) => new Proxy(fixed, { get: (t, k) => (k in t ? t[k] : (t[k] = deep(Object.assign(noop.bind(), {})))) });
  const none = () => Promise.resolve(null);
  window.FRVR = deep({ config: Object.assign({ abTests: {} }, window.FRVR && window.FRVR.config), init: () => Promise.resolve(), lifecycle: {},
    channel: deep({ getId: () => 'web' }), channelCharacteristics: { allowNavigation: true },
    auth: deep({ isLoggedIn: () => !!window.__knLoggedIn, isLoggingIn: false, getFreshAccessToken: () => Promise.resolve(window.__knLoggedIn ? 'kn-capture' : null), getCurrentPlatform: () => 'web', logout: none,
      tokenHandler: deep({ getRefreshToken: () => null, getAccessToken: () => null }) }),
    iap: deep({ getUnconsumedPurchases: () => Promise.resolve([]), purchase: () => Promise.reject(new Error('offline')), consumePurchase: none }),
    ads: deep({ AdType: { Interstitial: 0, Reward: 1, Banner: 2 }, AdShowResult: { Shown: 0, Skipped: 1, Failed: 2 }, show: () => Promise.resolve(2) }),
    tracker: deep({}), bootstrapper: deep({}), abTests: { abTestCohorts: {} } }); })();`;
let CURRENT_MAP = 'null'; // the capture's map JSON (openPage)
let CURRENT_API = {}; // the capture's API answers: { '/path': json } on api.krunker.io (openPage, o.api)
const MISSED = []; // API requests the sandbox answered 404 (KN_CAPTURE_DEBUG lists them)
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
  if (u.hostname === 'api.krunker.io' && Object.prototype.hasOwnProperty.call(CURRENT_API, '/' + p))
    return [JSON.stringify(CURRENT_API['/' + p]), '.json'];
  if (/^(gapi\.svc|api|matchmaker)\.krunker\.io$/.test(u.hostname)) { MISSED.push(u.hostname + u.pathname + u.search); return ''; }
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
    const Blocked = function () { throw new DOMException('blocked by capture-v10-popups.js', 'SecurityError'); };
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
  + ' setAcc: function (a) { _67362 = a; }, setAccStore: function (a) { _67362 = a; _10539.set(a); }, boot: function (x) { return _73861(x); }, survey: function (p) { return _70256(p); } }; };\n';
let clientJs = null;
function clientScript() {
  if (clientJs) return clientJs;
  const src = fs.readFileSync(CLIENT_JS, 'utf8');
  if (!src.includes(SCOPE_ANCHOR)) throw new Error('capture-v10-popups: the scope anchor is not in ' + CLIENT_JS);
  return (clientJs = Buffer.from(src.replace(SCOPE_ANCHOR, SCOPE_ANCHOR + SCOPE_HOOK)));
}

async function waitSandbox(c) {
  for (let i = 0; i < 60; i++) {
    if (await c.eval(`!!(window.__knScope && window.__knScope().sg && /CLICK TO PLAY/i.test(instructions.innerText))`).catch(() => false)) return 1;
    await sleep(500);
  }
  throw new Error('the v10 sandbox did not start');
}

const log = (m) => { if (process.env.KN_CAPTURE_DEBUG) console.error('[capture-v10-popups] ' + m); };
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
    CURRENT_API = o.api || {};
    MISSED.length = 0;
    const storage = Object.assign({ krunker_haslogin: 'true', consent: '1', custMap: CURRENT_MAP }, o.storage || {});
    await c.send('Page.addScriptToEvaluateOnNewDocument', { source: preScript(storage) });
    log('navigate'); await c.send('Page.navigate', { url: 'https://krunker.io/' });
    await sleep(o.settle);
  } catch (e) { close(); throw e; }
  return { c, errors, close };
}

// ---------------------------------------------------------------- scenarios

const S = {};
function scenario(name, run, opts = {}) { S[name] = Object.assign({ name, dir: 'popups', run, size: [1280, 720] }, opts); }

// the k-ID dialog: a guest whose saved display mode is gdc-kid (v10 mounts it at boot)
scenario('popup-kid', async (c) => {
  await waitSandbox(c);
  await c.eval(flatPage('#000000'));
  // the boot's guest branch (v10:221970 / 222212): the intro flag is set when the boot's map step runs,
  // which the sandbox never reaches on its own, so the capture runs that step (the k-ID dialog then mounts)
  const r = await c.eval(`(() => { try { window.__knScope().boot(true); return 1; } catch (e) { return String(e && e.message || e); } })()`);
  if (r !== 1) console.error('[capture-v10-popups] popup-kid: ' + r);
  await sleep(800);
}, { storage: { 'krunker-display-mode': 'gdc-kid', krunker_haslogin: null } });

// openLottery(3): the window's data request fails offline and the window shows v10's failed-request box
scenario('popup-lottery', async (c) => {
  await waitSandbox(c);
  await c.eval(flatPage('#000000'));
  await c.eval('(() => { openLottery(3); return 1; })()');
  await sleep(2500);
  await settle(c, 800);
});

// manageAccNames(): the Account window's Manage button, for a signed-in account (the setAcc hook)
scenario('popup-usernames', async (c) => {
  await waitSandbox(c);
  await c.eval(flatPage('#000000'));
  await c.eval(`(() => { window.__knScope().setAcc({ name: 'Sidney', level: 30, isDev: false, funds: 2500, score: 2600000,
    stats: {}, skins: [], badges: [], getSkinCount: () => 0 }); manageAccNames(); return 1; })()`);
  await sleep(1500);
  await settle(c, 800);
});

// The K.P.D popup (showPolicePop, v10:215977) for an officer (isMod, role 2 = Marshal): the Demos tab, its
// list answered by the /kpd/demos fixture (kpd.listDemos -> policeRes 4, v10:216006), and the Active
// Calls tab with its rows handed to policeRes 1 (v10:216110; the socket never answers offline).
const KPD_OFFICER = "{ name: 'Cop', level: 60, isDev: false, isMod: true, role: 2, funds: 2500, score: 200000, jobRating: 9, jobRatingPos: 6, " +
  "stats: {}, skins: [], badges: [], getSkinCount: () => 0 }";
const KPD_DEMO_GROUPS = [
  { grp: 7, name: '', gst: 0, ban: 0, demos: [
    { tok: 'tokA', ct: 'ctA', typ: 1, map: 'Kanji', mod: 'ffa', dat: 1700000000, dur: 125000, rsl: 0, opn: 1, yes: 3, f_no: 1, myv: 2 },
    { tok: 'tokB', ct: 'ctB', typ: 0, map: 'Sandstorm', mod: 'tdm', dat: 1700000100, dur: 60000, rsl: 1, opn: 0, yes: 0, f_no: 0, myv: null }] },
  { grp: 9, name: 'Bad_Ann', gst: 0, ban: 1, demos: [
    { tok: 'tokC', ct: '', typ: 0, map: 'Burg', mod: 'ffa', dat: 1700000200, dur: 0, rsl: 0, opn: 0, yes: 0, f_no: 0, myv: null }] },
  { grp: 11, name: '', gst: 1, ban: 0, demos: [
    { tok: 'tokD', ct: '', typ: 0, map: 'Burg', mod: 'ffa', dat: 1700000300, dur: 0, rsl: 0, opn: 0, yes: 0, f_no: 0, myv: null }] },
];
scenario('popup-kpd-demos', async (c) => {
  await waitSandbox(c);
  await c.eval(flatPage('#000000'));
  await c.eval(`(() => { window.__knScope().setAcc(${KPD_OFFICER}); switchKPDTab(3); return 1; })()`);
  // kpd.listDemos fails offline (its error text lands first); policeRes 4 with the same data as the API
  // answers it (v10:216006, the data is {rows, more}) then fills the list
  await sleep(2500);
  await c.eval(`(() => { policeRes(4, '', { rows: ${JSON.stringify(KPD_DEMO_GROUPS)}, more: 1 }); return 1; })()`);
  await settle(c, 800);
});
scenario('popup-kpd-calls', async (c) => {
  await waitSandbox(c);
  await c.eval(flatPage('#000000'));
  await c.eval(`(() => { window.__knScope().setAcc(${KPD_OFFICER}); showPolicePop(); policeRes(1, '', [
    { can: 'rep_one', con: 'suspect_a', gst: 0, gco: 'NODE:lobby1', typ: 1, jop: 5, jor: 6, tim: Date.now() / 1000 - 300, crs: 4000, cos: 90000, id: 1 },
    { can: 'rep_two', con: '', gst: 1, gco: 'NODE:lobby2', typ: 0, jop: 1, jor: 4, tim: Date.now() / 1000 - 3600, crs: 900, cos: 0, id: 2 }]); return 1; })()`);
  await sleep(800);
  await settle(c, 800);
});

// GAME_SURVEY as the server sends it (the survey's question and answers, expiresAt in unix seconds)
scenario('popup-survey', async (c) => {
  await waitSandbox(c);
  await c.eval(flatPage('#000000'));
  // the signed-in menu: v10's sign-in shows the chat holder (v10:210030) and the chat switch is on (v10:204189), so
  // #notificationCenter (inside the chat) is laid out. A guest's menu has no chat, so the survey shows nowhere there.
  await c.eval(`(() => { window.__knScope().setAcc({ name: 'Sidney', level: 30, isDev: false, funds: 2500, score: 2600000,
    stats: {}, skins: [], badges: [], getSkinCount: () => 0 }); chatUIHolder.style.display = 'block'; chatUI.style.display = 'block'; return 1; })()`);
  const r = await c.eval(`(() => { try {
      const expiresAt = Math.floor(Date.now() / 1000) + 120;
      window.__knScope().survey({ surveyId: 'kn-demo', question: 'How is your Krunker Box Office experience so far?',
        answers: ['Great', 'Good', 'Okay', 'Bad'], expiresAt, rewardAmount: 10 });
      return 1; } catch (e) { return String(e && e.message || e); } })()`);
  if (r !== 1) console.error('[capture-v10-popups] popup-survey: ' + r);
  await sleep(1200);
  console.error('[capture-v10-popups] survey dom: ' + await c.eval(`JSON.stringify({ items: document.querySelectorAll('.survey-notification').length, lists: document.querySelectorAll('.notification-list').length, center: document.querySelectorAll('.notification-item').length, nc: (document.getElementById('notificationCenter')||{}).outerHTML && document.getElementById('notificationCenter').outerHTML.slice(0,1400), ncl: (document.querySelector('.notification-item')||{}).outerHTML && document.querySelector('.notification-item').outerHTML.slice(0,600) })`));
  await settle(c, 800);
});

// Game Settings, advanced view: windows[0].settingType / tabIndex are the page's own tab state (v10:211036 toggleType,
// 211001 changeTab), set as capture-v7.js's win-settings-<tab> does; the ui/windows/settings-advanced-<n>.html tabs
// (General 0, Controls 1, Display 2, Render 3, Game 4, Sound 5). Guest, like the KN scenarios.
function settingsAdvanced(name, tab) {
  scenario(name, async (c) => {
    await waitSandbox(c);
    await c.eval(flatPage('#000000'));
    await c.eval(`(() => { closWind(); windows[0].settingType = 'advanced'; windows[0].tabIndex = ${tab}; windows[0].settingSearch = null; showWindow(1, true); return 1; })()`);
    await settle(c, 900);
  }, { dir: 'settings', storage: { krunker_haslogin: null } });
}
settingsAdvanced('settings-advanced-general', 0);
settingsAdvanced('settings-advanced-controls', 1);
settingsAdvanced('settings-advanced-game', 4);
settingsAdvanced('settings-advanced-sound', 5);

// the Ranked popup's Home tab (openRankedMenu, v10:215664 -> rankedMenuModal) for a signed-in level-60 player: the
// sign-in that creates the ranked client's session is FRVR's (the stub's isLoggedIn follows window.__knLoggedIn),
// the account goes into both the account object and the $account store, and the ranked API answers come from
// RANKED_API (the same player as tools/uidiff/scenarios/kn-ranked-home.json's state.ranked)
const RANKED_PROFILE = { player_name: 'Player', rank: 8, conservative_rating: 7.12, mmr: 1712, wins: 31, losses: 21, draws: 0,
  abandons: 1, total_kills: 812, total_deaths: 640, total_assists: 120, total_score: 51000, total_objective_score: 900,
  total_damage_done: 91000, total_time_played: 15600000, highest_kill_count: 34, highest_damage_in_match: 4100, peak_mmr: 1790,
  last_milestone_claimed: 1, total_accuracy: 0, total_matches: 52, placement_left: 0 };
const RANKED_MILESTONES = [{ matches: 10, reward: 100 }, { matches: 25, reward: 200 }, { matches: 50, reward: 350 },
  { matches: 75, reward: 500 }, { matches: 100, reward: 750 }];
function rankedApi() {
  const api = {};
  // _7307(): the season of today's date; the region the default region maps to (_34037: Europe = 3 without a setting)
  const now = new Date(); const season = now.getUTCFullYear() < 2025 ? 0 : 12 * (now.getUTCFullYear() - 2025) + (now.getUTCMonth() + 1 - 8 + 1);
  for (const s of [season, season - 1, 15]) for (const r of ['', '/region/3', '/region/1', '/region/2', '/region/0'])
    api[`/player-profile/me/season/${s}${r}`] = { data: { profile: Object.assign({}, RANKED_PROFILE), milestones: RANKED_MILESTONES } };
  api['/ranked/me/connection'] = { data: null };
  return api;
}
scenario('ranked-home', async (c) => {
  await waitSandbox(c);
  await c.eval(flatPage('#000000'));
  await c.eval(`(() => { window.__knLoggedIn = true; window.__knScope().setAccStore({ name: 'Player', level: 60, isDev: false, funds: 2500,
    score: 200000, rankedPoints: 1840, premiumT: 0, stats: {}, skins: [], badges: [], getSkinCount: () => 0 }); openRankedMenu(); return 1; })()`);
  await sleep(3000);
  await settle(c, 800);
}, { dir: 'ranked', api: rankedApi() });

// ---------------------------------------------------------------- main

async function capture(sc, idx) {
  const base = +process.env.KN_CAPTURE_PORT_BASE || 9560;
  const { c, close, errors } = await openPage({ w: sc.size[0], h: sc.size[1], port: base + (idx % 8), storage: sc.storage, api: sc.api, settle: 10000 });
  try {
    await sc.run(c);
    const dir = path.join(OUT, sc.dir);
    fs.mkdirSync(dir, { recursive: true });
    await c.shot(path.join(dir, sc.name + '.png'));
    console.error(`[capture-v10-popups] ${sc.dir}/${sc.name}.png (${errors.length} page errors)`);
    if (process.env.KN_CAPTURE_DEBUG) for (const m of MISSED) console.error('  404 ' + m);
    if (process.env.KN_CAPTURE_ERRORS) for (const e of errors.slice(0, 20)) console.error('  ' + String(e).split('\n')[0]);
  } finally { close(); }
}

(async () => {
  const args = process.argv.slice(2);
  const names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
    else if (args[i] === '--list') { console.log(Object.keys(S).join('\n')); return; }
    else names.push(args[i]);
  }
  const list = names.length ? names.map((n) => { if (!S[n]) throw new Error('unknown scenario ' + n); return S[n]; }) : Object.values(S);
  for (let i = 0; i < list.length; i++) await capture(list[i], i);
})().catch((e) => { console.error(e); process.exit(1); });
