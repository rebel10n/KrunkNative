// Web offline e2e (client/src/net/offline.c): the web client in headless Chromium (already
// started with --remote-debugging-port), in real time, plays offline on the in-process sandbox
// server: the page starts the sandbox, joins it, Play enters the game, the player walks /
// jumps / shoots, dev console commands reach the sandbox's chat commands (bot_add, god), and
// the page keeps a usable frame rate with the server ticking inside the frame.
//   node tools/kref/offline-e2e.js <cdp port> <page url> <seconds> [screenshot.png]
'use strict';
const { CDP, sleep } = require('./cdp.js');

(async () => {
  const [port, url, seconds] = [Number(process.argv[2]), process.argv[3], Number(process.argv[4] || 40)];
  const shotPath = process.argv[5] || '/tmp/kn-offline-e2e.png';
  const cdp = await CDP.connect(port);
  const lines = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') lines.push(m.params.args.map(a => a.value !== undefined ? a.value : a.description).join(' '));
    if (m.method === 'Runtime.exceptionThrown') lines.push('EXCEPTION ' + JSON.stringify(m.params.exceptionDetails.text));
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  await cdp.setViewport(1280, 720);
  await cdp.send('Page.navigate', { url });
  const deadline = Date.now() + seconds * 1000;
  const has = (re) => lines.some(l => re.test(l));
  let phase = 'load', fps = null, played = false;
  while (Date.now() < deadline && phase !== 'done') {
    await sleep(500);
    if (phase === 'load' && has(/offline: playing map/)) phase = 'menu';
    if (phase === 'menu') {
      const state = await cdp.eval("(() => { const h = document.getElementById('inGameUI'); const i = document.getElementById('instructions'); return (h && getComputedStyle(h).display !== 'none' ? 'in-game' : '') + '|' + (i ? i.textContent : ''); })()").catch(() => '');
      if (/^in-game/.test(state)) {
        played = true;
        lines.push('[e2e] in game');
        phase = 'play';
      } else if (/CLICK TO PLAY/.test(state)) {
        const r = await cdp.eval("(() => { const e = document.getElementById('instructions'); const b = e && e.getBoundingClientRect(); return b ? [b.x + b.width / 2, b.y + b.height / 2] : null; })()").catch(() => null);
        if (r) await cdp.click(r[0], r[1]).catch(() => {});
      }
    }
    if (phase === 'play') {
      // walk, jump and shoot like a user (trusted input)
      await cdp.keyDown('w', 'KeyW', 87).catch(() => {});
      await cdp.mouse('mousePressed', 640, 360, 'left').catch(() => {});
      await sleep(600);
      await cdp.key(' ', 'Space', 32).catch(() => {});
      await sleep(900);
      await cdp.mouse('mouseReleased', 640, 360, 'left').catch(() => {});
      await cdp.keyUp('w', 'KeyW', 87).catch(() => {});
      // frames per second over 3 s of real time (requestAnimationFrame: the main loop's pace)
      fps = await cdp.eval("new Promise(r => { let n = 0; const t0 = performance.now(); const f = () => { n++; if (performance.now() - t0 < 3000) requestAnimationFrame(f); else r(n * 1000 / (performance.now() - t0)); }; requestAnimationFrame(f); })").catch(() => null);
      phase = 'console';
    }
    if (phase === 'console') {
      // the developer console (` toggles it): commands go to the sandbox server as "/cmd" chat
      for (const cmd of ['god', 'bot_add 2']) {
        await cdp.key('`', 'Backquote', 192).catch(() => {});
        await sleep(300);
        await cdp.send('Input.insertText', { text: cmd }).catch(() => {});
        await sleep(100);
        await cdp.key('Enter', 'Enter', 13).catch(() => {});
        await sleep(1200); // io_ct's burst limit
        await cdp.key('Escape', 'Escape', 27).catch(() => {});
        await sleep(300);
      }
      await sleep(3000);
      // the replies land in the chat / console DOM (web: not mirrored to the browser console)
      const text = await cdp.eval("document.body ? document.body.innerText + ' ' + Array.from(document.querySelectorAll('#chatList, #chatList *')).map(e => e.textContent).join(' ') : ''").catch(() => '');
      for (const m of String(text).match(/Spawned \d+ [a-z]+ bots|God mode (ON|OFF|unavailable)/g) || []) lines.push('[dom] ' + m);
      phase = 'done';
    }
  }
  await cdp.shot(shotPath).catch(() => {});
  const count = (re) => lines.filter(l => re.test(l)).length;
  const report = {
    sandboxStarted: count(/\[sandbox\] Loaded map/),
    offlinePlaying: count(/offline: playing map/),
    joined: count(/\[sandbox\] .* connected from 127\.0\.0\.1/),
    inGame: played ? 1 : 0,
    botsSpawned: count(/Spawned \d+ .*bots/),
    godReply: count(/God mode (ON|OFF|unavailable)/), // answered ("unavailable": the walk can fall off a small map)
    exceptions: count(/EXCEPTION/),
    fps: fps ? Math.round(fps * 10) / 10 : null,
    screenshot: shotPath,
  };
  console.log(JSON.stringify(report));
  for (const l of lines.filter(l => /sandbox|offline|EXCEPTION|abort|error/i.test(l)).slice(0, 40)) console.log('  ' + l);
  if (process.env.KN_E2E_VERBOSE) for (const l of lines) console.log('  | ' + l);
  const ok = report.sandboxStarted && report.offlinePlaying && report.joined && report.inGame && report.botsSpawned && report.godReply && !report.exceptions;
  process.exit(ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
