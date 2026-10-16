'use strict';
// Web READ-usage e2e: the web client in headless Chromium must not copy GPU pixels back to the CPU
// every frame. The class preview (html_dom_canvas_gl, web_dom.c) used to read its render target
// back through pixel-pack buffers each frame; Chromium then printed "READ-usage buffer was written,
// then fenced, but written again before being read back" (about 50 to 73 a minute in game, about 256
// a menu session) until "WebGL: too many errors" stopped the console. This test counts both lines
// over a fixed window in the menu (class preview on screen) and in game (the pause menu, which shows the
// preview, opens every few rounds), and fails on any of them.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/readusage-e2e.js [menu-seconds] [game-seconds]
const { openPage, sleep, stackEnv } = require('./session.js');

const MENU_SECONDS = Number(process.argv[2] || 30);
const GAME_SECONDS = Number(process.argv[3] || 30);
const READ_USAGE = /READ-usage/;
const TOO_MANY = /WebGL: too many errors/;

(async () => {
  const env = stackEnv();
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  const count = (lines, re) => lines.filter((l) => re.test(l)).length;
  const page = await openPage(env.BASE + '/?offline=1');
  try {
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders (Quick Match button visible)');
    // the class preview is on screen: its frames are being delivered to the page canvas
    let frames0 = 0;
    for (let i = 0; i < 60; i++) {
      frames0 = await page.cdp.eval("(window.__knCanvasGl || {}).frames || 0").catch(() => 0);
      if (frames0 > 0) break;
      await sleep(500);
    }
    check(frames0 > 0, `the class preview delivers frames to the page (${frames0} so far)`);

    const first = page.log.all.length;
    const t0 = Date.now();
    await sleep(MENU_SECONDS * 1000);
    const menuSince = page.log.all.slice(first);
    const menuReadCount = count(menuSince, READ_USAGE);
    const menuTooMany = count(menuSince, TOO_MANY);
    const frames1 = await page.cdp.eval("(window.__knCanvasGl || {}).frames || 0").catch(() => 0);
    console.log(`menu ${Math.round((Date.now() - t0) / 1000)} s: READ-usage ${menuReadCount}, too many errors ${menuTooMany}, preview frames ${frames1 - frames0}`);
    check(menuReadCount === 0, `menu: no READ-usage warning in ${MENU_SECONDS} s (${menuReadCount})`);
    check(menuTooMany === 0, `menu: no "WebGL: too many errors" (${menuTooMany})`);
    check(page.log.exceptions.length === 0, `no uncaught exception ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
    check(page.log.gl.length === 0, `no GL error ${JSON.stringify(page.log.gl.slice(0, 2))}`);
    await page.shot(process.argv[4] || '/tmp/kn-readusage-menu.png');
  } finally {
    page.close();
  }

  // in game: a fresh page on the Kanji map, into the game, then a walk for GAME_SECONDS
  const game = await openPage(env.BASE + '/?offline=1&map=kanji');
  try {
    let inGame = false;
    const clickPlay = async () => {
      const r = await game.cdp.eval("(() => { const e = document.getElementById('instructions'); const b = e && e.getBoundingClientRect(); return b && /CLICK TO PLAY/.test(e.textContent) ? [b.x + b.width / 2, b.y + b.height / 2] : null; })()").catch(() => null);
      if (r) await game.cdp.click(r[0], r[1]).catch(() => {});
    };
    const isInGame = () => game.cdp.eval("(() => { const h = document.getElementById('inGameUI'); return !!(h && getComputedStyle(h).display !== 'none'); })()").catch(() => false);
    for (let i = 0; i < 120 && !inGame; i++) {
      await sleep(500);
      if (await isInGame()) inGame = true; else await clickPlay();
    }
    check(inGame, 'in game on kanji (inGameUI shown)');
    const keys = [['w', 'KeyW', 87], ['a', 'KeyA', 65], ['s', 'KeyS', 83], ['d', 'KeyD', 68]];
    const snap = game.mark();
    const first = game.log.all.length;
    const deadline = Date.now() + GAME_SECONDS * 1000;
    let round = 0;
    while (Date.now() < deadline) {
      const [k, code, kc] = keys[round % keys.length];
      await game.cdp.keyDown(k, code, kc).catch(() => {});
      await game.cdp.mouse('mouseMoved', 640 + ((round * 97) % 200) - 100, 360, 'none').catch(() => {});
      await game.cdp.mouse('mousePressed', 640, 360, 'left').catch(() => {});
      await sleep(700);
      await game.cdp.mouse('mouseReleased', 640, 360, 'left').catch(() => {});
      await game.cdp.keyUp(k, code, kc).catch(() => {});
      if (round % 4 === 3) {
        // the pause menu shows the class preview over the paused world, then back in game
        await game.cdp.key('Escape', 'Escape', 27).catch(() => {});
        await sleep(1500);
        await clickPlay();
        await sleep(500);
      }
      round++;
    }
    const since = game.log.all.slice(first);
    const gameRead = count(since, READ_USAGE);
    const gameTooMany = count(since, TOO_MANY);
    console.log(`game ${GAME_SECONDS} s: READ-usage ${gameRead}, too many errors ${gameTooMany}`);
    check(gameRead === 0, `game: no READ-usage warning in ${GAME_SECONDS} s (${gameRead})`);
    check(gameTooMany === 0, `game: no "WebGL: too many errors" (${gameTooMany})`);
    check(game.since(snap).exceptions.length === 0, 'game: no uncaught exception');
    check(game.log.gl.length === 0, `game: no GL error ${JSON.stringify(game.log.gl.slice(0, 2))}`);
  } finally {
    game.close();
  }

  if (failures.length) { console.log(`web readusage e2e: ${failures.length} failed`); process.exit(1); }
  console.log('web readusage e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
