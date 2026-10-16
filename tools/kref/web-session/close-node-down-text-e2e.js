'use strict';
// Web e2e: the only node stays down past the rejoin window (about 120 s). The player was in its lobby; the
// node is killed (no server ERROR, a dropped session), the client rejoins through the matchmaker every few
// seconds, and when the window ends the Lost Connection screen must say what v10 says for a server that
// cannot be reached ("DISCONNECTED", ID 0x000001), not "Servers are at Max Capacity" (the matchmaker's
// NoAvailableServers, which means the servers are full, not gone). servers_ui.c on_seek gives up here.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/close-node-down-text-e2e.js [screenshot.png]
const { execSync } = require('child_process');
const { openPage, sleep, stackEnv } = require('./session.js');

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-close-node-down.png';
  const page = await openPage(env.BASE + '/');
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  const lostText = () => page.cdp.eval(`(() => { const u = document.querySelector('#instructionsUpdate'); return u && getComputedStyle(u).display !== 'none' ? (u.innerText || '').trim() : ''; })()`);
  try {
    let play = false;
    for (let i = 0; i < 200 && !play; i++) {
      await sleep(500);
      play = await page.cdp.eval(`(() => { const e = document.querySelector('#instructions'); return !!e && /CLICK TO PLAY/.test(e.innerText || '') && getComputedStyle(e).display !== 'none'; })()`).catch(() => false);
    }
    check(play, 'joined the only node: CLICK TO PLAY shows');
    // the node dies for good (SIGKILL: no server ERROR to the client, a dropped session)
    const pid = execSync(`pgrep -f "[s]erver_bin --headless --config ${env.RUN}/node.json" | head -1 || true`).toString().trim();
    check(!!pid, `the node process is found (pid ${pid || 'none'})`);
    if (pid) execSync(`kill -KILL ${pid}`);
    // the rejoin window is REJOIN_WINDOW = 120 s; wait for the Lost Connection screen past it
    let text = '';
    const t0 = Date.now();
    while (Date.now() - t0 < 170000) {
      await sleep(2000);
      text = await lostText();
      if (Math.round((Date.now() - t0) / 1000) % 20 < 2) console.log(`info t+${Math.round((Date.now() - t0) / 1000)} s: ${JSON.stringify(text.slice(0, 80))}`);
      if (/Lost Connection/.test(text)) break;
    }
    const secs = Math.round((Date.now() - t0) / 1000);
    console.log(`info Lost Connection after ${secs} s: ${JSON.stringify(text.slice(0, 240))}`);
    check(/Lost Connection/.test(text), 'the Lost Connection screen shows once the rejoin window is over');
    check(/DISCONNECTED/.test(text), 'the error text is v10\'s player.disconnect: DISCONNECTED');
    check(!/Max Capacity/.test(text), 'the text does not say the servers are at max capacity');
    check(/ID: 0x000001/.test(text), 'the ID line is 0x000001 (the failed connect ID, as v10)');
    check(secs >= 115, `the screen waited out the rejoin window (${secs} s)`);
    await page.shot(shot);
    check(page.log.exceptions.length === 0, `no uncaught exception ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
    check(page.log.gl.length === 0, `no WebGL error ${JSON.stringify(page.log.gl.slice(0, 2))}`);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`close-node-down-text: ${failures.length} failed`); process.exit(1); }
  console.log('close-node-down-text: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
