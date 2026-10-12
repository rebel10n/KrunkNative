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
  'loadMessage', 'genericPop', 'popupHolder', 'editrDB', 'steamDB', 'iosDB', 'androidDB',
  // player-viewer WP-E part 2a: spectating and the minimap
  'spectateHUD', 'specNames', 'specTimer', 'specGameInfo', 'specTeam0', 'specTeam1', 'specControlHolder', 'miniMapOverlay',
  'topLeftMain',
  // the prize wheel screen (win-spin*)
  'spinWindow', 'spinInfo', 'spinItems', 'spinButton', 'spinKR', 'spinHistoryBtn', 'spinItem', 'spinItemCanvas',
  'spinItemName', 'spinUIBtns', 'spinNextBtn', 'spinAgainBtn', 'spinEquipBtn', 'spinHistoryPop', 'shopSideSellB'];
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

// The Lost Connection screen: io_error's UI steps (showWindow(0), CONTROLS.disable(),
// UI.hideUI(), UI.hideDiscon(), then f2040: #menuHolder hidden, #instructionsUpdate with the
// error and the instructionsTabs row) without its f2039(0) reload. The error text is
// KrunkNative's connect error (menu_ui.c CONNECT_ERROR_DEFAULT).
const DISCONNECT_SIZES = { '1280': [1280, 720], '1920': [1920, 1080], '1024': [1024, 768] };
for (const [tag, size] of Object.entries(DISCONNECT_SIZES)) {
  scenario('menu-disconnected-' + tag, 'menu', async (c) => {
    await menuBase(c);
    const ex = await exposeBundle(c, ['f2040', 'UI', 'CONTROLS']);
    if (ex !== 1) console.error('[capture-v7] menu-disconnected: the bundle scope was not reached');
    const r = await c.eval(`(() => { try { showWindow(0); __kn.CONTROLS.disable(); __kn.UI.hideUI(); __kn.UI.hideDiscon();
      __kn.f2040('Could not connect to the game server', '000000'); return 1; } catch (e) { return String(e.stack); } })()`);
    if (r !== 1) console.error('[capture-v7] menu-disconnected: ' + r);
    await c.eval(flatPage(MENU_BG));
    await settle(c, 900);
  }, { size });
}

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
// the store's Starter prize wheel as a guest (no KR box): prizeWheel(index, skin, multi, autoSpin,
// noAnim) with noAnim, so the wheel is at its full size at once (spinWhlMlt = 1); the four
// preview icons are random picks (seeded here, rand() in KrunkNative)
// win-spin-item*: the won item's reveal, signed in (season_fixture.json's account, as the
// win-battlepass captures) so the spin can land: v7's own io_spinResponse(null, randNum 40,
// skin 15 "Digital AK" (Uncommon), funds) after the wheel opened, then the ~7 s spin, the reveal
// and the item's fly-in (the 3D item keeps bobbing: sin(t) * 0.55 units).
const SPIN_SIZES = [['', [1280, 720]], ['-1920', [1920, 1080]], ['-1024', [1024, 768]], ['-portrait', [720, 1280]]];
for (const [tag, size] of SPIN_SIZES) {
  scenario('win-spin' + tag, 'windows', async (c) => {
    await menuBase(c);
    await c.eval('(() => { closWind(); prizeWheel(0, null, false, false, true); return 1; })()');
    await settle(c, 1200);
  }, { size });
  scenario('win-spin-item' + tag, 'windows', async (c) => {
    const acc = seasonAccount('free');
    await menuBase(c);
    const tuple = new Array(49).fill(null);
    Object.assign(tuple, { 0: 1, 1: 0, 2: 0, 3: 0, 4: 0, 5: acc.funds || 0, 6: acc.score || 0, 7: null, 8: 0, 9: 0, 10: [], 11: 0,
      12: 0, 13: 0, 14: 0, 15: acc.stats || {}, 16: 0, 21: 0, 23: acc.name, 44: [], 45: [], 47: 0 });
    const ex = await exposeBundle(c, ['io_accountResponse', 'io_spinResponse']);
    if (ex !== 1) console.error('[capture-v7] win-spin-item: the bundle scope was not reached');
    const r = await c.eval(`(() => { try { closWind();
      __kn.io_accountResponse(null, 1, ${JSON.stringify(acc.name)}, ${JSON.stringify(tuple)}, null);
      prizeWheel(0, null, false, false, true); __kn.io_spinResponse(null, 40, 15, ${(acc.funds || 0) - 50}, null);
      return 1; } catch (e) { return String(e.stack); } })()`);
    if (r !== 1) console.error('[capture-v7] win-spin-item: ' + r);
    // the pointer approaches its target by a fixed share per frame (~430 frames): poll for the reveal
    for (let i = 0; i < 120 && await c.eval('spinItem.style.display') !== 'block'; i++) await sleep(1000);
    await sleep(3000); // the fly-in
    await settle(c, 600);
  }, { size });
}
// Signed in with spin tickets (two Season 7 Starter, one Ultra) and the Free KR wheel claimable:
// the Store's home tab (Heroic card: 1x / 10x / Redeem), its Prize Wheels tab, and the wheel's
// Spin History popup (io_saveSpinHistory with SPIN_HISTORY's rows)
const SPIN_SKINS = [{ ind: 6423, cnt: 2 }, { ind: 6426, cnt: 1 }, { ind: 15, cnt: 1 }];
const SPIN_HISTORY = [{ ind: 15, dte: 1791800000000, fds: 50 }, { ind: 5996, dte: 1791790000000, fds: 50 },
  { ind: 6043, dte: 1791780000000, fds: 100 }, { ind: 6062, dte: 1791000000000, fds: 0 }, { ind: 6087, dte: 1790000000000, fds: 500 }];
