'use strict';
// Web e2e: the Servers window (window 2) lists the server finder through the page's own bridge.
// The public finder (matchmaker.krunk.io) answers only krunk.io's origin, so a web build hosted
// elsewhere names its bridge's finder (?finder=/servers); net-bridge serves GET /servers from the
// brain (brainProxy.ts PROXIED_PATHS). Fails without that route (the window shows
// "Server finder unavailable" and the browser logs a CORS or 404 error).
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/servers-e2e.js [screenshot.png]
const { openPage, sleep, stackEnv } = require('./session.js');

const SHOW_SERVERS = `window.Module.ccall('kn_ui_event','number',['string','string','number','string','number','number','number','number','number','number'],['click|showWindow', '[2,0]', 0, '', 0, 0, 0, 0, 0, 0])`;

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-servers.png';
  const page = await openPage(env.BASE + '/?offline=1&finder=/servers');
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  try {
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders');
    await sleep(1000);
    const snap = page.mark();
    await page.cdp.eval(SHOW_SERVERS);
    // the region row starts collapsed ("0 online"): one click expands it (the same toggle a player uses)
    let expanded = false, listed = false;
    for (let i = 0; i < 30 && !listed; i++) {
      await sleep(500);
      if (!expanded && (await page.visible('[onclick*="setExpandedRegion"]'))) { expanded = await page.clickSel('[onclick*="setExpandedRegion"]'); await sleep(400); }
      listed = /web session node/.test((await page.text('#menuWindow')) || '');
    }
    check(listed, 'the Servers window lists the stack node "web session node" from the finder');
    const text = (await page.text('#menuWindow')) || '';
    check(!/Server finder unavailable/.test(text), 'the window shows no "Server finder unavailable" row');
    await page.shot(shot);
    const d = page.since(snap);
    const cors = d.errors.filter((e) => /CORS|matchmaker\.krunk\.io/.test(e));
    check(cors.length === 0, `no CORS error for the public finder ${JSON.stringify(cors.slice(0, 1)).slice(0, 160)}`);
    check(d.exceptions.length === 0, `no uncaught exception ${JSON.stringify(d.exceptions.slice(0, 2))}`);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`web-session servers: ${failures.length} failed`); process.exit(1); }
  console.log('web-session servers: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
