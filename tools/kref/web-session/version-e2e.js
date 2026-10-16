'use strict';
// Web version e2e: the page's loader asks for assets/version.txt (the asset content version). A web
// build with no deploy file must still answer it (the bridge computes one for a dev stack), so the
// page loads without a 404 for it, and the asset URLs carry the version.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/version-e2e.js [screenshot.png]
const { openPage, stackEnv, sleep } = require('./session.js');

const env = stackEnv();
const shot = process.argv[2] || '/tmp/kn-web-session-version.png';
const failures = [];
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };

(async () => {
  const page = await openPage(env.BASE + '/?offline=1');
  try {
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders');
    const body = await page.cdp.eval(`fetch('assets/version.txt', { cache: 'no-store' }).then(async (r) => r.status + ' ' + (await r.text()))`);
    check(/^200 [0-9a-f]{16}\s*$/.test(body), `assets/version.txt answers with a version (${JSON.stringify(body)})`);
    check(!page.log.http.some((h) => /assets\/version\.txt/.test(h)), 'the page itself does not get a 404 for assets/version.txt');
    await page.shot(shot);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`version e2e: ${failures.length} failed`); process.exit(1); }
  console.log('version e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
