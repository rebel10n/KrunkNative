// The social WebSocket on local web hosting (R38: "ws://127.0.0.1:8080/social/ws?enc=json always
// fails"), driven by tools/kref/social-ws-e2e.sh. The page probes GET /mm/status before it opens
// /social/ws (client/src/net/backend_common.c), so:
//   static   `make web-serve` (a plain static server, no brain): no /social/ws attempt at all, and the
//            console says why ("no account server ... social features are off").
//   nobrain  `make bridge` with no kn_brain behind it: the same (no attempt, the same message).
//   brain    `make bridge --brain <kn_brain>`: /social/ws upgrades (101) through the bridge's proxy
//            and the page reports the brain found.
//   node tools/kref/social-ws-e2e.js <cdpPort> <case> <pageUrl>      exit 0 = all passed
'use strict';
const path = require('path');
const { CDP, sleep } = require(path.join(__dirname, 'cdp.js'));
const [CDPP, CASE, PAGE] = [Number(process.argv[2]), process.argv[3], process.argv[4]];
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${CASE}: ${name}${detail ? '  -- ' + String(detail).slice(0, 300) : ''}`); };

(async () => {
  const c = await CDP.connect(CDPP);
  const logs = [], sockets = new Map();
  c.on(m => {
    if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map(a => a.value !== undefined ? String(a.value) : a.description).join(' '));
    if (m.method === 'Network.webSocketCreated' && /\/social\/ws/.test(m.params.url)) sockets.set(m.params.requestId, { url: m.params.url, status: 0, error: '' });
    if (m.method === 'Network.webSocketHandshakeResponseReceived' && sockets.has(m.params.requestId)) sockets.get(m.params.requestId).status = m.params.response.status;
    if (m.method === 'Network.webSocketFrameError' && sockets.has(m.params.requestId)) sockets.get(m.params.requestId).error = m.params.errorMessage;
  });
  await c.send('Runtime.enable'); await c.send('Page.enable'); await c.send('Network.enable');
  await c.setViewport(640, 360);
  await c.send('Page.navigate', { url: 'about:blank' });
  await sleep(300);
  logs.length = 0;
  sockets.clear();
  await c.send('Page.navigate', { url: PAGE });
  const waitLog = async (re, t) => { const t0 = Date.now(); while (Date.now() - t0 < t) { const l = logs.find(x => re.test(x)); if (l) return l; await sleep(200); } return null; };
  const origin = new URL(PAGE).origin; // (the tab's previous page can still log a line after the navigation)
  const probe = await waitLog(new RegExp('\\[backend\\] (brain found|no account server) at ' + origin.replace(/[.]/g, '[.]') + '\\b'), 60000);
  await sleep(4000); // the social socket opens right after the probe (menu / party / friends)
  const list = [...sockets.values()];
  if (CASE === 'brain') {
    check('the probe finds the brain', probe && /brain found/.test(probe), probe);
    check('/social/ws opens through the bridge (101)', list.some(s => s.status === 101), JSON.stringify(list));
  } else {
    check('the probe finds no brain and says what that means', probe && /no account server .*social features are off/.test(probe), probe);
    check('no /social/ws attempt (nothing to fail in the console)', list.length === 0, JSON.stringify(list));
  }
  process.exit(results.length && results.every(Boolean) ? 0 : 1);
})().catch(e => { console.log(`FAIL  ${CASE}: ` + e.message); process.exit(1); });
