'use strict';
// Web e2e: Find New Game (F4) with a single lobby, after the player joined it. Nothing else has room, so
// the player stays in the lobby. v10 says nothing here: the player lands back in the same game without a
// word (servers_ui.c on_seek, KrunkNative's NoOtherLobby). No popup or notice may cover CLICK TO PLAY,
// which stays clickable: the element at its centre is #instructions, and no #genericPop response popup shows.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/close-rejoin-popup-e2e.js [screenshot.png]
const { openPage, sleep, stackEnv } = require('./session.js');

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-close-rejoin-popup.png';
  const page = await openPage(env.BASE + '/');
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  try {
    // the launch seek joins the stack's one node: that lobby is the only one there is
    let play = false;
    for (let i = 0; i < 200 && !play; i++) {
      await sleep(500);
      play = await page.cdp.eval(`(() => { const e = document.querySelector('#instructions'); return !!e && /CLICK TO PLAY/.test(e.innerText || '') && getComputedStyle(e).display !== 'none'; })()`).catch(() => false);
    }
    check(play, 'joined the only lobby: CLICK TO PLAY shows');
    const snap = page.mark();
    await page.cdp.key('F4', 'F4', 115); // Find New Game: the lobby is the only one, so nothing else has room
    await sleep(3000);
    const state = await page.cdp.eval(`(() => {
      const pop = document.querySelector('#genericPop');
      const popShown = !!pop && getComputedStyle(pop).display !== 'none' && getComputedStyle(document.querySelector('#popupHolder')).display !== 'none';
      const instr = document.querySelector('#instructions');
      const r = instr.getBoundingClientRect();
      const top = document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2);
      const covered = !(top && (top === instr || instr.contains(top)));
      const still = !!instr && /CLICK TO PLAY/.test(instr.innerText || '') && getComputedStyle(instr).display !== 'none';
      const notes = [...document.querySelectorAll('.notification-item')].map((e) => e.innerText || '').join(' | ');
      return {popShown, covered, still, popText: popShown ? pop.innerText.trim().slice(0, 120) : '', notes};
    })()`);
    check(!state.popShown, `no response popup covers the page after Find New Game (popup: ${JSON.stringify(state.popText)})`);
    check(!state.covered, 'CLICK TO PLAY is not covered: the element at its centre is #instructions');
    check(state.still, 'the player stays in the game: CLICK TO PLAY is still shown');
    check(!state.notes.includes('No other lobby has room'), `no notice for the lobby the player is already in, as v10 (notices: ${JSON.stringify(state.notes.slice(0, 120))})`);
    await page.shot(shot);
    const d = page.since(snap);
    check(d.exceptions.length === 0, `no uncaught exception ${JSON.stringify(d.exceptions.slice(0, 2))}`);
    check(page.log.gl.length === 0, `no WebGL error ${JSON.stringify(page.log.gl.slice(0, 2))}`);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`close-rejoin popup: ${failures.length} failed`); process.exit(1); }
  console.log('close-rejoin popup: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
