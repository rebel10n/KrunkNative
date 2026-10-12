#!/usr/bin/env node
// Krunker v7.1.4 reference captures for tools/uidiff (doc/ref/krunker-v7.1.4/).
//
//   node tools/uidiff/capture-v7.js [--out doc/ref/krunker-v7.1.4] [--list] [name...]
//
// Every capture loads v7's own client (public/index.html or social.html) from a read-only
// static serve of v7's public/ tree in headless Chromium (tools/ui-import/v7_page.js; v7's
// npm server is never run: it writes into the reference tree). Without a matchmaker the page
// starts its local sandbox game; the captures undo the sandbox's menu chrome (CONNECTED_MENU)
// so they show what a returning guest connected to a server sees. Determinism:
//   - storage: krunker_haslogin (a returning player: v7's fullMenu, no FTUE / bot match),
//     consent, settings at their defaults; custMap = public/maps/kanji.json, so the sandbox
//     is Free for All on Kanji (loadSandbox; v7 otherwise picks a random map and mode);
//   - Math.random is a seeded generator installed before any page script
//     (Page.addScriptToEvaluateOnNewDocument), so the page's random picks repeat;
//   - the hook's session timer (the unlabelled "00:00:00" box top right) is removed;
//   - the 3D game canvas (RENDER.renderer.domElement) is transparent (opacity 0: it still
//     takes the mouse) and the page background is
//     the scenario's flat colour (#d6c6c6 on the menu, #000 in game), like KrunkNative's
//     --render-scenario paints `background` under the page; the class preview canvas stays
//     (its region is skipped);
//   - #mainLogoFace = img/menu/faces/f_0.png (index.html picks randInt(0, 15));
//   - #streamCon ("Loading Streams...") shown and #adCon hidden (the guest's featured panel
//     with no stream API, as KrunkNative shows it);
//   - CSS animations: every running animation is paused at ANIM_MS (document.getAnimations(),
//     currentTime = 800 ms), and the scenarios use anim_time 0.8 (KrunkNative's virtual clock
//     starts the page's animations at 0, so phase 0.8 s is the same frame);
//   - in game the sandbox is entered through v7's real pointer-lock path (requestPointerLock is
//     shimmed: document.pointerLockElement = the game canvas + a pointerlockchange event), then
//     HUD states are driven with real key input (Tab, R, Enter, Escape) and v7's own socket
//     handlers (Game.sandbox.events[...], the functions the server's packets call).
// Output: <out>/<dir>/<name>.png, <name>.html.gz (the DOM after the capture) and
// <name>.rects.json (viewport rects of the elements tools/uidiff/gen_scenarios.py builds regions from).
'use strict';
const fs = require('fs');
const path = require('path');
const os = require('os');
const { V7_PUBLIC, staticServer, launchChrome, CONNECTED_MENU, GUEST_STORAGE, sleep } = require('../ui-import/v7_page.js');
const { CDP } = require('../kref/cdp.js');

const REPO = path.resolve(__dirname, '..', '..');
const ANIM_MS = 800;
let OUT = path.join(REPO, 'doc', 'ref', 'krunker-v7.1.4');

// ---------------------------------------------------------------- page helpers

const MENU_BG = '#d6c6c6';

// Flat background instead of the 3D scene, pinned logo face, guest featured panel.
function flatPage(bg) {
  return `(() => {
    const cv = window.RENDER && RENDER.renderer && RENDER.renderer.domElement;
    if (cv) cv.style.opacity = '0';
    document.documentElement.style.background = '${bg}';
    document.body.style.background = '${bg}';
    const face = document.getElementById('mainLogoFace'); if (face) face.src = './img/menu/faces/f_0.png';
    const sc = document.getElementById('streamCon'); if (sc) sc.style.display = 'block';
    const ad = document.getElementById('adCon'); if (ad) ad.style.display = 'none';
    const ls = document.getElementById('loadingStreams'); if (ls) ls.style.display = '';
    for (const d of document.querySelectorAll('body > div')) if (!d.id && /^\\d\\d:\\d\\d:\\d\\d$/.test(d.textContent.trim())) d.remove();
    return 1; })()`;
}

