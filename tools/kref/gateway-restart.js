// Web client side of server/tests/gateway_restart_e2e.py: opens the page in headless Chromium
// (started with --remote-debugging-port), gets in game, prints "IN_GAME", waits for a line on
// stdin (the test restarted a gateway meanwhile), then records what the page shows until it is in
// game again: the connection error screen's texts (#instructionsUpdate), the console lines, and
// whether a new session came up. Prints one JSON report line and exits.
//   node tools/kref/gateway-restart.js <cdp port> <page url> <seconds per phase> <session regex>
'use strict';
const readline = require('readline');
const { CDP, sleep } = require('./cdp.js');

const SCREEN = "(() => { const u = document.getElementById('instructionsUpdate'); const i = document.getElementById('instructions');" +
  " const h = document.getElementById('inGameUI');" +
  " return JSON.stringify({ update: u && getComputedStyle(u).display !== 'none' ? u.innerText : '', instructions: i ? i.textContent : ''," +
  " inGame: !!(h && getComputedStyle(h).display !== 'none') }); })()";

(async () => {
  const [port, url, seconds, sessionRe] = [Number(process.argv[2]), process.argv[3], Number(process.argv[4] || 40), new RegExp(process.argv[5] || 'session established')];
  const cdp = await CDP.connect(port);
  const lines = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') lines.push(m.params.args.map(a => a.value !== undefined ? a.value : a.description).join(' '));
    if (m.method === 'Runtime.exceptionThrown') lines.push('EXCEPTION ' + JSON.stringify(m.params.exceptionDetails.text));
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  await cdp.send('Page.navigate', { url });

  // play until the in-game HUD shows (menu_ui.c: CLICK TO PLAY needs a trusted click)
  const play = async (deadline) => {
    while (Date.now() < deadline) {
      await sleep(500);
      let st = {};
      try { st = JSON.parse(await cdp.eval(SCREEN)); } catch (e) { continue; }
      if (st.inGame) return true;
      if (/CLICK TO PLAY/.test(st.instructions || '')) {
        const r = await cdp.eval("(() => { const e = document.getElementById('instructions'); const b = e && e.getBoundingClientRect(); return b ? [b.x + b.width / 2, b.y + b.height / 2] : null; })()").catch(() => null);
        if (r) await cdp.click(r[0], r[1]).catch(() => {});
      }
    }
    return false;
  };
  const report = { firstSession: false, inGame: false, screens: [], sessionAgain: false, inGameAgain: false };
  report.inGame = await play(Date.now() + seconds * 1000);
  report.firstSession = lines.some(l => sessionRe.test(l));
  const mark = lines.length;
  console.log(report.inGame ? 'IN_GAME' : 'NOT_IN_GAME');
  if (!report.inGame) {
    report.console = lines.slice(-40);
    console.log(JSON.stringify(report));
    process.exit(1);
  }

  // the test restarts the gateway, then tells us
  await new Promise((res) => readline.createInterface({ input: process.stdin }).once('line', res));
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    await sleep(150);
    let st = {};
    try { st = JSON.parse(await cdp.eval(SCREEN)); } catch (e) { continue; }
    const text = (st.update || '').replace(/\s+/g, ' ').trim();
    if (text && report.screens[report.screens.length - 1] !== text) report.screens.push(text);
    if (lines.slice(mark).some(l => sessionRe.test(l) || /connected to server/.test(l))) report.sessionAgain = true;
    if (report.sessionAgain && report.screens.length) break;
  }
  if (report.sessionAgain) report.inGameAgain = await play(deadline + 20000);
  report.console = lines.slice(mark).filter(l => /disconnected|\[mm\]|\[reload\]|session|transport|reconnect|websocket|\[rtc\]|ERROR|EXCEPTION/i.test(l)).slice(0, 40);
  console.log(JSON.stringify(report));
  process.exit(0);
})().catch((e) => { console.error(e); process.exit(2); });
