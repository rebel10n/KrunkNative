// Failed WebRTC sessions print who failed and why (driven by tools/kref/rtc-fail-e2e.sh):
//   1. a gateway that refuses the target: the browser console's "[rtc] could not open" line names the
//      refusal (HTTP 403 target not allowed), the address the gateway saw (X-KN-RTC-Peer), the
//      browser's user agent, and the same text reaches the in-game console ("[net] webrtc could not open").
//   2. a gateway that ends a live session (no datagram from a paused server for its idle timeout):
//      the "[rtc] session lost" line carries the gateway's own reason (its kn-rtc-close notice) and
//      its session number, matching the gateway's log line, and the session's end names that reason.
//   node tools/kref/rtc-fail-e2e.js <cdpPort> <pageBase> <refusingGatewayUrl> <idleGatewayUrl> <serverPid>
'use strict';
const path = require('path');
const { CDP, sleep } = require(path.join(__dirname, 'cdp.js'));
const [CDPP, BASE, REFUSE, IDLE, SERVER_PID] = [Number(process.argv[2]), process.argv[3], process.argv[4], process.argv[5], Number(process.argv[6])];
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  -- ' + String(detail).slice(0, 400) : ''}`); };

(async () => {
  const c = await CDP.connect(CDPP);
  const logs = [];
  c.on(m => { if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map(a => a.value !== undefined ? String(a.value) : a.description).join(' ')); });
  await c.send('Runtime.enable'); await c.send('Page.enable');
  await c.setViewport(640, 360);
  const ua = await c.eval('navigator.userAgent');
  const waitLog = async (re, t) => { const t0 = Date.now(); while (Date.now() - t0 < t) { const l = logs.find(x => re.test(x)); if (l) return l; await sleep(200); } return null; };
  const open = async (gw) => { logs.length = 0; await c.send('Page.navigate', { url: `${BASE}&rtc=${encodeURIComponent(gw)}` }); };

  await open(REFUSE);
  const refused = await waitLog(/^\[rtc\] could not open/, 90000);
  check('refused: the console says why', refused && /the gateway refused \(HTTP 403 target not allowed\)/.test(refused), refused);
  check('refused: the console names the address the gateway saw', refused && /session -; address 127\.0\.0\.1;/.test(refused), refused);
  check('refused: the console names the browser (user agent)', refused && refused.includes(ua), ua);
  const note = await waitLog(/^\[net\] webrtc could not open: .*target not allowed.*address 127\.0\.0\.1/, 5000);
  check('refused: the in-game console gets the same line', note, logs.filter(l => /^\[net\]/.test(l)).slice(0, 4).join(' | '));

  await open(IDLE);
  const up = await waitLog(/webrtc session established/, 90000);
  check('idle: the session comes up over WebRTC', up, logs.filter(l => /\[rtc\]|\[net\]/.test(l)).slice(0, 4).join(' | '));
  process.kill(SERVER_PID, 'SIGSTOP'); // the game server goes silent: the gateway's idle timeout ends the session
  let lost;
  try { lost = await waitLog(/^\[rtc\] session lost/, 30000); } finally { process.kill(SERVER_PID, 'SIGCONT'); }
  check('idle: the console carries the gateway\'s reason', lost && /the gateway said: no datagram from 127\.0\.0\.1:\d+ for 3s/.test(lost), lost);
  check('idle: the console names the gateway session and address', lost && /session #\d+; address 127\.0\.0\.1;/.test(lost), lost);
  check('idle: the console names the browser', lost && lost.includes(ua));
  const ended = await waitLog(/^disconnected from server: /, 5000); // what the session end shows (net_wt.c wt_explain)
  check('idle: the session end names the gateway\'s reason', ended && /the connection gateway closed the connection: no datagram from 127\.0\.0\.1:\d+ for 3s \(WebRTC\)/.test(ended), ended);

  console.log(`${results.filter(Boolean).length}/${results.length} passed`);
  process.exit(results.length && results.every(Boolean) ? 0 : 1);
})().catch(e => { console.log('FAIL  ' + e.message); process.exit(1); });