// Viewport rects (x, y, w, h, rounded) of the visible elements regions are built from.
const RECT_IDS = ['windowHeader', 'menuWindow', 'windowHolder', 'signedOutHeaderBar', 'menuItemContainer', 'mainLogo',
  'gameNameHolder', 'seasonLabel', 'headerRight', 'menuServerInfoContainer', 'tlInfHold', 'mapInfoHld', 'subLogoButtons',
  'menuClassName', 'menuClassSubtext', 'menuClassIcn', 'menuClassNameTag', 'customizeButton', 'instructions',
  'instructionsUpdate', 'spectButton', 'streamContainer', 'infoHolder', 'termsInfo', 'chatUI', 'chatHolder',
  'chatList', 'chatInputHolder', 'timerHolder', 'topLeftMatchData', 'matchInfo', 'topLeftData', 'pingDisplay',
  'bottomLeftHolder', 'bottomLeftPlayer', 'bottomLeftHealth', 'bottomLeftHealthBar', 'bottomLeftClassImg', 'weapHolder',
  'ammoHolder', 'killFeed', 'centerLeaderDisplay', 'leaderContainerD', 'deathUIHolder', 'endUI', 'victorySub', 'endInfo',
  'endTimer', 'endTabbedView', 'endMidHolder', 'reloadMsg', 'aimDot', 'navBar', 'navTitle', 'profileInfo', 'navButtons',
  'loadMessage', 'genericPop', 'popupHolder', 'editrDB', 'steamDB', 'iosDB', 'androidDB'];
const RECTS = `(() => { const o = {}; for (const id of ${JSON.stringify(RECT_IDS)}) { const e = document.getElementById(id);
  if (!e) continue; const cs = getComputedStyle(e); if (cs.display === 'none' || cs.visibility === 'hidden') continue;
  const r = e.getBoundingClientRect(); if (r.width < 1 || r.height < 1) continue;
  o[id] = [Math.round(r.x), Math.round(r.y), Math.round(r.width), Math.round(r.height)]; } return o; })()`;

const PAUSE_ANIMS = `(() => { let n = 0; for (const a of document.getAnimations()) { try { a.pause(); a.currentTime = ${ANIM_MS}; n++; } catch (e) {} } return n; })()`;

// v7's real pointer-lock path: f2136 (controls.js) -> enterGame() + UI.toggleControlUI(true)
const LOCK_SHIM = `(() => {
  const cv = RENDER.renderer.domElement;
  Object.defineProperty(document, 'pointerLockElement', { configurable: true, get: () => window.__knLock || null });
  cv.requestPointerLock = function () { window.__knLock = cv; document.dispatchEvent(new Event('pointerlockchange')); };
  document.exitPointerLock = function () { window.__knLock = null; document.dispatchEvent(new Event('pointerlockchange')); };
  return 1; })()`;

async function key(c, k, code, keyCode, hold) {
  await c.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: k, code, windowsVirtualKeyCode: keyCode, nativeVirtualKeyCode: keyCode });
  if (hold) return;
  await sleep(60);
  await c.send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code, windowsVirtualKeyCode: keyCode, nativeVirtualKeyCode: keyCode });
}

async function menuBase(c) {
  await c.eval(CONNECTED_MENU);
  await c.eval(flatPage(MENU_BG));
  await sleep(500);
}

const DROP_TIMER = `(() => { for (const d of document.querySelectorAll('body > div')) if (!d.id && /^\\d\\d:\\d\\d:\\d\\d$/.test(d.textContent.trim())) d.remove(); return 1; })()`;

async function settle(c, ms = 600) {
  await sleep(ms);
  await c.eval(DROP_TIMER);
  await c.eval(PAUSE_ANIMS);
  await sleep(150);
}

// the match timer shows a fixed 03:45 (KrunkNative scenarios: hud.timerText): the element's
// text setters become no-ops after it is written
const PIN_TIMER = `(() => { const t = document.getElementById('timerVal'); if (!t) return 0; t.textContent = '03:45';
  for (const k of ['textContent', 'innerHTML', 'innerText']) Object.defineProperty(t, k, { configurable: true, set() {}, get() { return '03:45'; } });
  return 1; })()`;

