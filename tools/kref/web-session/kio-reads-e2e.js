'use strict';
// Web read e2e: the krunker.io reads of the web build reach their hosts from the page's own origin. Each read is
// the request backend_web_fetch sends (fetch, GET, Accept: application/json, no credentials, no other header),
// made in the page by the browser itself against the real hosts. It must resolve with status 200 and a JSON
// body: a read the browser blocks (CORS, or any network refusal) rejects with status 0, which is the silent
// empty panel. Every request to api.krunker.io / social.krunker.io is recorded too, with its response's
// Access-Control-Allow-Origin and any failure cause (CORS, or the network). A guest page (the stack's brain as the backend) must ask neither host on load: those reads
// run only with a krunker.io account or an official game (servers_ui.c, social_ui.c, v10_menu.c).
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/kio-reads-e2e.js [screenshot.png]
// Through web-session-e2e.sh (make web-session-e2e TESTS=tools/kref/web-session/kio-reads-e2e.js). Needs network
// to the Krunker hosts. A sandbox that reaches them only through a local CONNECT proxy points KN_WEB_CHROME at a
// Chromium wrapper that adds --proxy-server=<proxy> --proxy-bypass-list=127.0.0.1,localhost; a direct connection
// there gets Cloudflare's 403 without Access-Control-Allow-Origin, and every read fails with status 0.
const { openPage, stackEnv, sleep } = require('./session.js');

const HOST = /^https:\/\/(api\.krunker\.io|social\.krunker\.io)\//;
// The reads as the page builds them: krunker_kio_routes.c's URLs with the tab-0 list, the search, the hub's mods
// search (social_ui.c), the streams (v10_menu.c) and the footer's count (servers_ui.c).
const READS = [
  ['Games lists (maps/list)', 'https://api.krunker.io/webhooks/general/maps/list?index=0&pos=0'],
  ['Games search (maps/search)', 'https://api.krunker.io/webhooks/general/maps/search?val=kanji&cat=-1'],
  ['Hub mods (mods/search)', 'https://api.krunker.io/webhooks/general/mods/search?val=gun&type=mod'],
  ['Live Streams (streams/top)', 'https://api.krunker.io/streams/top?cb=2'],
  ['Footer count (player-count)', 'https://social.krunker.io/player-count'],
];

// backend_web_fetch's request, run in the page: the status and the CORS verdict the browser gave
const probe = (url) => `(async () => {
  try {
    const r = await fetch(${JSON.stringify(url)}, { method: 'GET', headers: { 'Accept': 'application/json' } });
    const t = await r.text();
    let json = true; try { JSON.parse(t); } catch (e) { json = false; }
    return { status: r.status, acao: r.headers.get('access-control-allow-origin'), bytes: t.length, json };
  } catch (e) { return { status: 0, error: String((e && e.message) || e) }; }
})()`;

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-kio-reads.png';
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };

  const page = await openPage(env.BASE + '/');
  try {
    // Network and console recorder for the krunker.io hosts (openPage ignores network-source console lines)
    const urlOf = new Map();
    const net = [];
    page.cdp.on((m) => {
      const p = m.params || {};
      if (m.method === 'Network.requestWillBeSent' && HOST.test(p.request.url)) {
        urlOf.set(p.requestId, p.request.url);
        net.push({ ev: 'sent', url: p.request.url, headers: Object.keys(p.request.headers || {}).sort().join(',') });
      } else if (m.method === 'Network.responseReceived' && HOST.test(p.response.url)) {
        const h = p.response.headers || {};
        const acao = h['access-control-allow-origin'] || h['Access-Control-Allow-Origin'] || null;
        net.push({ ev: 'response', url: p.response.url, status: p.response.status, acao });
      } else if (m.method === 'Network.loadingFailed' && urlOf.has(p.requestId)) {
        const cause = [p.errorText, p.blockedReason && 'blocked:' + p.blockedReason, p.corsErrorStatus && 'cors:' + p.corsErrorStatus.corsError].filter(Boolean).join(' ');
        net.push({ ev: 'failed', url: urlOf.get(p.requestId), cause });
      }
    });

    // 1. a guest page on load: no krunker.io host is asked (the brain is the backend). openPage's navigation
    // ran before the recorder: wait for its menu, then navigate again so the recorder sees the whole load.
    const menuUp = async () => { for (let i = 0; i < 160; i++) { await sleep(500); if (await page.visible('#menuBtnQuickMatch')) return true; } return false; };
    check(await menuUp(), 'the menu renders on the stack page');
    await page.cdp.send('Page.navigate', { url: env.BASE + '/' });
    await sleep(1500);
    const menu = await menuUp();
    check(menu, 'the menu renders again on the reloaded page');
    await sleep(8000);
    const onLoad = net.filter((x) => x.ev === 'sent');
    check(onLoad.length === 0, `a guest page asks no krunker.io host on load (${onLoad.length} requests: ${onLoad.slice(0, 3).map((x) => x.url).join(' ')})`);

    // 2. the page's own origin and the five reads, each as backend_web_fetch sends it. A read the browser blocks
    // (CORS, or no answer) rejects: status 0. A resolved read passed CORS; a script cannot see ACAO on a cross-origin
    // response (only the CORS-safelisted headers), so ACAO is read from the network record in step 3.
    const origin = await page.cdp.eval('location.origin');
    console.log(`page origin ${origin}`);
    const results = [];
    for (const [label, url] of READS) {
      const r = await page.cdp.eval(probe(url));
      results.push({ label, url, ...r });
      console.log(`${(r.status === 200 && r.json ? 'read' : 'NOT READ').padEnd(8)} ${label}: status ${r.status}${r.bytes === undefined ? '' : ', ' + r.bytes + ' bytes'}${r.error ? ', ' + r.error : ''}`);
      check(r.status === 200 && r.json, `${label} is read from the page (status ${r.status}, JSON ${r.json === true})`);
    }
    await sleep(2000);

    // 3. the browser's own network record per read: request sent, answer (status, ACAO), a failure with its CORS cause
    for (const [label, url] of READS) {
      const sent = net.some((x) => x.ev === 'sent' && x.url === url);
      const resp = net.find((x) => x.ev === 'response' && x.url === url);
      const fail = net.find((x) => x.ev === 'failed' && x.url === url);
      console.log(`${label}: request ${sent ? 'sent' : 'NOT sent'}, response ${resp ? resp.status + ' ACAO ' + (resp.acao || 'none') : 'none'}${fail ? ', failed: ' + fail.cause : ''}`);
      check(sent && !!resp && resp.status === 200 && !!resp.acao && !fail, `${label}: sent, answered 200 with Access-Control-Allow-Origin, not failed`);
    }
    const sentAll = net.filter((x) => x.ev === 'sent');
    console.log(`krunker.io requests sent: ${sentAll.length}; request header names: ${[...new Set(sentAll.map((x) => x.headers))].join(' | ') || 'none'}`);

    await page.shot(shot);
    console.log(JSON.stringify(results.map((r) => ({ label: r.label, status: r.status, json: r.json || false, error: r.error || null }))));
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`web kio reads e2e: ${failures.length} failed`); process.exit(1); }
  console.log('web kio reads e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
