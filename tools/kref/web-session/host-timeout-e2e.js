'use strict';
// Web e2e: Start Game when the brain never answers its /mm/host-game. CDP's Fetch domain holds that request
// (it is paused and never continued: a stalled brain). The web client's brain timeout (backend_web.c,
// BACKEND_HTTP_TIMEOUT_MS, 8 s, as the native socket's) then ends the request with status 0, and the Host window
// shows v10's failure text, "Please try again.", where "Creating Game..." would otherwise stay until a reload.
// The brain is asked once (a /mm/host-game is never retried).
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/host-timeout-e2e.js [screenshot.png]
const { openPage, sleep, stackEnv } = require('./session.js');

async function waitFor(fn, ms, step = 250) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const v = await fn();
    if (v) return v;
    await sleep(step);
  }
  return null;
}

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-host-timeout.png';
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  const page = await openPage(env.BASE + '/');
  try {
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders (the page finds the stack brain: its /mm/status answers)');
    await sleep(2000); // the brain probe (backend_probe_start) answers and the menu takes clicks

    // the stalled brain: /mm/host-game is paused by CDP and never continued
    const held = [];
    page.cdp.on((m) => { if (m.method === 'Fetch.requestPaused') held.push(m.params.request.url); });
    await page.cdp.send('Fetch.enable', { patterns: [{ urlPattern: '*/mm/host-game*' }] });

    // Host Game (menuBtnHost) -> the Custom Game card (v10's openHostWindow(false, 0)) opens the Host window
    const card = '[onclick*="openHostWindow(false, 0)"]';
    await page.clickId('menuBtnHost');
    const chooser = await waitFor(() => page.visible(card), 15000);
    check(!!chooser, 'Host Game opens the game type chooser');
    await page.clickSel(card);
    const hasStart = await waitFor(() => page.visible('#startServBtn'), 15000);
    check(!!hasStart, 'the Custom Game card opens the Host window (Start Game is shown)');
    await sleep(500);
    // a map to host: the Select tab's first Krunker map card (its checkbox is gameMap<index>, host_flow_ui.c)
    const mapCard = 'img.hostMapImg';
    const mapShown = await waitFor(() => page.visible(mapCard), 10000);
    check(!!mapShown, 'the Select tab lists Krunker maps');
    if (mapShown) await page.clickSel(mapCard);
    const picked = await page.cdp.eval(`(() => { const b = document.querySelector('input[id^="gameMap"]'); return !!(b && b.checked); })()`);
    check(!!picked, 'a map is picked for the game');

    const snap = page.mark();
    const t0 = Date.now();
    await page.clickId('startServBtn');
    const spinner = await waitFor(async () => (await page.text('#hostGameMsg')) === 'Creating Game...', 10000, 100);
    check(!!spinner, 'Start Game shows Creating Game... while the request is out');
    const asked = await waitFor(() => held.some((u) => u.includes('/mm/host-game')), 10000, 100);
    check(!!asked, 'the request reached the brain (it is held there)');

    // the brain's timeout ends it: v10's failure text in the Host window's error line
    const failed = await waitFor(() => page.visible('#hostGameMsg span.error'), 30000, 200);
    const waited = Date.now() - t0;
    check(!!failed, `the Host window gets an error line after the brain timeout (${waited} ms)`);
    const msg = (await page.text('#hostGameMsg')) || '';
    check(msg === 'Please try again.', `the error line is v10's failure text ("${msg}")`);
    check(!/Creating Game/.test(msg), 'the spinner does not outlive the request');
    check(waited >= 7000 && waited < 25000, `the answer came at the brain timeout, not at once or never (${waited} ms)`);
    check(held.filter((u) => u.includes('/mm/host-game')).length === 1, 'the brain was asked once (a host request is never retried)');

    const d = page.since(snap);
    check(d.exceptions.length === 0, `no uncaught exception ${JSON.stringify(d.exceptions.slice(0, 2))}`);
    // the failure is logged like native's, with the path only (the request's query carries the session token)
    const logged = page.log.all.filter((l) => l.includes('/mm/host-game') && l.includes('failed'));
    check(logged.length === 1 && /GET \S*\/mm\/host-game failed: no answer within 8 s \(timed out\)/.test(logged[0]) && !/token=|config=/.test(logged[0]),
      `the timeout is logged once, without the query (${JSON.stringify(logged.slice(0, 1)).slice(0, 200)})`);
    await page.shot(shot);
    console.log('screenshot ' + shot);
  } finally {
    page.close();
  }
  if (failures.length) {
    console.log(`host-timeout-e2e: ${failures.length} failed`);
    process.exit(1);
  }
  console.log('host-timeout-e2e: all checks passed');
})().catch((e) => { console.error(e); process.exit(1); });
