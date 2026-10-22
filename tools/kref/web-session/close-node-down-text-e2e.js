'use strict';
// Web e2e: the only node dies under the player (SIGKILL: no server ERROR, a dropped session). As v10 (the game
// socket's onclose shows the Lost Connection screen at once, deob-10.0.3.js:111477 -> 261688 -> 261271), the
// client shows the Lost Connection screen within the session deadlines (client.h: 15 s idle, 20 s join; the
// bridge closes the WebSocket when the node's TCP goes), with the transport's reason and ID 0x000001, and rejoins
// nothing on a timer (servers_ui.c connection_dropped: no spinner for a 120 s rejoin window). Its Find Game
// with no lobby to join ends on that screen again (the matchmaker's answer), never on the spinner; once the
// node is back, Find Game joins it (CLICK TO PLAY).
//   KN_WEB_RUN=<stack run dir> [KN_WEB_BUILD=<build dir>] node tools/kref/web-session/close-node-down-text-e2e.js [screenshot.png]
const { execSync, spawn } = require('child_process');
const fs = require('fs');
const http = require('http');
const path = require('path');
const { openPage, sleep, stackEnv } = require('./session.js');

const LOST_DEADLINE_S = 25; // NET_JOIN_TIMEOUT_SECONDS (20 s) + the poll
const FIND_DEADLINE_S = 25;

function getJson(url) {
  return new Promise((resolve) => {
    http.get(url, (res) => { let b = ''; res.on('data', (d) => { b += d; }); res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { resolve(null); } }); })
      .on('error', () => resolve(null));
  });
}

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-close-node-down.png';
  const build = process.env.KN_WEB_BUILD || path.resolve(__dirname, '../../../cmake-build-debug');
  const page = await openPage(env.BASE + '/');
  const failures = [];
  let node2 = null;
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  const lostText = () => page.cdp.eval(`(() => { const u = document.querySelector('#instructionsUpdate'); return u && getComputedStyle(u).display !== 'none' ? (u.innerText || '').trim() : ''; })()`);
  const playShown = () => page.cdp.eval(`(() => { const e = document.querySelector('#instructions'); return !!e && /CLICK TO PLAY/.test(e.innerText || '') && getComputedStyle(e).display !== 'none'; })()`).catch(() => false);
  const waitLost = async (re, deadlineS) => {
    const t0 = Date.now();
    let text = '';
    while (Date.now() - t0 < deadlineS * 1000 + 5000) {
      await sleep(1000);
      text = await lostText();
      if (re.test(text)) break;
    }
    return { secs: (Date.now() - t0) / 1000, text };
  };
  try {
    let play = false;
    for (let i = 0; i < 200 && !play; i++) { await sleep(500); play = await playShown(); }
    check(play, 'joined the only node: CLICK TO PLAY shows');
    // the node dies for good (SIGKILL: no server ERROR to the client, a dropped session)
    const pid = execSync(`pgrep -f "[s]erver_bin --headless --config ${env.RUN}/node.json" | head -1 || true`).toString().trim();
    check(!!pid, `the node process is found (pid ${pid || 'none'})`);
    if (pid) execSync(`kill -KILL ${pid}`);
    const lost = await waitLost(/Lost Connection/, LOST_DEADLINE_S);
    console.log(`info Lost Connection after ${lost.secs.toFixed(1)} s: ${JSON.stringify(lost.text.slice(0, 240))}`);
    check(/Lost Connection/.test(lost.text), `the Lost Connection screen shows (${lost.secs.toFixed(0)} s)`);
    check(lost.secs <= LOST_DEADLINE_S, `it came within ${LOST_DEADLINE_S} s (${lost.secs.toFixed(1)} s), not after a rejoin window`);
    check(/Lost connection to the server/.test(lost.text), 'the text is the transport\'s reason: "Lost connection to the server ..."');
    check(/ID: 0x000001/.test(lost.text), 'the ID line is 0x000001 (the lost session, as v10)');
    check(/Find Game/.test(lost.text), 'the screen has its Find Game tab');
    // no rejoin on a timer: the screen stays, no spinner takes its place
    await sleep(6000);
    const still = await lostText();
    check(/Lost Connection/.test(still) && !(await playShown()), 'the screen stays: nothing rejoins behind a spinner');
    // Find Game with no lobby to join (the node is gone): the seek fails and ends on the screen again
    check(await page.clickSel('#instructionsUpdate .instructionsTab'), 'Find Game is clicked');
    await sleep(1500);
    const find = await waitLost(/Lost Connection/, FIND_DEADLINE_S);
    console.log(`info after Find Game (${find.secs.toFixed(1)} s): ${JSON.stringify(find.text.slice(0, 240))}`);
    check(/Lost Connection/.test(find.text) && find.secs <= FIND_DEADLINE_S, `Find Game with no lobby ends on the Lost Connection screen within ${FIND_DEADLINE_S} s, not on the spinner`);
    // the node comes back: Find Game joins it
    node2 = spawn(path.join(build, 'server_bin'), ['--headless', '--config', path.join(env.RUN, 'node.json'), '--cfg', 'none'],
      { stdio: ['ignore', fs.openSync(path.join(env.RUN, 'node2.log'), 'w'), fs.openSync(path.join(env.RUN, 'node2.log'), 'a')], detached: true });
    let listed = false;
    for (let i = 0; i < 60 && !listed; i++) {
      await sleep(1000);
      const list = await getJson(`http://127.0.0.1:${env.BRAIN}/mm/game-list?hostname=krunknative`);
      listed = !!(list && Array.isArray(list.games) && list.games.length > 0);
    }
    check(listed, 'the restarted node is listed by the brain again');
    check(await page.clickSel('#instructionsUpdate .instructionsTab'), 'Find Game is clicked again');
    play = false;
    for (let i = 0; i < 120 && !play; i++) { await sleep(500); play = await playShown(); }
    check(play, 'the reconnect works: Find Game joined the restarted node (CLICK TO PLAY)');
    await page.shot(shot);
    check(page.log.exceptions.length === 0, `no uncaught exception ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
    check(page.log.gl.length === 0, `no WebGL error ${JSON.stringify(page.log.gl.slice(0, 2))}`);
  } finally {
    page.close();
    if (node2) { try { process.kill(-node2.pid, 'SIGKILL'); } catch (e) { try { node2.kill('SIGKILL'); } catch (e2) { /* gone */ } } }
  }
  if (failures.length) { console.log(`close-node-down-text: ${failures.length} failed`); process.exit(1); }
  console.log('close-node-down-text: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
