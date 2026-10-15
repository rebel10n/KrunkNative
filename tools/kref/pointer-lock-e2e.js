// Web pointer-lock cycles (mouse freezes, R41): the web build in headless Chromium, offline (the
// in-page sandbox server), driven like a player: click to play, move the mouse, lose the lock
// (Escape: the page's exitPointerLock, as the browser does), alt-tab (window blur + the lock lost),
// the developer console, a menu window, and back in each time. After every cycle the mouse must
// turn the camera again, and no movement may be thrown away while the game shows with the pointer
// locked (main.c kn_look_probe "frozen": the freeze the players saw).
//   node tools/kref/pointer-lock-e2e.js <cdpPort> <pageUrl> [cycles]      exit 0 = all passed
'use strict';
const path = require('path');
const { CDP, sleep } = require(path.join(__dirname, 'cdp.js'));
const CDPP = +process.argv[2], URL = process.argv[3], CYCLES = +(process.argv[4] || 3);
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  -- ' + String(detail).slice(0, 300) : ''}`); };
const W = 960, H = 540;

(async () => {
  const c = await CDP.connect(CDPP);
  const logs = [];
  c.on(m => { if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map(a => a.value !== undefined ? String(a.value) : a.description).join(' ')); });
  await c.send('Runtime.enable'); await c.send('Page.enable');
  await c.setViewport(W, H);
  await c.send('Page.navigate', { url: URL });
  // the game's look probe plus the browser's own signed movementX sum under the lock (the turn the
  // mouse asked for; a test-only listener next to main.c's web_take_look one)
  const probe = async () => JSON.parse(await c.eval(`(()=>{const cv=document.getElementById('canvas');
    if (cv && !window.__knMv) { window.__knMv = {x: 0}; cv.addEventListener('mousemove', (e) => { if (document.pointerLockElement === cv) window.__knMv.x += e.movementX || 0; }); }
    const p = typeof _kn_look_probe === 'function' ? JSON.parse(UTF8ToString(_kn_look_probe())) : {};
    p.net = window.__knMv ? window.__knMv.x : 0; return JSON.stringify(p)})()`).catch(() => '{}'));
  const ui = () => c.eval(`(()=>{const d=id=>{const e=document.getElementById(id);return e?getComputedStyle(e).display:'-'};
    return {game: d('inGameUI') === 'block', menu: d('menuHolder') !== 'none' && d('instructionHolder') !== 'none', lock: !!document.pointerLockElement}})()`).catch(() => ({}));
  const until = async (fn, t = 8000) => { const t0 = Date.now(); let v; while (Date.now() - t0 < t) { v = await fn(); if (v) return v; await sleep(100); } return v; };
  let mx = W / 2, my = H / 2;
  // relative motion under the lock (movementX from the event positions): one direction per call,
  // towards the far side of the centre, so a call never nets out to no turn
  const wiggle = async (n = 12) => {
    const dir = mx > W / 2 ? -25 : 25;
    for (let i = 0; i < n; i++) { mx += dir; await c.mouse('mouseMoved', mx, my, 'none'); await sleep(30); }
  };
  const play = async () => {
    await c.click(W / 2, H / 2); // CLICK TO PLAY (#instructionHolder: requestPointerLock on the gesture)
    mx = W / 2; my = H / 2;      // (the click moved the cursor there)
    return until(async () => { const s = await ui(); return s.game && s.lock && !s.menu ? s : null; });
  };
  const turns = async (label) => {
    await c.mouse('mouseMoved', mx, my, 'none'); // Chrome's cursor where we think it is (a lock change can leave it elsewhere)
    await sleep(250);
    const a = await probe();
    await wiggle();
    await sleep(300);
    const b = await probe();
    const s = await ui();
    check(`${label}: in game with the pointer locked`, s.game && s.lock && !s.menu, JSON.stringify(s));
    const net = b.net - a.net;
    check(`${label}: the mouse turns the camera`, b.applied > a.applied && net !== 0 && Math.abs(b.yaw - a.yaw) > 1e-3,
          `yaw ${a.yaw} -> ${b.yaw}, applied ${a.applied} -> ${b.applied}, net movementX ${net}`);
    check(`${label}: no movement dropped in game (no freeze)`, b.frozen === a.frozen, `frozen ${a.frozen} -> ${b.frozen} px in ${b.frozenFrames - a.frozenFrames} frames`);
  };

  const ready = await until(async () => logs.some(l => /\[sandbox\] Loaded map|connected to server/.test(l)), 120000);
  check('the offline game loads', ready);
  await until(async () => /CLICK TO PLAY/i.test(await c.eval("(document.getElementById('instructions')||{}).textContent||''").catch(() => '')), 30000);
  check('click to play enters the game', await play());
  await turns('first entry');

  for (let i = 1; i <= CYCLES; i++) {
    // Escape: the browser leaves the lock (the page sees pointerlockchange with no element)
    await c.eval('document.exitPointerLock()');
    const menu = await until(async () => { const s = await ui(); return s.menu && !s.lock ? s : null; });
    check(`cycle ${i} escape: the menu comes back, the pointer is free`, menu, JSON.stringify(await ui()));
    const p0 = await probe();
    await wiggle(6); // a free cursor over the menu turns nothing
    check(`cycle ${i} escape: the menu cursor does not turn the camera`, (await probe()).yaw === p0.yaw);
    check(`cycle ${i} escape: click to play again`, await play());
    await turns(`cycle ${i} after escape`);

    // alt-tab: the window loses focus and the browser drops the lock
    await c.eval("window.dispatchEvent(new Event('blur')); document.exitPointerLock()");
    await sleep(400);
    await c.eval("window.dispatchEvent(new Event('focus'))");
    check(`cycle ${i} alt-tab: the menu comes back`, await until(async () => (await ui()).menu));
    check(`cycle ${i} alt-tab: click to play again`, await play());
    await turns(`cycle ${i} after alt-tab`);

    // the developer console (backtick) opens over the game and closes again
    await c.key('`', 'Backquote', 192);
    await until(async () => !(await ui()).lock, 3000);
    await sleep(300);
    await c.key('`', 'Backquote', 192);
    await sleep(500);
    if (!(await ui()).game || !(await ui()).lock) await play(); // v10: closing the console may leave CLICK TO PLAY
    await turns(`cycle ${i} after the console`);

    // a menu window (Settings) opened from the menu and closed
    await c.eval('document.exitPointerLock()');
    await until(async () => (await ui()).menu);
    await c.eval("typeof showWindow === 'function' ? showWindow(1) : (document.querySelector('[onclick*=\"showWindow(1)\"]') || {click(){}}).click()").catch(() => {});
    await sleep(600);
    await c.key('Escape', 'Escape', 27); // closes the window
    await sleep(400);
    check(`cycle ${i} window: click to play again`, await play());
    await turns(`cycle ${i} after a window`);
  }
  const fin = await probe();
  console.log('[pointer-lock] probe', JSON.stringify(fin));
  console.log(`${results.filter(Boolean).length}/${results.length} passed`);
  process.exit(results.length && results.every(Boolean) ? 0 : 1);
})().catch(e => { console.log('FAIL  ' + e.message); process.exit(1); });