async function enterGame(c) {
  await c.eval(CONNECTED_MENU);
  await c.eval(LOCK_SHIM);
  await c.eval('RENDER.renderer.domElement.requestPointerLock(), 1');
  await sleep(2500);
  await c.eval(flatPage('#000000'));
  await c.eval(PIN_TIMER);
}

// ---------------------------------------------------------------- scenarios
// name -> {dir, size, page, query, run(c)}

const S = {};
function scenario(name, dir, run, opts = {}) { S[name] = Object.assign({ name, dir, run, size: [1280, 720], page: 'index.html' }, opts); }

scenario('menu-1280', 'menu', async (c) => { await menuBase(c); await settle(c); });
scenario('menu-1920', 'menu', async (c) => { await menuBase(c); await settle(c); }, { size: [1920, 1080] });

// windows: showWindow(id) [+ the window's own tab call]
function win(name, id, extra) {
  scenario(name, 'windows', async (c) => {
    await menuBase(c);
    await c.eval(`(() => { closWind(); ${extra || ''} showWindow(${id}, true); return 1; })()`);
    await settle(c, 900);
  });
}
const SETTINGS_TABS = { general: 0, controls: 1, display: 2, render: 3, game: 4, sound: 5 };
for (const [tab, i] of Object.entries(SETTINGS_TABS)) {
  win('win-settings-' + tab, 1, `windows[0].settingType = 'advanced'; windows[0].tabIndex = ${i}; windows[0].settingSearch = null;`);
}
win('win-settings-basic', 1, `windows[0].settingType = 'basic'; windows[0].tabIndex = 0; windows[0].settingSearch = null;`);
// Servers: v7's list comes from the matchmaker's /game-list; three games in KrunkNative's
// regions (us-ca-sv / us-ca-la / us-il) stand in for it, tuples as the matchmaker sends them
// ([gameId, region, players, maxPlayers, {g: mode, i: map, v: version}]).
const MOCK_GAMES = `(() => { const v = 'egh9QTeIptdMdJIrIKaFXvKz9fCqj0mt';
  const games = [['us-ca-sv:knref1', 'us-ca-sv', 3, 8, {g: 0, i: 'Kanji', v}], ['us-ca-la:knref2', 'us-ca-la', 0, 8, {g: 0, i: 'Sandstorm', v}],
    ['us-il:knref3', 'us-il', 5, 8, {g: 1, i: 'Burg', v}]];
  client.fetchGameList = () => Promise.resolve({games, totalPlayerCount: 8});
  client.fetchGameInfo = (id) => Promise.resolve(games.find(g => g[0] === id) || games[0]);
  windows[1].lastLoadTime = 0; return 1; })()`;
scenario('win-servers', 'windows', async (c) => {
  await menuBase(c);
  await c.eval(MOCK_GAMES);
  await c.eval(`(() => { closWind(); showWindow(2, true); return 1; })()`);
  await sleep(1500);
  await c.eval(`(() => { showWindow(2, true); return 1; })()`);
  await settle(c, 900);
});
win('win-loadout', 3);
win('win-classes', 3, `windows[2].tabIndex = 0;`);
win('win-mods', 4);
win('win-profile-login', 5);
win('win-keybinds', 7, `windows[6].tabIndex = 0;`);
win('win-host', 8);
win('win-spray', 9);
win('win-games', 10);
win('win-shop', 14);
win('win-join', 24);
win('win-download', 25);
win('win-ranked', 27);
win('win-attach', 31);
win('win-reticle', 32);
win('win-challenges', 33);
win('win-changelog', 35);
win('win-host-type', 42);
win('win-server-finder', 48);