async function spinAccount(c, extra) {
  const acc = seasonAccount('free');
  await menuBase(c);
  const tuple = new Array(49).fill(null);
  Object.assign(tuple, { 0: 1, 1: 0, 2: 0, 3: 0, 4: 0, 5: acc.funds || 0, 6: acc.score || 0, 7: null, 8: 0, 9: 0, 10: SPIN_SKINS, 11: 0,
    12: 0, 13: 0, 14: 0, 15: acc.stats || {}, 16: 0, 21: 0, 23: acc.name, 44: [], 45: [], 47: 0 });
  let ok = false; // the pause can land outside the bundle (hook.js: every name null): retry
  for (let i = 0; i < 8 && !ok; i++) {
    await exposeBundle(c, ['io_accountResponse', 'io_saveSpinHistory']).catch(() => 0);
    ok = await c.eval("typeof (window.__kn || {}).io_accountResponse === 'function'");
  }
  if (!ok) console.error('[capture-v7] spinAccount: the bundle scope was not reached');
  const r = await c.eval(`(() => { try { closWind();
    __kn.io_accountResponse(null, 1, ${JSON.stringify(acc.name)}, ${JSON.stringify(tuple)}, null); ${extra}
    return 1; } catch (e) { return String(e.stack); } })()`);
  if (r !== 1) console.error('[capture-v7] spinAccount: ' + r);
}
scenario('win-shop-spin', 'windows', async (c) => {
  await spinAccount(c, 'windows[13].tabIndex = 0; windows[13].subTab = 0; showWindow(14, true);');
  await settle(c, 1500);
});
scenario('win-shop-wheels', 'windows', async (c) => {
  await spinAccount(c, 'windows[13].tabIndex = 1; showWindow(14, true);');
  await settle(c, 1500);
});
scenario('win-spin-history', 'windows', async (c) => {
  await spinAccount(c, `prizeWheel(0, null, false, false, true); __kn.io_saveSpinHistory(${JSON.stringify(SPIN_HISTORY)});`);
  await settle(c, 1500);
});
// win-spin-multi: the Starter wheel's 10x spin (prizeWheel(0, null, 10)) answered with ten rolls
// and items (a brain_social_ws run's), revealed: the highest rarity first and "Next Item"
const MULTI = { rolls: [11.3775, 56.5799, 73.6815, 57.9684, 81.7105, 26.9725, 24.0595, 82.6168, 13.3432, 51.2703],
  items: [6043, 6062, 6083, 6020, 5996, 6087, 6083, 6033, 6082, 6031] };
