#!/usr/bin/env node
// Krunker v10.0.2 in-game reference captures for tools/uidiff (doc/ref/krunker-v10/ingame/), taken
// from the OFFLINE v10 sandbox: the pinned v10.0.2 client (~/.cache/kn-v10/v10.js) on the mirrored
// krunker.io page, in sandboxed headless Chromium, with nothing leaving the machine.
//
//   node tools/uidiff/capture-v10.js [--out doc/ref/krunker-v10] [--list] [--probe <js>] [name...]
//
// RUNS KRUNKER'S CLIENT (the page's own JS), with the user's approval for sandboxed UI capture
// (2026-10-13). The sandbox rules are tools/ui-import/capture_v10_live.js's offline mode, kept exactly:
//   - a fresh profile per capture (deleted on exit), --headless=new, DISPLAY / WAYLAND_DISPLAY removed;
//   - the proxy is a closed port (127.0.0.1:9), so nothing that is not answered from disk can leave;
//   - Fetch-domain interception answers every GET/HEAD to krunker.io / *.krunker.io from the local
//     mirror (KN_V10_PUBLIC, default ~/.cache/kn-v10/live/site/public; the page keeps its
//     https://krunker.io/ origin) and then from local asset trees (ASSET_DIRS: the v10 live asset
//     stage, KrunkNative's assets/, v7.1.4's public/), and fails everything else (other hosts, POST);
//   - the page's module script (static/index-*.js) is KN_V10_CLIENT_JS (default ~/.cache/kn-v10/v10.js);
//     libs/ the mirror lacks come from v7.1.4's public/libs; FRVR's SDK is an inert stub (logged out);
//   - window.WebSocket throws (set before any page script) and navigator.sendBeacon is a no-op:
//     never a matchmaker, game, social or queue socket; no login, no cookies or tokens of ours.
// With localStorage custMap set, v10's boot (v10:222911) starts its in-browser sandbox game
// (_73956: the server's Game object _69422 runs in the page, the local player is _69423), the
// same path v7's captures used (tools/uidiff/capture-v7.js). Determinism as capture-v7.js:
// a seeded Math.random, the timer pinned to 03:45, CSS animations paused at 800 ms, the 3D canvas
// transparent over a flat background (#000 in game), the pointer lock shimmed (pointerlockchange).
//
// Output: <out>/ingame/<name>.png, <name>.html.gz (the DOM after the capture), <name>.rects.json.
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const zlib = require('zlib');
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
  // the game API (gapi.svc.krunker.io / api.krunker.io) GET /maps: the boot awaits the map list
  // before it starts the sandbox, and the client generates map 0 of that list (custMap is the
  // sandbox server's). The list is the capture's map alone, so both sides play it.
  if (/^(gapi\.svc|api)\.krunker\.io$/.test(u.hostname) && p === 'maps') return ['{"data":[' + CURRENT_MAP + ']}', '.json'];
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

// The sandbox's map: v7.1.4's public/maps/kanji.json (v10 loads v7 maps) narrowed to one mode,
// as capture-v7.js does (`mode`: the index in v10's mode list, gameplay.md; 0 = Free for All).
const V10_MODE_INDEX = { ffa: 0, tdm: 1, point: 2, ctf: 3, kc: 19, dom: 25 };
function mapJson(file, mode) {
  const kn = /^kn:/.test(file || '');
  const src = kn ? path.join(REPO, 'assets', 'maps', file.slice(3)) : path.join(V7_PUBLIC, 'maps', file || 'kanji.json');
  const m = JSON.parse(fs.readFileSync(src, 'utf8'));
  const modes = [V10_MODE_INDEX[mode || 'ffa']];
  if (m.config) m.config.modes = modes; else m.config = { modes };
  return JSON.stringify(m);
}

