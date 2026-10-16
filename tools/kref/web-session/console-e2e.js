'use strict';
// Web console e2e: the client's own [ui] handler / [ui] listener traces are a log, not errors. On the
// web they must reach the browser console as log lines (stdout), not as console.error (stderr), so a
// session that presses menu buttons shows no console error from them, and the trace is still there.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/console-e2e.js [screenshot.png]
const { openPage, stackEnv, sleep } = require('./session.js');

const env = stackEnv();
const shot = process.argv[2] || '/tmp/kn-web-session-console.png';
const failures = [];
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };

(async () => {
  const page = await openPage(env.BASE + '/?offline=1');
  try {
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders');
    for (const id of ['menuBtnJoin', 'menuBtnCustomGames']) {
      await page.clickId(id);
      await sleep(1000);
      await page.clickSel('#windowCloser');
      await sleep(400);
      await page.clickSel('#popupBack');
      await sleep(300);
    }
    await sleep(1500);
    await page.shot(shot);
    const traces = page.log.all.filter((l) => /^log \[ui\] (handler|listener) /.test(l));
    check(traces.length > 0, `the handler / listener traces are logged (${traces.length} lines)`);
    const uiErrors = page.log.errors.filter((e) => /^\[ui\] /.test(e));
    check(uiErrors.length === 0, `no [ui] trace is a console error ${JSON.stringify(uiErrors.slice(0, 2))}`);
    check(page.log.exceptions.length === 0, `no uncaught exception ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`console e2e: ${failures.length} failed`); process.exit(1); }
  console.log('console e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
