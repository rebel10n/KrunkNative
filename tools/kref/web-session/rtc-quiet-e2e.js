'use strict';
// Web transport fallback e2e: a local stack has no WebRTC gateway (rtc.json 404) and no WebTransport
// relay (wt.json 404). The client falls back to the bridge WebSocket, and says so with one line per
// transport at info level: no warning, no error, no diagnostic dump in the console.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/rtc-quiet-e2e.js [screenshot.png]
const { openPage, stackEnv, sleep } = require('./session.js');

const env = stackEnv();
const shot = process.argv[2] || '/tmp/kn-web-session-rtc.png';
const failures = [];
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };

(async () => {
  const page = await openPage(env.BASE + '/');
  try {
    let joined = false;
    for (let i = 0; i < 160 && !joined; i++) {
      await sleep(500);
      joined = await page.visible('#menuBtnQuickMatch') && page.log.all.some((l) => /connected to server/.test(l) || /WEB1/.test(l));
    }
    await sleep(2500);
    const all = page.log.all;
    const rtcWarn = all.filter((l) => /^warn \[rtc\]|^error \[rtc\]|^warn \[wt\]|^error \[wt\]/.test(l));
    check(rtcWarn.length === 0, `no [rtc] / [wt] warning or error ${JSON.stringify(rtcWarn.slice(0, 2)).slice(0, 200)}`);
    const why = all.filter((l) => /^log \[(rtc|wt)\] .*(no WebRTC gateway configured|no WebTransport relay).*the game uses the WebSocket/.test(l));
    check(why.length >= 2, `each transport says why in one line (${why.length} lines)`);
    check(!all.some((l) => /navigator|userAgent|Mozilla\//.test(l) && /\[rtc\]/.test(l)), 'the fallback line carries no browser user agent');
    check(page.log.exceptions.length === 0, `no uncaught exception ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
    await page.shot(shot);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`rtc-quiet e2e: ${failures.length} failed`); process.exit(1); }
  console.log('rtc-quiet e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