scenario('win-spin-multi', 'windows', async (c) => {
  const acc = seasonAccount('free');
  await menuBase(c);
  const tuple = new Array(49).fill(null);
  Object.assign(tuple, { 0: 1, 1: 0, 2: 0, 3: 0, 4: 0, 5: acc.funds || 0, 6: acc.score || 0, 7: null, 8: 0, 9: 0, 10: [], 11: 0,
    12: 0, 13: 0, 14: 0, 15: acc.stats || {}, 16: 0, 21: 0, 23: acc.name, 44: [], 45: [], 47: 0 });
  const ex = await exposeBundle(c, ['io_accountResponse', 'io_spinResponse']);
  if (ex !== 1) console.error('[capture-v7] win-spin-multi: the bundle scope was not reached');
  const r = await c.eval(`(() => { try { closWind();
    __kn.io_accountResponse(null, 1, ${JSON.stringify(acc.name)}, ${JSON.stringify(tuple)}, null);
    prizeWheel(0, null, 10, false, true);
    __kn.io_spinResponse(null, ${JSON.stringify(MULTI.rolls)}, ${JSON.stringify(MULTI.items)}, ${(acc.funds || 0) - 500}, null);
    return 1; } catch (e) { return String(e.stack); } })()`);
  if (r !== 1) console.error('[capture-v7] win-spin-multi: ' + r);
  for (let i = 0; i < 480 && await c.eval('spinItem.style.display') !== 'block'; i++) await sleep(1000);
  await sleep(3000);
  await settle(c, 600);
});
win('win-join', 24);
win('win-download', 25);
win('win-ranked', 27);
win('win-attach', 31);
win('win-reticle', 32);
win('win-challenges', 33);
win('win-changelog', 35);
win('win-host-type', 42);
win('win-server-finder', 48);

// season-system: the signed-in Battle Pass / Challenges windows. The account, the pass and the
// challenge list are tools/uidiff/season_fixture.json (the scenarios' state.account, gen_scenarios.py),
// handed to v7's own socket handlers ("a" io_accountResponse with the Account data tuple, "bpdl"
// windows[5].dataLoaded, "bppd" io_syncBpProg; exposeBundle). The pass's startdate keeps the
// scenario's distance to its "now" (the Available BPP bar). Run with KN_V7_OVERLAY=ui so the
// images v7 loaded from its CDN (img/chals/*) come from KrunkNative's fetched copies.
// v7's bundle is one IIFE: its socket handlers are not globals. Pausing in a frame of the bundle
// (the render loop) and evaluating there copies them to window.__kn.
async function exposeBundle(c, names) {
  await c.send('Debugger.enable');
  const paused = new Promise((res) => c.on((m) => { if (m.method === 'Debugger.paused') res(m.params); }));
  await c.send('Debugger.pause');
  const p = await paused;
  const frame = p.callFrames.find((f) => /\/js\/index\.js/.test(f.url)) || p.callFrames[0];
  const expr = `(window.__kn = {${names.map((n) => `${n}: typeof ${n} !== 'undefined' ? ${n} : null`).join(', ')}}, 1)`;
  const r = await c.send('Debugger.evaluateOnCallFrame', { callFrameId: frame.callFrameId, expression: expr, returnByValue: true });
  await c.send('Debugger.resume');
  await c.send('Debugger.disable');
  return r && r.result ? r.result.value : null;
}

