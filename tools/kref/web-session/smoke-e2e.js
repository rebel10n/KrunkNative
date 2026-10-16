'use strict';
// Web smoke e2e: the web build served by net-bridge --web opens the main menu in headless Chromium,
// the menu's buttons open their windows, and the page throws no exception and makes no WebGL error.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/smoke-e2e.js [screenshot.png]
const { openPage, sleep } = require('./session.js');
const { stackEnv } = require('./session.js');

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-smoke.png';
  const page = await openPage(env.BASE + '/?offline=1');
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  try {
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders (Quick Match button visible)');
    for (const id of ['menuBtnJoin', 'menuBtnCustomGames']) {
      const snap = page.mark();
      const hit = await page.clickId(id);
      await sleep(1500);
      check(hit, `${id} is clickable`);
      await page.clickSel('#windowCloser');
      await sleep(400);
      const d = page.since(snap);
      check(d.exceptions.length === 0, `${id}: no uncaught exception ${JSON.stringify(d.exceptions.slice(0, 2))}`);
      await page.clickSel('#popupBack');
      await sleep(300);
    }
    await sleep(2000);
    await page.shot(shot);
    check(page.log.exceptions.length === 0, `no uncaught exception in the session ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
    check(page.log.gl.length === 0, `no WebGL error ${JSON.stringify(page.log.gl.slice(0, 2))}`);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`web-session smoke: ${failures.length} failed`); process.exit(1); }
  console.log('web-session smoke: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