// in game
async function hudBase(c) { await enterGame(c); await settle(c, 800); }
scenario('ingame-plain', 'ingame', hudBase);
scenario('ingame-scoreboard', 'ingame', async (c) => {
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
scenario('ingame-reloading', 'ingame', async (c) => {
  await enterGame(c);
  await shoot(c, 3);
  await key(c, 'r', 'KeyR', 82);
  await settle(c, 300);
});
scenario('ingame-chat-open', 'ingame', async (c) => {
  await enterGame(c);
  await key(c, 'Enter', 'Enter', 13);
  await sleep(300);
  await c.send('Input.insertText', { text: 'gg' });
  await settle(c, 500);
});
scenario('ingame-pause-esc', 'ingame', async (c) => {
  await enterGame(c);
  await c.eval('document.exitPointerLock(), 1');
  await c.eval(flatPage('#000000'));
  await settle(c, 800);
});
// a bot through v7's own spawnBot bind (pressButton(107): sandboxGame.bots.add + the
// "Spawned a Bot" chat line), then the kill packet "3" from the sandbox's socket handlers
const SPAWN_BOT = `(() => { pressButton(107); return 1; })()`;
const BOT_SID = `(Game.players.list.find(p => !p.isYou) || {}).sid`;
async function withBot(c) { await enterGame(c); await c.eval(SPAWN_BOT); await sleep(1500); }
// kills through the sandbox server (game.js players.kill(victim, killer, info)): the "3"
// packet, the kill feed and the death screen as a real match sends them
const SERVER_BOT = `sandboxGame.players.list.find(p => p.isBot)`;
scenario('ingame-killfeed', 'ingame', async (c) => {
  await withBot(c);
  await c.eval(`(() => { sandboxGame.players.kill(${SERVER_BOT}, sandboxPlayer, {}); return 1; })()`);
  await settle(c, 500);
});
scenario('ingame-death', 'ingame', async (c) => {
  await withBot(c);
  await c.eval(`(() => { sandboxGame.players.kill(sandboxPlayer, ${SERVER_BOT}, {}); return 1; })()`);
  await settle(c, 800);
});
scenario('ingame-death-self', 'ingame', async (c) => {
  await enterGame(c);
  await c.eval(`(() => { sandboxGame.players.kill(sandboxPlayer, null, {}); return 1; })()`);
  await settle(c, 800);
});
scenario('ingame-scoped', 'ingame', async (c) => {
  await enterGame(c);
  await c.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: 640, y: 360, button: 'right', buttons: 2, clickCount: 1 });
  await settle(c, 900);
});
// the sandbox's own game end (game.js endGame(): the "end" packet, v7's end animation)
scenario('ingame-endscreen', 'ingame', async (c) => {
  await enterGame(c);
  await c.eval(`(() => { sandboxGame.endGame(); return 1; })()`);
  await settle(c, 1200);
});
// The results that follow config.endAnim (GUI.endScreen tabs) are not captured: io_endGame
// only schedules them for a connected socket, and the sandbox never reaches f1911 even with
// io.connected forced (the local player is deactivated by the sandbox's own end first).

scenario('hub-maps', 'hub', async (c) => { await settle(c, 1500); }, { page: 'social.html', query: 'p=maps' });
scenario('hub-feed', 'hub', async (c) => { await settle(c, 1500); }, { page: 'social.html', query: 'p=feed' });
scenario('hub-leaders', 'hub', async (c) => { await settle(c, 1500); }, { page: 'social.html', query: 'p=leaders' });
scenario('hub-market', 'hub', async (c) => { await settle(c, 1500); }, { page: 'social.html', query: 'p=market' });
scenario('hub-mods', 'hub', async (c) => { await settle(c, 1500); }, { page: 'social.html', query: 'p=mods' });
scenario('hub-login', 'hub', async (c) => { await c.eval('loginPopup(), 1'); await settle(c, 1000); }, { page: 'social.html', query: 'p=maps' });
scenario('hub-settings', 'hub', async (c) => { await c.eval('openSettings(), 1'); await settle(c, 1000); }, { page: 'social.html', query: 'p=maps' });

// ---------------------------------------------------------------- main

