// Web UDP transport e2e (doc/transport.md): opens the web client in headless Chromium (already
// started with --remote-debugging-port), presses Play, and reports from the page console whether
// the session came up over the expected knet transport (webtransport: wt-e2e.sh, webrtc:
// rtc-e2e.sh), plus pings and game packets received.
//   node tools/kref/wt-e2e.js <cdp port> <page url> <seconds> [webtransport|webrtc]
'use strict';
const { CDP, sleep } = require('./cdp.js');

(async () => {
  const [port, url, seconds] = [Number(process.argv[2]), process.argv[3], Number(process.argv[4] || 20)];
  const transport = process.argv[5] || 'webtransport';
  const cdp = await CDP.connect(port);
  const lines = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') lines.push(m.params.args.map(a => a.value !== undefined ? a.value : a.description).join(' '));
    if (m.method === 'Runtime.exceptionThrown') lines.push('EXCEPTION ' + JSON.stringify(m.params.exceptionDetails.text));
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  // KN_E2E_ORIGIN: present the page as that origin to a deployed gateway's CORS / origin check
  // (Chromium started with --disable-web-security; tests against kru from a local page)
  if (process.env.KN_E2E_ORIGIN) {
    await cdp.send('Network.enable');
    await cdp.send('Network.setExtraHTTPHeaders', { headers: { Origin: process.env.KN_E2E_ORIGIN } });
  }
  await cdp.send('Page.navigate', { url });
  const deadline = Date.now() + seconds * 1000;
  let played = false;
  const rtts = [];
  while (Date.now() < deadline) {
    await sleep(1000);
    const st = await cdp.eval("typeof _kn_wt_stats === 'function' ? UTF8ToString(_kn_wt_stats()) : null").catch(() => null);
    try { const j = JSON.parse(st); if (j && j.up && j.rtt >= 0) rtts.push(j.rtt); } catch (e) {}
    if (!played && lines.some(l => /session established|connected to server/.test(l))) {
      // click to play once the menu says so (menu_ui.c can_play: connected + ready; the
      // headless page can take a while to load), until the in-game HUD shows
      const state = await cdp.eval("(() => { const h = document.getElementById('inGameUI'); const i = document.getElementById('instructions'); return (h && getComputedStyle(h).display !== 'none' ? 'in-game' : '') + '|' + (i ? i.textContent : ''); })()").catch(() => '');
      if (/^in-game/.test(state)) {
        played = true;
        lines.push('[e2e] in game');
        // play a little: walk forward and shoot (trusted input, like a user)
        await cdp.keyDown('w', 'KeyW', 87).catch(() => {});
        await cdp.mouse('mousePressed', 640, 360, 'left').catch(() => {});
        await sleep(1500);
        await cdp.mouse('mouseReleased', 640, 360, 'left').catch(() => {});
        await cdp.keyUp('w', 'KeyW', 87).catch(() => {});
      } else if (/CLICK TO PLAY/.test(state)) {
        // click to play (ui_handlers.c: mousedown on #instructionHolder) as a trusted click on the
        // "CLICK TO PLAY" line: entering the game waits for the pointer lock, which needs a user gesture
        const r = await cdp.eval("(() => { const e = document.getElementById('instructions'); const b = e && e.getBoundingClientRect(); return b ? [b.x + b.width / 2, b.y + b.height / 2] : null; })()").catch(() => null);
        if (r) await cdp.click(r[0], r[1]).catch(() => {});
      }
    }
  }
  const count = (re) => lines.filter(l => re.test(l)).length;
  const report = {
    transport,
    session: count(new RegExp(transport + ' session established')),
    webtransport: count(/webtransport session established/),
    websocketFallback: count(/connecting to ws/),
    transportFailures: count(/unavailable \(.*\), trying the next transport/),
    connected: count(/connected to server/),
    downloadedMap: count(/downloaded map/),
    inGame: count(/\[e2e\] in game/),
  };
  // the HUD's ping (Krunker's #pingDisplay; PING_RESULT from the server)
  report.hudPing = await cdp.eval("(() => { const e = document.getElementById('pingDisplay') || document.getElementById('pingText'); return e ? e.textContent : null; })()").catch(() => null);
  report.knetRtt = rtts.length ? { samples: rtts.length, min: Math.min(...rtts), max: Math.max(...rtts),
    median: rtts.slice().sort((x, y) => x - y)[rtts.length >> 1] } : null;
  report.inGameUI = await cdp.eval("(() => { const e = document.getElementById('inGameUI'); return e ? getComputedStyle(e).display : null; })()").catch(() => null);
  report.knetStats = await cdp.eval("typeof _kn_wt_stats === 'function' ? UTF8ToString(_kn_wt_stats()) : null").catch(() => null);
  console.log(JSON.stringify(report));
  for (const l of lines.filter(l => /\[wt\]|\[rtc\]|webtransport|webrtc|transport|websocket|connected|EXCEPTION|refused/i.test(l)).slice(0, 30)) console.log('  ' + l);
  if (process.env.KN_E2E_VERBOSE) for (const l of lines) console.log('  | ' + l);
  process.exit(report.session && report.connected && (!process.env.KN_E2E_REQUIRE_PLAY || report.inGame) ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