// the account variants of gen_scenarios.py season_account()
function seasonAccount(kind) {
  const acc = JSON.parse(fs.readFileSync(path.join(REPO, 'tools', 'uidiff', 'season_fixture.json'), 'utf8'));
  if (kind === 'vip') Object.assign(acc.battlepass.progress, { tier: 1, items_claimed: '20001,21001,21004' });
  if (kind === 'chal') { acc.battlepass.tab = 1; acc.challenges = acc.challenges.filter((x) => x.T !== 'weekly'); }
  if (kind === 'profile') Object.assign(acc, PROFILE_ACCOUNT);
  return acc;
}
// profile-layout: the signed-in Profile window with a long name, a clan and a high level
// (gen_scenarios.py PROFILE_ACCOUNT, the same overrides)
const PROFILE_ACCOUNT = { name: 'xXLongPlayerNameXx', clan: 'KRUNK', score: 12500000, stats: { s: 50000, h: 21000, hs: 3000, c0: 40000 } };

function seasonWin(name, window, kind, opts) {
  scenario(name, 'windows', async (c) => {
    const acc = seasonAccount(kind), bp = acc.battlepass || {};
    const sc = { state: { window } };
    await menuBase(c);
    const tuple = new Array(49).fill(null);
    Object.assign(tuple, { 0: 1, 1: 0, 2: 0, 3: 0, 4: 0, 5: acc.funds || 0, 6: acc.score || 0, 7: acc.clan || null, 8: 0, 9: 0, 10: [], 11: 0,
      12: 0, 13: 0, 14: 0, 15: acc.stats || {}, 16: 0, 21: acc.premium || 0, 23: acc.name, 44: acc.challenges || [], 45: acc.badges || [],
      47: bp.progress ? bp.progress.bpp : 0 });
    const data = bp.data ? Object.assign({}, bp.data) : null;
    if (data && bp.now) data.startdate = `__START__`;
    const shift = data && bp.now ? (bp.now * 1000 - Date.parse(bp.data.startdate)) : 0;
    const ex = await exposeBundle(c, ['io_accountResponse', 'io_syncBpProg']);
    if (ex !== 1) console.error('[capture-v7] ' + name + ': the bundle scope was not reached');
    const r = await c.eval(`(() => { try { closWind();
      __kn.io_accountResponse(null, 1, ${JSON.stringify(acc.name)}, ${JSON.stringify(tuple)}, null);
      const data = ${JSON.stringify(data)}; if (data && data.startdate === '__START__') data.startdate = new Date(Date.now() - ${shift}).toISOString();
      windows[5].dataLoaded(data); __kn.io_syncBpProg(${JSON.stringify(bp.progress || null)});
      windows[5].tabIndex = ${bp.tab ? 1 : 0}; windows[32].openChallenge = null;
      showWindow(${sc.state.window}, true); return 1; } catch (e) { return String(e.stack); } })()`);
    if (r !== 1) console.error('[capture-v7] ' + name + ': ' + r);
    await settle(c, 1500);
  }, opts);
}
seasonWin('win-battlepass', 6, 'free');
seasonWin('win-battlepass-vip', 6, 'vip');
seasonWin('win-battlepass-chal', 6, 'chal');
seasonWin('win-challenges-acc', 33, 'free');
// the Profile window (windows[4]: this.width = account ? 1280 : 955) at the menu sizes
const PROFILE_SIZES = { '1280': [1280, 720], '1920': [1920, 1080], '1024': [1024, 768] };
for (const [tag, size] of Object.entries(PROFILE_SIZES)) seasonWin('win-profile-acc-' + tag, 5, 'profile', { size });

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
// player-viewer WP-E part 2a. The death screen with what an account brings: the killer's class
// level ("3" [1]: account.stats["c" + classIndex]), its player card stat ("3" [2]: pcStatIndex 3,
// KDR from the account's kills / deaths) and this life's stats ("3" [3]), set on the sandbox
// server's players before its real players.kill. The player card banners are left out (their
// thumbnails are WebP, which KrunkNative's native HTML engine does not decode yet).
scenario('ingame-death-v7', 'ingame', async (c) => {
  await withBot(c);
  const r = await c.eval(`(() => { const bot = ${SERVER_BOT};
    bot.account = Object.assign({activeChallenges: []}, bot.account || {}, {kills: 300, deaths: 120, games: 40, wins: 25, score: 45000});
    bot.account.stats = Object.assign({}, (bot.account && bot.account.stats) || {}, {['c' + bot.classIndex]: 2500});
    bot.anonMode = false; bot.pcStatIndex = 3;
    sandboxPlayer.totShotsL = 42; sandboxPlayer.hitShotsL = 16; sandboxPlayer.killsL = 3; sandboxPlayer.scoreL = 450;
    sandboxPlayer.spawnTime = Date.now() - 65000;
    try { sandboxGame.players.kill(sandboxPlayer, bot, {}); } catch (e) { return String(e.stack); } return 1; })()`);
  if (r !== 1) console.error('[capture-v7] ingame-death-v7 kill: ' + r);
  await settle(c, 800);
  console.error('[capture-v7] ingame-death-v7 killer: ' + JSON.stringify(await c.eval(`(() => { const b = ${SERVER_BOT};
    return {cls: b.classIndex, weapon: b.weapon && b.weapon.name, icon: b.weapon && b.weapon.icon, name: b.name}; })()`)));
});
// Spectating the sandbox (setSpect(true) before the pointer lock: io "ent" with the spectate
// flag): the free cam's #spectateUI with #specControlHolder and the player list.
scenario('ingame-spectate', 'ingame', async (c) => {
  await c.eval(CONNECTED_MENU);
  await c.eval(SPAWN_BOT);
  await sleep(800);
  await c.eval('setSpect(true), 1');
  await c.eval(LOCK_SHIM);
  await c.eval('RENDER.renderer.domElement.requestPointerLock(), 1');
  await sleep(2500);
  await c.eval(flatPage('#000000'));
  await c.eval(PIN_TIMER);
  await settle(c, 800);
});
// The minimap (Minimap.js): v7 production hides it on custom maps (customsData && isProd), and
// the captures load their map as one, so customsData is cleared and the minimap reset as for an
// official map. The pose v7's sandbox spawned at is logged for the KrunkNative scenario's `pose`
// (gen_scenarios.py: [x, y, z, xDr, yDr]). ingame-minimap is Kanji (the captures' default map);
// the others load another map (`map`: v7's public/maps/<file>, or a KrunkNative assets/maps/ file
// for the v3.1.1 maps v7 does not ship): sandstorm / burg draw their mmId image, 311-burg (no
// mmId) draws its blocks.
function minimapScenario(name, map) {
  scenario(name, 'ingame', async (c) => {
    await enterGame(c);
    await c.eval(`(() => { Game.customsData = null; toggleMinimap(true); return 1; })()`);
    await settle(c, 800);
    console.error(`[capture-v7] ${name} pose: ` + JSON.stringify(await c.eval(`(() => { const p = Game.players.list.find(q => q.isYou);
      return p ? [p.x, p.y, p.z, CONTROLS.xDr, CONTROLS.yDr, CONTROLS.object.position.x, CONTROLS.object.position.z] : null; })()`)));
  }, map ? { map } : {});
}
minimapScenario('ingame-minimap');
minimapScenario('ingame-minimap-sandstorm', 'sandstorm_v3.json');
minimapScenario('ingame-minimap-burg', 'burg_new.json');
minimapScenario('ingame-minimap-311-burg', 'kn:311_burg.json');