// Seeded Math.random (mulberry32) before any page script. Also v7.1.4's reconstruction bug
// `requestAnimationFrame(fn, delay)` (a decompiled setTimeout: the death screen's deathDelay,
// copyGameLink, ...) runs fn after `delay` like the original game did (KrunkNative fixes it
// the same way), otherwise the death screen is gone the next frame.
const SEED_RNG = `(() => { let a = 0x4b4e; Math.random = function () { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a);
  t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };
  if (location.pathname.indexOf('social.html') >= 0) {
    // the hub's social socket (config.socialURL/ws): no server here, so a socket that opens
    // and never answers (the guest's chrome with every view waiting for its data)
    window.WebSocket = class { constructor() { this.readyState = 0; setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 50); }
      send() {} close() {} static get OPEN() { return 1; } };
  }
  const raf = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = function (fn, delay) { return typeof delay === 'number' ? setTimeout(fn, delay) : raf(fn); };
})();`;
// Kanji with its game config narrowed to Free for All (the map's own config lists 13 modes,
// of which the sandbox plays a random one)
const MAP_JSON = (() => { const m = JSON.parse(fs.readFileSync(path.join(V7_PUBLIC, 'maps', 'kanji.json'), 'utf8'));
  if (m.config) m.config.modes = [0]; return JSON.stringify(m); })();

async function openPage(o) {
  const server = await staticServer(V7_PUBLIC, o.httpPort);
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'kn-v7ref-chrome-'));
  const chrome = launchChrome(o.port, o.w, o.h, profile);
  const c = await CDP.connect(o.port);
  await c.send('Runtime.enable');
  await c.send('Page.enable');
  const errors = [];
  c.on((m) => { if (m.method === 'Runtime.exceptionThrown') errors.push((m.params.exceptionDetails.exception || {}).description || m.params.exceptionDetails.text); });
  await c.setViewport(o.w, o.h);
  const base = `http://127.0.0.1:${o.httpPort}/`;
  await c.send('Page.navigate', { url: base + 'manifest.json' });
  await sleep(300);
  const storage = Object.assign({}, GUEST_STORAGE, o.page === 'index.html' ? { custMap: MAP_JSON } : {});
  await c.eval(`(() => { localStorage.clear(); const s = ${JSON.stringify(storage)}; for (const k in s) localStorage.setItem(k, s[k]); return 1; })()`);
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: SEED_RNG });
  await c.send('Page.navigate', { url: base + o.page + (o.query ? '?' + o.query : '') });
  await sleep(o.settle);
  const close = () => { try { c.close(); } catch (e) { } try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { } server.close(); fs.rmSync(profile, { recursive: true, force: true }); };
  return { c, errors, close };
}

async function capture(sc, idx) {
  const { c, close, errors } = await openPage({ w: sc.size[0], h: sc.size[1], port: 9500 + (idx % 8), httpPort: 6900 + (idx % 8),
    page: sc.page, query: sc.query || '', settle: 12000 });
  try {
    await sc.run(c);
    const dir = path.join(OUT, sc.dir);
    fs.mkdirSync(dir, { recursive: true });
    await c.shot(path.join(dir, sc.name + '.png'));
    // layout of the elements the regions are built from (gen_scenarios.py): viewport rects
    fs.writeFileSync(path.join(dir, sc.name + '.rects.json'), JSON.stringify(await c.eval(RECTS), null, 0) + '\n');
    // the DOM after the capture, gzipped (the window markup makes some dumps 700 KB)
    fs.writeFileSync(path.join(dir, sc.name + '.html.gz'), require('zlib').gzipSync(Buffer.from(await c.eval('document.documentElement.outerHTML'), 'utf8'), { level: 9 }));
    console.error(`[capture-v7] ${sc.dir}/${sc.name}.png (${errors.length} page errors)`);
  } finally { close(); }
}

(async () => {
  const args = process.argv.slice(2);
  const names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
    else if (args[i] === '--list') { console.log(Object.keys(S).join('\n')); return; }
    else if (args[i] === '--probe') { // debugging: enter the game and print the result of <js>
      const js = args[++i];
      const { c, close } = await openPage({ w: 1280, h: 720, port: 9509, httpPort: 6909, page: 'index.html', settle: 12000 });
      try { await enterGame(c); console.log(JSON.stringify(await c.eval(js), null, 1)); } finally { close(); }
      return;
    }
    else names.push(args[i]);
  }
  const list = names.length ? names.map((n) => { if (!S[n]) throw new Error('unknown scenario ' + n); return S[n]; }) : Object.values(S);
  for (let i = 0; i < list.length; i++) await capture(list[i], i);
})().catch((e) => { console.error(e); process.exit(1); });
