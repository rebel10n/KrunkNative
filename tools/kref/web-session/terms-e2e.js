'use strict';
// Web e2e for window 47 (Terms): the window must not frame frvr.com (the browser refuses that
// frame, so the window was blank on the web), must show the placeholder notice that ui/windows/47.html
// carries, and must log no console error or failed request while it opens.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/terms-e2e.js [screenshot.png]
const { openPage, sleep, stackEnv } = require('./session.js');

// The notice ui/windows/47.html shows (the text the window agreed on; checked verbatim).
const NOTICE = 'The Krunker Terms of Service are not shown here.';

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-terms.png';
  const page = await openPage(env.BASE + '/?offline=1');
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  try {
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders (Quick Match button visible)');

    // The Terms link (ui/index.html #termsInfo) opens the browser tab while window.canShowExternalLinks
    // is set, so window 47 is opened the way that link's else branch does: showWindow(47).
    // The shim's handler globals (showWindow, ...) appear once knActivate() runs, a second or two
    // after the menu shows, so wait for them before calling.
    let globalsReady = false;
    for (let i = 0; i < 60 && !globalsReady; i++) { await sleep(250); globalsReady = await page.cdp.eval("typeof showWindow === 'function'"); }
    check(globalsReady, 'the shim installs the handler globals (showWindow)');
    const snap = page.mark();
    const requested = [];
    page.cdp.on((m) => { if (m.method === 'Network.requestWillBeSent' && /frvr\.com/.test(m.params.request.url)) requested.push(m.params.request.url); });
    await page.cdp.eval('showWindow(47)');
    let open = false;
    for (let i = 0; i < 40 && !open; i++) { await sleep(250); open = await page.visible('#menuWindow'); }
    await sleep(1000);
    check(open, 'window 47 opens (#menuWindow visible)');

    const frames = await page.cdp.eval(`Array.from(document.querySelectorAll('#menuWindow iframe')).map((f) => f.getAttribute('src') || '')`);
    const frvr = (frames || []).filter((src) => /frvr\.com/.test(src));
    check(frvr.length === 0, `window 47 frames no frvr.com page ${JSON.stringify(frvr)}`);

    const body = await page.cdp.eval(`(() => { const w = document.querySelector('#menuWindow'); return w ? (w.innerText || w.textContent || '') : ''; })()`);
    check(String(body).includes(NOTICE), `window 47 body has the notice "${NOTICE}" (got ${JSON.stringify(String(body).slice(0, 120))})`);

    const links = await page.cdp.eval(`Array.from(document.querySelectorAll('#menuWindow a[href], #menuWindow [onclick]')).map((e) => (e.getAttribute('href') || '') + ' ' + (e.getAttribute('onclick') || '')).filter((s) => /frvr/.test(s))`);
    check((links || []).length === 0, `window 47 links to no frvr.com page ${JSON.stringify(links)}`);

    await sleep(1000);
    const d = page.since(snap);
    // Known lines that are not window 47's: the [ui] handler trace (web-play-console-trace, queued) and
    // the sound-asset gap (53 sound names are missing from the asset tree, web and native alike).
    const KNOWN = [/^\[ui\] handler /, /missing sound /, /failed to fetch asset assets\/sound\//];
    const errors = d.errors.filter((t) => !KNOWN.some((re) => re.test(t)));
    check(errors.length === 0, `window 47: no console error ${JSON.stringify(errors.slice(0, 3))}`);
    check(d.exceptions.length === 0, `window 47: no uncaught exception ${JSON.stringify(d.exceptions.slice(0, 2))}`);
    check(requested.length === 0, `window 47: requests no frvr.com URL ${JSON.stringify(requested.slice(0, 2))}`);
    check(d.failed.length === 0, `window 47: no failed request (a refused frame is ERR_BLOCKED_BY_RESPONSE) ${JSON.stringify(d.failed.slice(0, 3))}`);
    await page.shot(shot);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`web-session terms: ${failures.length} failed`); process.exit(1); }
  console.log('web-session terms: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