// the sandbox's own game end (game.js endGame(): the "end" packet, v7's end animation)
scenario('ingame-endscreen', 'ingame', async (c) => {
  await enterGame(c);
  await c.eval(`(() => { sandboxGame.endGame(); return 1; })()`);
  await settle(c, 1200);
});
// The chat at other window sizes: #chatHolder sits in #uiBase (scaleBase: scale = min(w / 2210,
// h / 1170) with scaleUI 0.7) at its .onMenu place (left 150, bottom 20) in the menu and its
// .onGame place (left 15, bottom 130) in game; ESC drops .onGame (toggleControlUI(false)) and the
// chat goes back beside the menu. chat-ingame-lines writes sandbox lines until
// #chatList reaches its max-height (chatHeight 2.5 = 250 px) and scrolls to the newest.
const CHAT_SIZES = { '1024': [1024, 768], tall: [720, 1280] };
for (const [tag, size] of Object.entries(CHAT_SIZES)) {
  scenario('menu-' + tag, 'menu', async (c) => { await menuBase(c); await settle(c); }, { size });
  scenario('ingame-pause-' + tag, 'ingame', async (c) => {
    await withBot(c);
    await c.eval('document.exitPointerLock(), 1');
    await c.eval(flatPage('#000000'));
    await settle(c, 800);
  }, { size });
}
for (const [tag, size] of Object.entries(Object.assign({ '1920': [1920, 1080] }, CHAT_SIZES))) {
  scenario('ingame-chat-' + tag, 'ingame', async (c) => { await withBot(c); await settle(c, 800); }, { size });
}
// ESC while spectating: toggleControlUI(false) drops .onGame, toggleMenuHider(true) .onSpect, so
// the chat leaves the spectator's corner for its .onMenu place.
scenario('ingame-spectate-menu', 'ingame', async (c) => {
  await c.eval(CONNECTED_MENU);
  await c.eval(SPAWN_BOT);
  await sleep(800);
  await c.eval('setSpect(true), 1');
  await c.eval(LOCK_SHIM);
  await c.eval('RENDER.renderer.domElement.requestPointerLock(), 1');
  await sleep(2500);
  await c.eval('document.exitPointerLock(), 1');
  await c.eval(flatPage('#000000'));
  await settle(c, 800);
});
// the sandbox has no chat server ("ct" is never answered): its spawnBot / clearBots binds
// (pressButton(107) / (109)) write "Spawned a Bot" / "Cleared Bots" lines instead
async function chatLines(c, n) {
  for (let i = 0; i < n; i++) {
    await c.eval(`(() => { pressButton(${i % 2 ? 109 : 107}); return 1; })()`);
    await sleep(120);
  }
}
scenario('ingame-chat-lines', 'ingame', async (c) => { await enterGame(c); await chatLines(c, 12); await settle(c, 800); });

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
// The sandbox's map (Kanji unless the scenario names one: "<file>" in v7's public/maps, or
// "kn:<file>" in KrunkNative's assets/maps) with its game config narrowed to Free for All (the
// map's own config lists up to 13 modes, of which the sandbox plays a random one)
function mapJson(file) {
  const kn = /^kn:/.test(file || '');
  const src = kn ? path.join(REPO, 'assets', 'maps', file.slice(3)) : path.join(V7_PUBLIC, 'maps', file || 'kanji.json');
  const m = JSON.parse(fs.readFileSync(src, 'utf8'));
  if (m.config) m.config.modes = [0];
  else m.config = { modes: [0] };
  return JSON.stringify(m);
}

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
  const storage = Object.assign({}, GUEST_STORAGE, o.page === 'index.html' ? { custMap: mapJson(o.map) } : {});
  await c.eval(`(() => { localStorage.clear(); const s = ${JSON.stringify(storage)}; for (const k in s) localStorage.setItem(k, s[k]); return 1; })()`);
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: SEED_RNG });
  await c.send('Page.navigate', { url: base + o.page + (o.query ? '?' + o.query : '') });
  await sleep(o.settle);
  const close = () => { try { c.close(); } catch (e) { } try { process.kill(-chrome.pid, 'SIGKILL'); } catch (e) { } server.close(); fs.rmSync(profile, { recursive: true, force: true }); };
  return { c, errors, close };
}

async function capture(sc, idx) {
  // KN_CAPTURE_PORT_BASE moves the Chromium debug ports (base + 0..7) and the static server
  // (base + 1000 + 0..7) off the defaults, for parallel runs.
  const base = +process.env.KN_CAPTURE_PORT_BASE || 0;
  const { c, close, errors } = await openPage({ w: sc.size[0], h: sc.size[1], port: (base || 9500) + (idx % 8), httpPort: (base ? base + 1000 : 6900) + (idx % 8),
    page: sc.page, query: sc.query || '', map: sc.map, settle: 12000 });
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
