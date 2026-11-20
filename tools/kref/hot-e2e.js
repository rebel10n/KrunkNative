// Web hot loader e2e + timing (doc/web-hot-loader.md): opens the game page in headless Chromium
// (started with --remote-debugging-port), clicks to play once the menu offers it, and reports the
// time from navigation to each step, from the page console and __knHot.times.
//   node tools/kref/hot-e2e.js <cdp port> <page url> <seconds> <label>
// Prints one JSON line {label, hot, adopted, ms: {...}}; exit 0 once in game.
'use strict';
const { CDP, sleep } = require('./cdp.js');

(async () => {
  const [port, url, seconds, label] = [Number(process.argv[2]), process.argv[3], Number(process.argv[4] || 60), process.argv[5] || 'run'];
  const cdp = await CDP.connect(port);
  const lines = [];
  let t0 = 0;
  const at = () => Date.now() - t0;
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') lines.push([at(), m.params.args.map(a => a.value !== undefined ? a.value : a.description).join(' ')]);
    if (m.method === 'Runtime.exceptionThrown') lines.push([at(), 'EXCEPTION ' + JSON.stringify(m.params.exceptionDetails.text)]);
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  // KN_E2E_LATENCY=<ms> KN_E2E_MBPS=<Mbit/s>: a real network's round trips and bandwidth on every HTTP
  // request (the page, the client files, the content, the matchmaker; WebRTC's UDP is not shaped)
  if (process.env.KN_E2E_LATENCY || process.env.KN_E2E_MBPS) {
    const bps = Number(process.env.KN_E2E_MBPS || 0) * 125000;
    await cdp.send('Network.enable');
    await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: Number(process.env.KN_E2E_LATENCY || 0),
      downloadThroughput: bps || -1, uploadThroughput: bps || -1 });
  }
  await cdp.send('Page.navigate', { url: 'about:blank' });
  await sleep(300);
  lines.length = 0;
  t0 = Date.now();
  await cdp.send('Page.navigate', { url });
  const first = (re) => { const l = lines.find(x => re.test(x[1])); return l ? l[0] : null; };
  const ms = {};
  const deadline = Date.now() + seconds * 1000;
  let clicked = 0;
  while (Date.now() < deadline && ms.inGame === undefined) {
    await sleep(100);
    const state = await cdp.eval("(() => { const h = document.getElementById('inGameUI'); const i = document.getElementById('instructions'); return (h && getComputedStyle(h).display !== 'none' ? 'in-game' : '') + '|' + (i ? i.textContent : ''); })()").catch(() => '');
    // (in game only after the click: before the page's CSS has loaded the HUD shows as display:block)
    if (/^in-game/.test(state) && ms.clickToPlay !== undefined) { ms.inGame = at(); break; }
    if (/CLICK TO PLAY/.test(state)) {
      if (ms.clickToPlay === undefined) ms.clickToPlay = at();
      if (Date.now() - clicked > 700) { // a trusted click on the line (the pointer lock needs a gesture)
        const r = await cdp.eval("(() => { const e = document.getElementById('instructions'); const b = e && e.getBoundingClientRect(); return b ? [b.x + b.width / 2, b.y + b.height / 2] : null; })()").catch(() => null);
        if (r) { await cdp.click(r[0], r[1]).catch(() => {}); clicked = Date.now(); }
      }
    }
  }
  const hot = await cdp.eval('window.__knHot ? JSON.stringify({times: __knHot.times, status: __knHot.status, used: __knHot.used, rtc: __knHot.rtc ? {adopted: __knHot.rtc.adopted, open: __knHot.rtc.open, failed: __knHot.rtc.failed} : null}) : null').catch(() => null);
  Object.assign(ms, {
    hotTaken: first(/\[hot\] the client takes/),
    rtcAdopted: first(/taking the hot loader's channel/),
    rtcConnecting: first(/\[rtc\] connecting to/),
    session: first(/session established/),
    connected: first(/connected to server/),
    seekGame: first(/\[mm\] (joining|hot seek)/),
  });
  // the page's downloads: the client files, and the game content (count, bytes, when the last ended)
  const res = await cdp.eval(`JSON.stringify((() => {
    const out = {};
    const nav = performance.timeOrigin;
    for (const e of performance.getEntriesByType('resource')) {
      const n = e.name.replace(/[?].*$/, '');
      const key = /client[.][^/]*js$/.test(n) ? 'js' : /[.]wasm$/.test(n) ? 'wasm' : /[.]data$/.test(n) ? 'data' : /[/]assets[/]/.test(n) ? 'assets' : /[/]mm[/]/.test(n) ? 'mm' : 'page';
      const o = out[key] = out[key] || {n: 0, kb: 0, first: 1e9, last: 0};
      o.n++; o.kb += Math.round((e.transferSize || 0) / 1024);
      o.first = Math.min(o.first, Math.round(e.startTime)); o.last = Math.max(o.last, Math.round(e.responseEnd));
    }
    return out;
  })())`).catch(() => null);
  const report = { label, url, hot: hot ? JSON.parse(hot) : null, ms, res: res ? JSON.parse(res) : null };
  console.log(JSON.stringify(report));
  if (process.env.KN_E2E_VERBOSE) for (const [t, l] of lines) console.log('  ' + t + ' | ' + l);
  else for (const [t, l] of lines.filter(x => /\[hot\]|\[rtc\]|\[mm\]|session established|connected to server|EXCEPTION|refused|fail/i.test(x[1])).slice(0, 25)) console.log('  ' + t + ' | ' + l);
  process.exit(ms.inGame !== undefined ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
