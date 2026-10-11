// Web UDP transport e2e (doc/transport.md): opens the web client in headless Chromium (already
// started with --remote-debugging-port), presses Play, and reports from the page console whether
// the session came up over WebTransport (knet), plus pings and game packets received.
//   node tools/kref/wt-e2e.js <cdp port> <page url> <seconds>
'use strict';
const { CDP, sleep } = require('./cdp.js');

(async () => {
  const [port, url, seconds] = [Number(process.argv[2]), process.argv[3], Number(process.argv[4] || 20)];
  const cdp = await CDP.connect(port);
  const lines = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') lines.push(m.params.args.map(a => a.value !== undefined ? a.value : a.description).join(' '));
    if (m.method === 'Runtime.exceptionThrown') lines.push('EXCEPTION ' + JSON.stringify(m.params.exceptionDetails.text));
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  await cdp.send('Page.navigate', { url });
  const deadline = Date.now() + seconds * 1000;
  let played = false;
  while (Date.now() < deadline) {
    await sleep(1000);
    if (!played && lines.some(l => /session established|connected to server/.test(l))) {
      await sleep(3000);
      // the menu's Play button (menu_ui.c), as a user click
      await cdp.eval("(() => { const b = document.getElementById('menuBtnPlay') || document.querySelector('[onclick*=play]'); if (b) b.click(); return !!b; })()").catch(() => {});
      played = true;
    }
  }
  const count = (re) => lines.filter(l => re.test(l)).length;
  const report = {
    webtransport: count(/webtransport session established/),
    websocketFallback: count(/using the WebSocket transport|connecting to ws/),
    connected: count(/connected to server/),
    downloadedMap: count(/downloaded map/),
  };
  // the HUD's ping (Krunker's #pingDisplay; PING_RESULT from the server)
  report.hudPing = await cdp.eval("(() => { const e = document.getElementById('pingDisplay') || document.getElementById('pingText'); return e ? e.textContent : null; })()").catch(() => null);
  report.knetStats = await cdp.eval("typeof _kn_wt_stats === 'function' ? UTF8ToString(_kn_wt_stats()) : null").catch(() => null);
  console.log(JSON.stringify(report));
  for (const l of lines.filter(l => /\[wt\]|webtransport|websocket|connected|EXCEPTION|refused/i.test(l)).slice(0, 30)) console.log('  ' + l);
  process.exit(report.webtransport && report.connected ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