// Before any page script: guest storage (a returning guest: krunker_haslogin, consent), the
// sandbox map, no WebSocket / beacons, seeded Math.random, the pointer-lock shim.
function preScript(storage) {
  return `(() => { const s = ${JSON.stringify(storage)};
    try { for (const k in s) localStorage.setItem(k, s[k]); } catch (e) {}
    window.__knMap = s.custMap;
    const Blocked = function () { throw new DOMException('blocked by capture-v10.js', 'SecurityError'); };
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

// the WebGL canvas the renderer made (v10's renderer.domElement), transparent over a flat page
const GAME_CANVAS = `(() => { let best = null; for (const cv of document.querySelectorAll('canvas')) {
  if (['game-overlay', 'miniMapCanvas'].includes(cv.id) || cv.closest('#uiBase')) continue;
  if (!best || cv.width * cv.height > best.width * best.height) best = cv; } return best; })()`;
function flatPage(bg) {
  return `(() => { const cv = ${GAME_CANVAS}; if (cv) cv.style.opacity = '0';
    document.documentElement.style.background = '${bg}'; document.body.style.background = '${bg}'; return 1; })()`;
}
const PAUSE_ANIMS = `(() => { let n = 0; for (const a of document.getAnimations()) { try { a.pause(); a.currentTime = ${ANIM_MS}; n++; } catch (e) {} } return n; })()`;
const PIN_TIMER = `(() => { const t = document.getElementById('timerVal'); if (!t) return 0; t.textContent = '03:45';
  for (const k of ['textContent', 'innerHTML', 'innerText']) Object.defineProperty(t, k, { configurable: true, set() {}, get() { return '03:45'; } });
  return 1; })()`;
const PIN_TEXT = (id, text) => `(() => { const t = document.getElementById(${JSON.stringify(id)}); if (!t) return 0; t.textContent = ${JSON.stringify(text)};
  for (const k of ['textContent', 'innerHTML', 'innerText']) Object.defineProperty(t, k, { configurable: true, set() {}, get() { return ${JSON.stringify(text)}; } });
  return 1; })()`;
const PIN_GMSG = (text) => `(() => { const t = document.getElementById('gameMessage'); if (!t) return 0; t.textContent = ${JSON.stringify(text)};
  for (const k of ['textContent', 'innerHTML', 'innerText']) Object.defineProperty(t, k, { configurable: true, set() {}, get() { return ${JSON.stringify(text)}; } });
  return 1; })()`;
const HIDE_OVERLAY = `(() => { const o = document.getElementById('game-overlay'); if (o) o.style.opacity = '0'; return 1; })()`;

const RECT_IDS = ['timerHolder', 'topLeftMatchData', 'matchInfo', 'topLeftData', 'pingDisplay', 'topLeftHolder', 'topLeftMain',
  'miniMapOverlay', 'bottomLeftHolder', 'bottomLeftPlayer', 'bottomLeftHealth', 'bottomLeftHealthBar', 'bottomLeftClassImg',
  'weapHolder', 'ammoHolder', 'killFeed', 'killStreakHolder', 'centerLeaderDisplay', 'leaderContainerD', 'deathUIHolder', 'endUI',
  'victorySub', 'endInfo', 'endTimer', 'endTabbedView', 'endMidHolder', 'reloadMsg', 'aimDot', 'aimRecticle', 'chatUI', 'chatUIHolder',
  'chatHolder', 'chatList', 'chatInputHolder', 'spectateUI', 'spectateHUD', 'specNames', 'specTimer', 'specGameInfo', 'specControlHolder',
  'teamScores', 'gameMessage', 'topMidHold', 'interactMsg', 'instructionHolder', 'instructions', 'menuHolder', 'windowHolder', 'uiBase',
  'topRight', 'leaderboardHolder', 'streakVal', 'perkHolder', 'bottomRight', 'killCount', 'deathCount', 'streakCount', 'kdCount', 'scoreCount',
  'killsIcon', 'deathsIcon', 'streakIcon', 'kdIcon', 'menuWindow', 'windowHeader', 'playerSearchH', 'playerListH', 'voteToKick'];
const RECTS = `(() => { const o = {}; for (const id of ${JSON.stringify(RECT_IDS)}) { const e = document.getElementById(id);
  if (!e) continue; const cs = getComputedStyle(e); if (cs.display === 'none' || cs.visibility === 'hidden') continue;
  const r = e.getBoundingClientRect(); if (r.width < 1 || r.height < 1) continue;
  o[id] = [Math.round(r.x), Math.round(r.y), Math.round(r.width), Math.round(r.height)]; } return o; })()`;

async function key(c, k, code, keyCode, hold) {
  await c.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: k, code, windowsVirtualKeyCode: keyCode, nativeVirtualKeyCode: keyCode });
  if (hold) return;
  await sleep(60);
  await c.send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code, windowsVirtualKeyCode: keyCode, nativeVirtualKeyCode: keyCode });
}
async function settle(c, ms = 600) {
  await sleep(ms);
  await c.eval(PAUSE_ANIMS);
  await sleep(150);
}

// v10's module scope is reached through one accessor added to the served client script (the
// only change to it, after `var _69423 = null;`, v10:207629): window.__knScope() returns the
// sandbox server game (_69422), its local player (_69423), the client game (_67298) and the
// client's own player (_69415) and the client socket (_33978).
const SCOPE_ANCHOR = '  var _69422 = null;\n  var _69423 = null;\n';
// setAcc(a) stands a signed-in account (_67362, v10's iîïììîí) in for the Player List capture: the
// K.P.D and Votekick buttons are an account's.
const SCOPE_HOOK = '  window.__knScope = function () { return { sg: _69422, sp: _69423, cg: _67298, me: _69415, sock: _33978,'
  + ' setAcc: function (a) { _67362 = a; } }; };\n';
let clientJs = null;
function clientScript() {
  if (clientJs) return clientJs;
  const src = fs.readFileSync(CLIENT_JS, 'utf8');
  if (!src.includes(SCOPE_ANCHOR)) throw new Error('capture-v10: the scope anchor is not in ' + CLIENT_JS);
  return (clientJs = Buffer.from(src.replace(SCOPE_ANCHOR, SCOPE_ANCHOR + SCOPE_HOOK)));
}
const SG = 'window.__knScope().sg', SP = 'window.__knScope().sp', CG = 'window.__knScope().cg';

async function waitSandbox(c) {
  for (let i = 0; i < 60; i++) {
    log('waitSandbox ' + i);
    if (await c.eval(`!!(window.__knScope && ${SG} && /CLICK TO PLAY/i.test(instructions.innerText))`).catch(() => false)) return 1;
    await sleep(500);
  }
  throw new Error('the v10 sandbox did not start');
}

async function lock(c) {
  await c.eval(`(() => { const cv = ${GAME_CANVAS}; cv.requestPointerLock(); return 1; })()`);
}
async function enterGame(c) {
  await waitSandbox(c);
  await lock(c);
  for (let i = 0; i < 40 && !await c.eval(`!!${SP}`).catch(() => false); i++) await sleep(250);
  await sleep(2500);
  await c.eval(flatPage('#000000'));
  await c.eval(PIN_TIMER);
}

// ---------------------------------------------------------------- scenarios

const S = {};
function scenario(name, run, opts = {}) { S[name] = Object.assign({ name, dir: 'ingame', run, size: [1280, 720] }, opts); }

scenario('ingame-plain', async (c) => { await enterGame(c); await settle(c, 800); });
scenario('ingame-scoreboard', async (c) => {
  await enterGame(c);
  await key(c, 'Tab', 'Tab', 9, true);
  await settle(c, 800);
});
async function shoot(c, n) {
  for (let i = 0; i < n; i++) {
    await c.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: 640, y: 360, button: 'left', buttons: 1, clickCount: 1 });
    await sleep(60);
    await c.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 640, y: 360, button: 'left', buttons: 0, clickCount: 1 });
    await sleep(250);
  }
}
scenario('ingame-reloading', async (c) => {
  await enterGame(c);
  await shoot(c, 3);
  await key(c, 'r', 'KeyR', 82);
  await settle(c, 300);
});
scenario('ingame-scoped', async (c) => {
  await enterGame(c);
  await c.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: 640, y: 360, button: 'right', buttons: 2, clickCount: 1 });
  await settle(c, 900);
});
scenario('ingame-chat-open', async (c) => {
  await enterGame(c);
  await key(c, 'Enter', 'Enter', 13);
  await sleep(300);
  await c.send('Input.insertText', { text: 'gg' });
  await settle(c, 500);
});
scenario('ingame-pause-esc', async (c) => {
  await enterGame(c);
  await c.eval('document.exitPointerLock(), 1');
  await sleep(300);
  await c.eval(flatPage('#000000'));
  await settle(c, 800);
});
// the Player List (pListKey -> showWindow(23)) of a populated lobby: three sandbox bots made
// players with accounts (level, accid, clan, ping), one flagged isHacker, a level-30 account (the
// K.P.D button from level 5, Votekick for accounts; the sandbox is not custom for genList)
scenario('ingame-playerlist', async (c) => {
  await enterGame(c);
  for (let i = 0; i < 3; i++) { await c.eval(SPAWN_BOT); await sleep(700); }
  await sleep(1200);
  const r = await c.eval(`(() => { try {
    const s = window.__knScope(), cg = s.cg;
    const rows = [['Kanzaki', 'VIP', 34, 12], ['Rook', '', 87, 7], ['Sable', 'CMD', 162, 3]];
    const others = cg.players.list.filter(p => !p.isYou);
    others.forEach((p, i) => { const d = rows[i % rows.length]; p.alias = p.name = d[0]; p.clan = d[1]; p.ping = d[2];
      p.level = d[3]; p.accid = 9100 + i; p.isBot = false; p.isHacker = i == 2; });
    cg.isCustom = false; cg.isRanked = false;
    s.setAcc({ level: 30, isDev: false, canTeleport: false, name: 'Local User', stats: {}, skins: [], badges: [], getSkinCount: () => 0 });
    document.exitPointerLock();
    window.showWindow(23);
    return others.length; } catch (e) { return String(e.stack); } })()`);
  if (typeof r !== 'number') console.error('[capture-v10] playerlist: ' + r);
  await sleep(300);
  await c.eval(flatPage('#000000'));
  await settle(c, 800);
});
// a sandbox bot: v10's spawnBot bind (pressButton(107), as in v7) adds one through the
// sandbox server's bots; kills go through the sandbox server's players.kill(victim, killer, info)
const SPAWN_BOT = `(() => { window.pressButton(107); return 1; })()`;
const SERVER_BOT = `${SG}.players.list.find(p => p.isBot)`;
async function withBot(c) { await enterGame(c); await c.eval(SPAWN_BOT); await sleep(1500); }
scenario('ingame-chat-lines', async (c) => {
  await enterGame(c);
  for (let i = 0; i < 12; i++) { await c.eval(`(() => { window.pressButton(${i % 2 ? 109 : 107}); return 1; })()`); await sleep(120); }
  await settle(c, 800);
});
scenario('ingame-killfeed', async (c) => {
  await withBot(c);
  const r = await c.eval(`(() => { try { ${SG}.players.kill(${SERVER_BOT}, ${SP}, {}); return 1; } catch (e) { return String(e.stack); } })()`);
  if (r !== 1) console.error('[capture-v10] killfeed: ' + r);
  await settle(c, 500);
});
scenario('ingame-death', async (c) => {
  await withBot(c);
  const r = await c.eval(`(() => { try { ${SG}.players.kill(${SP}, ${SERVER_BOT}, {}); return 1; } catch (e) { return String(e.stack); } })()`);
  if (r !== 1) console.error('[capture-v10] death: ' + r);
  await settle(c, 800);
});
scenario('ingame-death-self', async (c) => {
  await enterGame(c);
  const r = await c.eval(`(() => { try { ${SG}.players.kill(${SP}, null, {}); return 1; } catch (e) { return String(e.stack); } })()`);
  if (r !== 1) console.error('[capture-v10] death-self: ' + r);
  await settle(c, 800);
});
// spectating: setSpect(true) before the pointer lock. v10's sandbox then adds no player and sends
// no START_GAME (v10:208083 `if (!_69628)`), so the capture sends the START_GAME a server sends a
// spectator ('start': timer, team 0, spectating, ..., v10:208110) through the sandbox socket:
// v10's own handler (_72673) shows the free cam's #spectateUI.
async function spectate(c, esc) {
  await waitSandbox(c);
  await c.eval(SPAWN_BOT);
  await sleep(800);
  await c.eval('window.setSpect(true), 1');
  await lock(c);
  await sleep(1000);
  const r = await c.eval(`(() => { try { const g = ${SG}; g.sandbox.send(null, 'start', g.gameTimer, 0, true, 0, 0, 1, null, 0); g.players.sync(); g.players.syncLeaders(true); return 1; } catch (e) { return String(e.stack); } })()`);
  if (r !== 1) console.error('[capture-v10] spectate start: ' + r);
  await sleep(1500);
  if (esc) { await c.eval('document.exitPointerLock(), 1'); await sleep(300); }
  await c.eval(flatPage('#000000'));
  await c.eval(PIN_TIMER);
  await c.eval(PIN_TEXT('specTimer', '03:45'));
  await settle(c, 800);
}
scenario('ingame-spectate', (c) => spectate(c, false));
scenario('ingame-minimap', async (c) => {
  await enterGame(c);
  await c.eval(`(() => { ${CG}.customsData = null; window.toggleMinimap && window.toggleMinimap(true); return 1; })()`);
  await settle(c, 800);
  console.error('[capture-v10] ingame-minimap pose: ' + JSON.stringify(await c.eval(`(() => { const p = ${SP}; return p ? [p.x, p.y, p.z, p.xDire, p.yDire] : null; })()`)));
});
// the objective modes' HUD on Kanji, the sandbox playing that mode (overlay canvas hidden)
function modeScenario(name, mode, run) {
  scenario(name, async (c) => {
    await enterGame(c);
    await c.eval(HIDE_OVERLAY);
    if (run) await run(c);
    console.error(`[capture-v10] ${name}: ` + JSON.stringify(await c.eval(`(() => ({mode: ${CG}.mode && ${CG}.mode.alias,
      team: ${SP}.team, scores: ${CG}.teamScores, msg: gameMessage.textContent}))()`).catch((e) => String(e))));
    await settle(c, 800);
  }, { mode });
}
modeScenario('ingame-mode-point', 'point', async (c) => { await c.eval(PIN_GMSG('next objective 00:45')); });
modeScenario('ingame-mode-ctf', 'ctf', async (c) => { await c.eval(PIN_GMSG('')); });
// the top-right stat counters (#killCount, #deathCount, #streakCount, #kdCount, #scoreCount: all off
// by default) switched on through v10's own settings boot (kro_setngss_<key>, v10:207523, then each
// setting's set()), one sandbox kill, the figures forced to kn-ingame-counters' (3 kills, 1 death,
// streak 3, kdVal "3" = (3 / 1).round(1).toString(), score 225). The sandbox rewrites the values
// through paths PIN_TEXT's instance setters do not see, so FORCE_TEXT writes the text node itself
// and puts it back whenever the page changes it. -ds: only Deaths and Streak on (the commonest pair).
const FORCE_TEXT = (vals) => `(() => { const v = ${JSON.stringify(vals)}; let n = 0;
  for (const id in v) { const t = document.getElementById(id); if (!t) continue; n++;
    const put = () => { if (t.childNodes.length !== 1 || t.firstChild.nodeType !== 3 || t.firstChild.data !== v[id]) t.replaceChildren(document.createTextNode(v[id])); };
    put(); new MutationObserver(put).observe(t, { childList: true, characterData: true, subtree: true }); }
  return n; })()`;
const COUNTER_VALS = { killsVal: '3', deathsVal: '1', streakVal: '3', kdVal: '3', myScoreVal: '225' };
const COUNTER_RECTS = `(() => [...document.querySelectorAll('.topRightCounters > .statIcon')].filter(e => getComputedStyle(e).display !== 'none')
  .map(e => { const r = e.getBoundingClientRect(); return [e.id, Math.round(r.x * 100) / 100, Math.round(r.y * 100) / 100,
    Math.round(r.width * 100) / 100, Math.round(r.height * 100) / 100]; }))()`;
// -wide: the kill feed's victim renamed to a long name, so #killFeed (inline-grid) is wider than the floated
// #leaderboardHolder and sets #topRight's shrink-to-fit width: the counters then flow beside the float
const WIDE_VICTIM = 'Wintercrystal_Humpenscrump';
const WIDE_FEED = `(() => { const s = [...document.querySelectorAll('#killFeed .killfeedMsg span')].pop(); if (!s) return 0;
  s.textContent = ${JSON.stringify(WIDE_VICTIM)}; return 1; })()`;
function countersScenario(name, keys, wide) {
  const storage = {};
  for (const k of keys) storage['kro_setngss_' + k] = 'true';
  scenario(name, async (c) => {
    await withBot(c);
    await c.eval(`(() => { try { ${SG}.players.kill(${SERVER_BOT}, ${SP}, {}); return 1; } catch (e) { return String(e.stack); } })()`);
    await sleep(800);
    if (wide && await c.eval(WIDE_FEED) !== 1) console.error('[capture-v10] ' + name + ': no kill feed row');
    await c.eval(FORCE_TEXT(COUNTER_VALS));
    await c.eval(HIDE_OVERLAY);
    await settle(c, 800);
    console.error('[capture-v10] ' + name + ': ' + JSON.stringify(await c.eval(COUNTER_RECTS)));
  }, { storage });
}
countersScenario('ingame-counters', ['showKillC', 'showDeaths', 'showStreak', 'showKD', 'showScore']);
countersScenario('ingame-counters-ds', ['showDeaths', 'showStreak']);
countersScenario('ingame-counters-wide', ['showKillC', 'showDeaths', 'showStreak', 'showKD', 'showScore'], true);
countersScenario('ingame-counters-ds-wide', ['showDeaths', 'showStreak'], true);
// the menu's Host Game window (showWindow(42), host_type): its Community Games card draws one of
// gamecats/c_{0,2,4,6,10,13}.png at random (gen(), v10:215179); Math.random is held at 0 for that one
// call so the card is c_0, the banner the KrunkNative scenario pins (ui_windows.c gen_42)
scenario('menu-host-type', async (c) => {
  await waitSandbox(c);
  await c.eval(flatPage('#000000'));
  const r = await c.eval(`(() => { const rnd = Math.random; Math.random = () => 0; try { showWindow(42); return 1; } catch (e) { return String(e.stack); } finally { Math.random = rnd; } })()`);
  if (r !== 1) console.error('[capture-v10] menu-host-type: ' + r);
  await settle(c, 1500);
}, { dir: 'menu' });
// the sandbox's own game end (the "end" packet, v10's end animation + victory title)
scenario('ingame-endscreen', async (c) => {
  await enterGame(c);
  await c.eval(`(() => { ${SG}.endGame(); return 1; })()`);
  await settle(c, 1200);
});
// the results that follow the end animation: v10 builds them (_72911, GUI.endScreen.init) only
// for a connected socket (v10:219684), so the sandbox's socket says connected (its send stays the
// sandbox's no-op)
scenario('ingame-endscreen-late', async (c) => {
  await enterGame(c);
  await c.eval(`(() => { window.__knScope().sock.connected = true; ${SG}.endGame(); return 1; })()`);
  await settle(c, 7000);
});

// ---------------------------------------------------------------- main

const CLOSERS = new Set();
for (const sig of ['SIGINT', 'SIGTERM', 'SIGHUP']) process.on(sig, () => { for (const f of CLOSERS) f(); process.exit(130); });
process.on('exit', () => { for (const f of CLOSERS) f(); });
const log = (m) => { if (process.env.KN_CAPTURE_DEBUG) console.error('[capture-v10] ' + m); };

async function openPage(o) {
  const profile = fs.mkdtempSync(path.join(HOME, '.cache', 'kn-v10ref-chrome-'));
  const chrome = launchChrome(o.port, o.w, o.h, profile);
  let c = null;
  const close = () => { CLOSERS.delete(close); try { c && c.close(); } catch (e) { } try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { } fs.rmSync(profile, { recursive: true, force: true }); };
  CLOSERS.add(close);
  const blocked = [], errors = [], missing = [];
  try {
    log('chrome ' + chrome.pid); c = await CDP.connect(o.port); log('cdp connected');
    c.on((m) => {
      if (m.method === 'Fetch.requestPaused') {
        const p = m.params; const u = new URL(p.request.url); const meth = p.request.method;
        const why = !['http:', 'https:'].includes(u.protocol) ? 'scheme' : !ALLOW_HOST(u.hostname) ? 'host' : !['GET', 'HEAD'].includes(meth) ? 'method' : '';
        if (why) { blocked.push([meth, p.request.url, why]); c.send('Fetch.failRequest', { requestId: p.requestId, errorReason: 'BlockedByClient' }).catch(() => {}); return; }
        const f = offlineFile(u);
        const stub = Array.isArray(f);
        const body = stub ? Buffer.from(f[0]) : f ? fs.readFileSync(f) : Buffer.from('');
        const ext = stub ? f[1] : f ? path.extname(f).toLowerCase() : '';
        if (!f) missing.push(p.request.url);
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
    await c.setViewport(o.w, o.h);
    CURRENT_MAP = mapJson(o.map, o.mode);
    const storage = Object.assign({ krunker_haslogin: 'true', consent: '1', custMap: CURRENT_MAP }, o.storage || {});
    await c.send('Page.addScriptToEvaluateOnNewDocument', { source: preScript(storage) });
    // capture-v10-seq.js: extra pre-page script, virtual time before the first page script, a query
    if (o.preScript) await c.send('Page.addScriptToEvaluateOnNewDocument', { source: o.preScript });
    if (o.beforeNavigate) await o.beforeNavigate(c);
    log('navigate'); await c.send('Page.navigate', { url: 'https://krunker.io/' + (o.query || '') }); log('navigated');
    await sleep(o.settle);
  } catch (e) { close(); throw e; }
  return { c, errors, blocked, missing, close };
}

async function capture(sc, idx) {
  const base = +process.env.KN_CAPTURE_PORT_BASE || 9540;
  const { c, close, errors, blocked } = await openPage({ w: sc.size[0], h: sc.size[1], port: base + (idx % 8), map: sc.map, mode: sc.mode, storage: sc.storage, settle: 10000 });
  try {
    await sc.run(c);
    const dir = path.join(OUT, sc.dir);
    fs.mkdirSync(dir, { recursive: true });
    await c.shot(path.join(dir, sc.name + '.png'));
    fs.writeFileSync(path.join(dir, sc.name + '.rects.json'), JSON.stringify(await c.eval(RECTS), null, 0) + '\n');
    fs.writeFileSync(path.join(dir, sc.name + '.html.gz'), zlib.gzipSync(Buffer.from(await c.eval('document.documentElement.outerHTML'), 'utf8'), { level: 9 }));
    console.error(`[capture-v10] ${sc.dir}/${sc.name}.png (${errors.length} page errors, ${blocked.length} requests refused)`);
    if (process.env.KN_CAPTURE_ERRORS) for (const e of errors.slice(0, 20)) console.error('  ' + String(e).split('\n')[0]);
  } finally { close(); }
}

// the frame-sequence captures (capture-v10-seq.js) reuse the sandbox: openPage, enterGame, the
// page helpers; the CLI below runs only when this file is the entry point
module.exports = { REPO, openPage, enterGame, waitSandbox, lock, key, shoot, settle, flatPage, scenario, S, RECTS, PIN_TIMER, PIN_TEXT,
  PIN_GMSG, HIDE_OVERLAY, PAUSE_ANIMS, GAME_CANVAS, SPAWN_BOT, SERVER_BOT, SG, SP, CG, CLOSERS, setOut: (o) => { OUT = o; } };
if (require.main === module) (async () => {
  const args = process.argv.slice(2);
  const names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
    else if (args[i] === '--list') { console.log(Object.keys(S).join('\n')); return; }
    else if (args[i] === '--probe') { // debugging: enter the game and print the result of <js>
      const js = args[++i];
      const { c, close, errors } = await openPage({ w: 1280, h: 720, port: 9549, settle: 10000 });
      try { await enterGame(c); console.log(JSON.stringify(await c.eval(js), null, 1)); for (const e of errors.slice(0, 10)) console.error(String(e).split('\n')[0]); } finally { close(); }
      return;
    } else if (args[i] === '--probe-menu') { // debugging: the loaded page, no game entry
      const js = args[++i];
      const { c, close, errors, blocked, missing } = await openPage({ w: 1280, h: 720, port: 9549, settle: 15000 });
      process.on('exit', () => console.error('404: ' + missing.slice(0, 60).join('\n')));
      try { console.log(JSON.stringify(await c.eval(js), null, 1)); for (const e of errors.slice(0, 20)) console.error(String(e).split('\n').slice(0, 3).join(' | '));
        console.error('blocked: ' + blocked.slice(0, 20).map((b) => b.join(' ')).join('\n')); } finally { close(); }
      return;
    } else names.push(args[i]);
  }
  const list = names.length ? names.map((n) => { if (!S[n]) throw new Error('unknown scenario ' + n); return S[n]; }) : Object.values(S);
  for (let i = 0; i < list.length; i++) await capture(list[i], i);
})().catch((e) => { console.error(e); process.exit(1); });
