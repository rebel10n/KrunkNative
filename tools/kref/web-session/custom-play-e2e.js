'use strict';
// Web e2e for the Play button of a Custom Games card (windows[9] map cards: playMap(name)) and for
// the page's ?play=<map> link. Both must start the map (v10: playMap = /?play=<name>, the page joins
// a lobby on that map or starts one). The page is online (the stack's quick play joins its node),
// so the lobby it joins runs some other map: the card or link must end on the map, with no
// exception and no failed request. Before the fix the page stayed on the joined lobby's map.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/custom-play-e2e.js [screenshot.png]
const { openPage, sleep, stackEnv } = require('./session.js');

// the maps the Play card can name (Home tab cards of the Custom Games window)
const CANDIDATES = ['Burg', 'Littletown', 'Sandstorm', 'Subzero', 'Shipment', 'Freight'];

// the maps the stack's lobbies run now (the brain's game list), so the test names one they do not
async function lobbyMaps(base) {
  const res = await fetch(base + '/mm/game-list?hostname=krunknative');
  const body = await res.json();
  return (body.games || []).map((g) => g[4] && g[4].map).filter(Boolean);
}

// the in-game HUD's map line ("on Burg", a visible leaf element beside the mode name), '' before a
// game. #mapInfo is not used: the menu refreshes it only while the menu is open.
async function hudMap(page) {
  return page.cdp.eval(`(() => { for (const e of document.querySelectorAll('*')) { if (e.children.length === 0 && e.offsetParent !== null && /^on \\S/.test((e.textContent || '').trim())) return e.textContent.trim(); } return ''; })()`);
}

// the lobby the page joined: #mapInfo while the menu is open ("Free for All on Burg"), else the HUD
async function joinedLobby(page) {
  const info = await page.cdp.eval(`(() => { const e = document.getElementById('mapInfo'); return e ? (e.innerText || e.textContent || '').trim() : ''; })()`);
  return /\bon \S/.test(info) || (await hudMap(page)) !== '';
}

async function waitFor(fn, ms, step = 500) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const v = await fn();
    if (v) return v;
    await sleep(step);
  }
  return null;
}

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-custom-play.png';
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  const busy = await lobbyMaps(env.BASE);
  const map = CANDIDATES.find((m) => !busy.includes(m));
  if (!map) throw new Error('every candidate map is running in a lobby');

  // 1. the Games window's Play card
  {
    const page = await openPage(env.BASE + '/');
    try {
      let menu = false;
      for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
      check(menu, 'the main menu renders (online page: quick play joins the stack node)');
      const joined = await waitFor(() => joinedLobby(page), 60000);
      check(!!joined, 'the page joins a lobby first');
      await sleep(1500);
      await page.clickId('menuBtnCustomGames');
      const opened = await waitFor(() => page.visible('#windowCloser'), 10000, 250);
      check(!!opened, 'Custom Games opens');
      await sleep(1500);
      const r = await page.cdp.eval(`(() => { const c = [...document.querySelectorAll("[title='Play']")].find(e => e.getAttribute('onclick') === 'playMap("${map}")'); if (!c) return null; c.scrollIntoView({ block: 'center' }); const b = c.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()`);
      check(!!r, `the ${map} card has a Play button`);
      const snap = page.mark(); // from the Play click on: what starting the map may fail with
      if (r) {
        // the overlay shows on hover: move the pointer over the card first
        await page.cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: r[0], y: r[1] - 60, button: 'none' });
        await sleep(300);
        await page.cdp.click(r[0], r[1]);
      }
      const started = await waitFor(async () => (await hudMap(page)) === 'on ' + map, 40000);
      check(!!started, `Play on the ${map} card starts ${map} (HUD "${await hudMap(page)}")`);
      const d = page.since(snap);
      check(d.exceptions.length === 0, `no uncaught exception ${JSON.stringify(d.exceptions.slice(0, 2))}`);
      // the play's own seek for the map (v10 playMap: dataQuery {"i":"<map>"}) answers 503 NoMatchingServers when no
      // lobby runs that map: the expected answer, the play then starts the offline game on it. Any other 503 fails.
      const playProbe = (h) => /^503 \S*\/mm\/seek-game\?/.test(h) && h.includes(`dataQuery=%7B%22i%22%3A%22${encodeURIComponent(map)}%22`);
      const http = d.http.filter((h) => !/\/assets\/sound\//.test(h) && !playProbe(h)); // a stack asset missing from the tree, on every map
      check(http.length === 0 && d.failed.length === 0, `no failed or HTTP >= 400 request ${JSON.stringify([...d.failed, ...http].slice(0, 3))}`);
      check(d.gl.length === 0, `no WebGL error ${JSON.stringify(d.gl.slice(0, 2))}`);
      await sleep(2000);
      await page.shot(shot);
    } finally {
      page.close();
    }
  }

  // 2. the page's ?play=<map> link
  {
    const link = busy.includes('Littletown') ? 'Subzero' : 'Littletown';
    const page = await openPage(env.BASE + '/?play=' + encodeURIComponent(link));
    try {
      const started = await waitFor(async () => (await hudMap(page)) === 'on ' + link, 60000);
      check(!!started, `?play=${link} starts ${link} (HUD "${await hudMap(page)}")`);
      check(page.log.exceptions.length === 0, `?play=${link}: no uncaught exception ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
    } finally {
      page.close();
    }
  }

  if (failures.length) { console.log(`custom-play e2e: ${failures.length} failed`); process.exit(1); }
  console.log('custom-play e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
