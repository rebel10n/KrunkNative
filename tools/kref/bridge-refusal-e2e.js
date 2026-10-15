// Web bridge admission e2e (driven by tools/kref/bridge-refusal-e2e.sh): the page served by
// `net-bridge --web` with KRUNK_NATIVE_TARGETS set to the one game server (a deployed bridge's
// allowlist, no loopback catch-all).
//   1. allowed: ?ws=ws://<bridge>/native?target=127.0.0.1:<game> connects and the game comes up.
//   2. allowed as `localhost:<game>` (the bridge maps it to the IPv4 loopback the server binds).
//   3. refused: target=127.0.0.1:<bridge port> (the page's own web server, the target in the
//      user's report) is not on the allowlist. The bridge completes the handshake and closes it
//      with 1008 "target not allowed" (webBridge.ts admit), so the page logs the code and the
//      reason, shows the Lost Connection box naming it, and does not retry (a final refusal).
//   node tools/kref/bridge-refusal-e2e.js <cdpPort> <bridgePort> <gamePort>      exit 0 = all passed
'use strict';
const path = require('path');
const { CDP, sleep } = require(path.join(__dirname, 'cdp.js'));
const [CDPP, HTTP, GAME] = process.argv.slice(2).map(Number);
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  -- ' + detail : ''}`); };

(async () => {
  const c = await CDP.connect(CDPP);
  const logs = [];
  c.on(m => { if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map(a => a.value !== undefined ? String(a.value) : a.description).join(' ')); });
  await c.send('Runtime.enable'); await c.send('Page.enable');
  await c.setViewport(640, 360);
  const waitLog = async (re, t) => { const t0 = Date.now(); while (Date.now() - t0 < t) { const l = logs.find(x => re.test(x)); if (l) return l; await sleep(200); } return null; };
  const open = async (target) => {
    logs.length = 0;
    await c.send('Page.navigate', { url: `http://127.0.0.1:${HTTP}/?mode=offline&ws=${encodeURIComponent(`ws://127.0.0.1:${HTTP}/native?target=${target}`)}` });
  };
  // the Lost Connection box (.instructionsHeader) and its text (#instructionHolder: header, message, tabs)
  const box = () => c.eval(`(()=>{const h=document.querySelector('.instructionsHeader');const e=document.getElementById('instructionHolder');
    return h ? (e ? e.textContent.replace(/\\s+/g, ' ') : '?') : null})()`).catch(() => null);

  await open(`127.0.0.1:${GAME}`);
  check('allowed target: the game session comes up', await waitLog(new RegExp(`connected to server`), 90000), logs.filter(l => /net\]|Could not/.test(l)).slice(0, 3).join(' | '));

  await open(`localhost:${GAME}`);
  check('allowed target as localhost: the game session comes up', await waitLog(/connected to server/, 90000), logs.filter(l => /net\]|Could not/.test(l)).slice(0, 3).join(' | '));

  await open(`127.0.0.1:${HTTP}`);
  const closed = await waitLog(/websocket closed \(code/, 60000);
  check('refused target: the page sees close 1008 with the reason', closed && /code 1008: target not allowed/.test(closed), closed || '(no close)');
  const failed = await waitLog(/connection gateway refused/, 5000);
  check('refused target: the failure names the refusal', failed && /target not allowed/.test(failed), failed || logs.filter(l => /Could not/.test(l)).join(' | '));
  let text = null;
  for (let i = 0; i < 40 && !text; i++) { text = await box(); if (!text) await sleep(250); }
  check('refused target: the Lost Connection box shows the reason', text && /Lost Connection/.test(text) && /refused this server: target not allowed/.test(text), String(text).slice(0, 160));
  check('refused target: the box offers Retry', text && /Retry/.test(text));
  await sleep(6000); // past the first two backoff steps (2 s, 4 s)
  const retried = logs.filter(l => /\[net\] retrying|connecting to ws/.test(l));
  check('refused target: no retry (the refusal is final)', retried.length <= 1, retried.join(' | '));
  check('refused target: no give-up loop', !logs.some(l => /gave up after/.test(l)));

  console.log(`${results.filter(Boolean).length}/${results.length} passed`);
  process.exit(results.length && results.every(Boolean) ? 0 : 1);
})().catch(e => { console.log('FAIL  ' + e.message); process.exit(1); });
