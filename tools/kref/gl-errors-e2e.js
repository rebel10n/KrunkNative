// Web GL-error e2e: the web client in headless Chromium (already started with
// --remote-debugging-port) plays offline for a while like a user (walk, turn, jump, shoot,
// reload, the pause menu and its class preview, back in game) and fails when the browser
// console shows any GL_INVALID_* error (WebGL rejects the draw: e.g. "Feedback loop formed
// between Framebuffer and active Texture") or "WebGL: too many errors".
//   node tools/kref/gl-errors-e2e.js <cdp port> <page url> <play seconds> [screenshot.png]
'use strict';
const { CDP, sleep } = require('./cdp.js');

(async () => {
  const [port, url, seconds] = [Number(process.argv[2]), process.argv[3], Number(process.argv[4] || 60)];
  const shotPath = process.argv[5] || '/tmp/kn-gl-errors-e2e.png';
  const cdp = await CDP.connect(port);
  const lines = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') lines.push(m.params.args.map(a => a.value !== undefined ? a.value : a.description).join(' '));
    if (m.method === 'Runtime.exceptionThrown') lines.push('EXCEPTION ' + JSON.stringify(m.params.exceptionDetails.text));
    // WebGL's own errors (GL_INVALID_*) reach the console as Log entries, not console API calls
    if (m.method === 'Log.entryAdded') lines.push('[log ' + m.params.entry.source + '] ' + m.params.entry.text);
  });
  await cdp.send('Runtime.enable');
  await cdp.send('Log.enable');
  await cdp.send('Page.enable');
  await cdp.setViewport(1280, 720);
  await cdp.send('Page.navigate', { url });
  const has = (re) => lines.some(l => re.test(l));
  const glErrors = () => lines.filter(l => /GL_INVALID_|WebGL: too many errors|CONTEXT_LOST_WEBGL/.test(l));
  const inGame = () => cdp.eval("(() => { const h = document.getElementById('inGameUI'); return !!(h && getComputedStyle(h).display !== 'none'); })()").catch(() => false);
  const clickPlay = async () => {
    const r = await cdp.eval("(() => { const e = document.getElementById('instructions'); const b = e && e.getBoundingClientRect(); return b && /CLICK TO PLAY/.test(e.textContent) ? [b.x + b.width / 2, b.y + b.height / 2] : null; })()").catch(() => null);
    if (r) await cdp.click(r[0], r[1]).catch(() => {});
  };

  // load + join (the sandbox starts in the page), then into the game
  let deadline = Date.now() + 60000, played = false;
  while (Date.now() < deadline && !played) {
    await sleep(500);
    if (!has(/offline: playing map/)) continue;
    if (await inGame()) played = true; else await clickPlay();
  }
  lines.push(played ? '[e2e] in game' : '[e2e] never reached the game');

  // play: every few seconds a different user action, for `seconds` of real time
  const keys = [['w', 'KeyW', 87], ['a', 'KeyA', 65], ['s', 'KeyS', 83], ['d', 'KeyD', 68]];
  let menus = 0, round = 0;
  deadline = Date.now() + seconds * 1000;
  while (played && Date.now() < deadline) {
    const [k, code, kc] = keys[round % keys.length];
    await cdp.keyDown(k, code, kc).catch(() => {});
    await cdp.mouse('mouseMoved', 640 + ((round * 97) % 200) - 100, 360, 'none').catch(() => {});
    await cdp.mouse('mousePressed', 640, 360, 'left').catch(() => {});
    await sleep(700);
    await cdp.key(' ', 'Space', 32).catch(() => {});
    await sleep(500);
    await cdp.mouse('mouseReleased', 640, 360, 'left').catch(() => {});
    await cdp.keyUp(k, code, kc).catch(() => {});
    if (round % 3 === 1) await cdp.key('r', 'KeyR', 82).catch(() => {});
    if (round % 4 === 3) {
      // the pause menu (class preview, page over the paused world) for a moment, then back in
      await cdp.key('Escape', 'Escape', 27).catch(() => {});
      await sleep(1500);
      menus++;
      await clickPlay();
      await sleep(500);
    }
    round++;
  }
  await cdp.shot(shotPath).catch(() => {});

  const errors = glErrors();
  const report = {
    offlinePlaying: lines.filter(l => /offline: playing map/.test(l)).length,
    inGame: played ? 1 : 0,
    rounds: round,
    menus,
    glErrors: errors.length,
    exceptions: lines.filter(l => /EXCEPTION/.test(l)).length,
    screenshot: shotPath,
  };
  console.log(JSON.stringify(report));
  for (const l of errors.slice(0, 20)) console.log('  ' + l);
  if (process.env.KN_E2E_VERBOSE) for (const l of lines) console.log('  | ' + l);
  process.exit(report.offlinePlaying && report.inGame && !report.glErrors && !report.exceptions ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
