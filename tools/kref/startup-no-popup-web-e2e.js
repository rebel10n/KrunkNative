// Web start-up without the first-run popup (client/src/ui/menu_ui.c) in headless Chromium: a fresh browser
// profile goes straight to the menu (Box Office by default), with no #knFirstRun popup and nothing stored
// for the old choice; the same profile on a later load is the same. The bridge is not a relay here (the
// page's origin answers /healthz with 404).
//   node tools/kref/startup-no-popup-web-e2e.js <cdpPort> <pageUrl> fresh|again     exit 0 = all passed
// fresh: a profile never seen before; again: the same profile on a later load.
'use strict';
const path = require('path');
const http = require('http');
const WebSocket = require('/home/user/src/KrunkNative/net-bridge/node_modules/ws');
const { CDP, sleep } = require(path.join(__dirname, 'cdp.js'));
const CDPP = +process.argv[2], URL = process.argv[3], MODE = process.argv[4] || 'fresh';
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  -- ' + String(detail).slice(0, 300) : ''}`); };

// Browser.close: Chromium writes the profile's localStorage on a clean shutdown
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
  const menuUp = await c.waitFor(`!!document.getElementById('menuBtnQuickMatch')`, 120000).then(() => true, () => false);
  check(`${MODE}: the menu is up`, menuUp);
  await sleep(3000);
  const popups = await c.eval(`document.querySelectorAll('#knFirstRun, .kn-firstrun').length`).catch(() => -1);
  check(`${MODE}: no first-run popup`, popups === 0, popups);
  const connect = await c.eval(`(() => { const e = document.getElementById('connectAddr'); return !!e && e.offsetParent !== null; })()`).catch(() => null);
  check(`${MODE}: no Connect window opens`, connect === false, connect);
  const stored = await c.eval(`localStorage.getItem('knFirstRun')`).catch(() => 'error');
  check(`${MODE}: nothing stored for the old choice`, stored === null, stored);
  c.close();
  await closeBrowser(CDPP);
  const failed = results.filter((r) => !r).length;
  console.log(`startup-no-popup-web-e2e ${MODE}: ${results.length - failed}/${results.length} passed`);
  process.exit(failed ? 1 : 0);
})().catch(async (e) => { console.error('startup-no-popup-web-e2e:', e.message); await closeBrowser(CDPP); process.exit(1); });
