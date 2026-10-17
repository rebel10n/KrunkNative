// Web first-run choice (client/src/ui/first_run_ui.c, client/src/ui/seek_prefs.c) in headless Chromium:
// the popup shows once per browser profile, each option is stored, and the same profile never sees it
// again. The bridge is not a relay here (the page's origin answers /healthz with 404), so the Krunker
// option says krunker.io is not reachable and KrunkNative still works.
//   node tools/kref/first-run-web-e2e.js <cdpPort> <pageUrl> fresh|again     exit 0 = all passed
// fresh: a profile with no choice yet (the popup shows once; the Krunker option is chosen, its note
// shown, the server list opens, the Server Type is saved as krunker.io servers);
// again: the same profile on a later load (no popup).
'use strict';
const path = require('path');
const http = require('http');
const WebSocket = require('/home/user/src/KrunkNative/net-bridge/node_modules/ws');
const { CDP, sleep } = require(path.join(__dirname, 'cdp.js'));
const CDPP = +process.argv[2], URL = process.argv[3], MODE = process.argv[4] || 'fresh';
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  -- ' + String(detail).slice(0, 300) : ''}`); };

// Browser.close: Chromium writes the profile's localStorage on a clean shutdown; a kill can lose the
// choice, and the next launch on the same profile would then show the popup again
async function closeBrowser(port) {
  try {
    const v = await new Promise((res, rej) => http.get(`http://127.0.0.1:${port}/json/version`, (r) => {
      let d = ''; r.on('data', (x) => d += x); r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } });
    }).on('error', rej));
    const ws = new WebSocket(v.webSocketDebuggerUrl);
    await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
    ws.send(JSON.stringify({ id: 1, method: 'Browser.close' }));
    await sleep(1500);
    ws.close();
  } catch (e) { /* already gone */ }
}

(async () => {
  const c = await CDP.connect(CDPP);
  await c.send('Runtime.enable'); await c.send('Page.enable');
  await c.setViewport(1280, 720);
  await c.send('Page.navigate', { url: URL });
  const count = () => c.eval(`document.querySelectorAll('#knFirstRun').length`).catch(() => -1);
  if (MODE === 'fresh') {
    const up = await c.waitFor(`document.querySelectorAll('#knFirstRun').length > 0`, 120000).then(() => true, () => false);
    check('fresh profile: the first-run popup shows', up);
    await sleep(1500);
    check('fresh profile: the popup shows once (one #knFirstRun)', (await count()) === 1, await count());
    const note = await c.eval(`(document.getElementById('knFirstRunOpt0') || {}).textContent || ''`).catch(() => '');
    check('Krunker option says krunker.io is not reachable (relay off)', /not reachable/.test(note), note);
    check('Box Office option is there', await c.eval(`!!document.getElementById('knFirstRunOpt1')`));
    check('Private server option is there', await c.eval(`!!document.getElementById('knFirstRunOpt2')`));
    await c.eval(`document.getElementById('knFirstRunOpt0').click()`);
    await sleep(1500);
    check('the choice closes the popup', (await count()) === 0, await count());
    const stored = await c.eval(`localStorage.getItem('knFirstRun')`);
    check('knFirstRun is stored as krunker', stored === 'krunker', stored);
    const seek = await c.eval(`(() => { try { return JSON.parse(localStorage.getItem('knSettings') || '{}').knSeekServers; } catch (e) { return 'err'; } })()`);
    check('Server Type is saved as krunker.io servers (official)', seek === 'official', seek);
  } else {
    await sleep(8000);
    check('same profile: no popup on a later load', (await count()) === 0, await count());
    const stored = await c.eval(`localStorage.getItem('knFirstRun')`);
    check('knFirstRun is still stored', stored === 'krunker', stored);
  }
  c.close();
  await closeBrowser(CDPP);
  const failed = results.filter((r) => !r).length;
  console.log(`first-run-web-e2e ${MODE}: ${results.length - failed}/${results.length} passed`);
  process.exit(failed ? 1 : 0);
})().catch(async (e) => { console.error('first-run-web-e2e:', e.message); await closeBrowser(CDPP); process.exit(1); });
