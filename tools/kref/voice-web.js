// Web client side of server/tests/voice_e2e.py (scenario "web"): opens the page in headless Chromium
// (started with --remote-debugging-port and a fake microphone), gets in game, holds the voice key
// (V) for `talk` ms, and prints one JSON report: the page's [voice] console lines and what the
// mic icon showed; then listens for [listen ms] (default 2500) while a native client speaks.
//   node tools/kref/voice-web.js <cdp port> <page url> <talk ms> <seconds to get in game> [listen ms]
'use strict';
const { CDP, sleep } = require('./cdp.js');

const SCREEN = "(() => { const i = document.getElementById('instructions'); const h = document.getElementById('inGameUI');" +
  " return JSON.stringify({ instructions: i ? i.textContent : '', inGame: !!(h && getComputedStyle(h).display !== 'none') }); })()";
const ICON = "(() => { const e = document.getElementById('voiceIcon'); return e ? e.className + ' ' + (e.style.opacity || '') : ''; })()";

(async () => {
  const [port, url, talk, seconds] = [Number(process.argv[2]), process.argv[3], Number(process.argv[4] || 3000), Number(process.argv[5] || 60)];
  const cdp = await CDP.connect(port);
  const lines = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') lines.push(m.params.args.map(a => a.value !== undefined ? a.value : a.description).join(' '));
    if (m.method === 'Runtime.exceptionThrown') lines.push('EXCEPTION ' + JSON.stringify(m.params.exceptionDetails.text));
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  await cdp.send('Page.navigate', { url });
  const report = { inGame: false, icons: [], console: [] };
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline && !report.inGame) {
    await sleep(500);
    let st = {};
    try { st = JSON.parse(await cdp.eval(SCREEN)); } catch (e) { continue; }
    report.inGame = st.inGame;
    if (!st.inGame && /CLICK TO PLAY/.test(st.instructions || '')) {
      const r = await cdp.eval("(() => { const e = document.getElementById('instructions'); const b = e && e.getBoundingClientRect(); return b ? [b.x + b.width / 2, b.y + b.height / 2] : null; })()").catch(() => null);
      if (r) await cdp.click(r[0], r[1]).catch(() => {});
    }
  }
  if (report.inGame) {
    await sleep(1500);
    await cdp.keyDown('v', 'KeyV', 86);
    const end = Date.now() + talk;
    while (Date.now() < end) {
      await sleep(400);
      report.icons.push(await cdp.eval(ICON).catch(() => '?'));
    }
    await cdp.keyUp('v', 'KeyV', 86);
    await sleep(Number(process.argv[6] || 2500)); // the native client speaks meanwhile
  }
  report.console = lines.filter(l => /\[voice\]|Voice|EXCEPTION|getUserMedia/i.test(l)).slice(0, 80);
  console.log(JSON.stringify(report));
  cdp.close();
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(2); });
