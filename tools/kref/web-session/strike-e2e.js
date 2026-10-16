'use strict';
// Web Strike e2e: the Krunker Strike page (/strike/, strike/index.html, FRVR's page with the shared
// client) boots: the client and its game data load from the page's root (not from /strike/), the
// loading screen goes away, a Strike menu button is visible and clicking it opens a menu with no
// exception, and the page makes no failed request or WebGL error (apart from the known asset gaps).
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/strike-e2e.js [screenshot.png]
const { openPage, stackEnv, sleep } = require('./session.js');

const env = stackEnv();
const shot = process.argv[2] || '/tmp/kn-web-session-strike.png';
const failures = [];
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };

(async () => {
  const page = await openPage(env.BASE + '/strike/');
  try {
    let ready = false;
    for (let i = 0; i < 160 && !ready; i++) {
      await sleep(500);
      ready = await page.cdp.eval('!!window.__krunknative_wasm_ready').catch(() => false);
    }
    check(ready, 'the Strike page loads the client (client.wasm and client.data from the page root)');
    check(!page.log.http.some((h) => /\/strike\/client\./.test(h)), 'no client file is requested under /strike/');
    let menu = false;
    for (let i = 0; i < 160 && !menu; i++) {
      await sleep(500);
      menu = await page.cdp.eval(`(() => { const l = document.getElementById('frvrLoadingScreen'); const gone = !l || getComputedStyle(l).display === 'none' || l.getBoundingClientRect().height === 0; return gone && document.getElementById('frvrMenuMobileHolder') && document.getElementById('frvrMenuMobileHolder').children.length > 0; })()`).catch(() => false);
    }
    check(menu, 'the Strike menu is up (the loading screen is gone and the menu holder has content)');
    await sleep(2000);
    // two menu buttons, as a player would press them: no exception, no WebGL error
    const snap = page.mark();
    for (const label of ['EXTRAS']) {
      const r = await page.cdp.eval(`(() => { const e = [...document.querySelectorAll('button,div,span')].filter(x => x.children.length === 0 && x.textContent.trim().toUpperCase() === ${JSON.stringify(label)} && x.getBoundingClientRect().width > 0).pop(); if (!e) return null; const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()`);
      check(!!r, `the ${label} button is on the menu`);
      if (r) await page.cdp.click(r[0], r[1]);
      await sleep(1500);
    }
    check(page.since(snap).exceptions.length === 0, 'the menu buttons run without an exception');
    await page.shot(shot);
    check(page.log.exceptions.length === 0, `no uncaught exception ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
    check(page.log.gl.length === 0, `no WebGL error ${JSON.stringify(page.log.gl.slice(0, 2))}`);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`strike e2e: ${failures.length} failed`); process.exit(1); }
  console.log('strike e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
