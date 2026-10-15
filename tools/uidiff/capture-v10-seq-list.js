// The sequences of capture-v10-seq.js (doc/ref/krunker-v10.0.2/sequences/): UI-PARITY-AUDIT section 3.
// v10 line numbers (v10:N) are ~/.cache/kn-v10/v10.js, the client the sandbox runs. Every sequence
// names the native scenario that renders the same frames (tools/uidiff/scenarios/seq-<name>.json).
// `unreachable` sequences are listed with the reason and never captured (nothing is faked).
'use strict';
const { sleep } = require('../kref/cdp.js');

module.exports = function (seq, V) {
  const { SG, SP, CG } = V;
  const evalOk = async (c, js, what) => {
    const r = await c.eval(`(() => { try { ${js}; return 1; } catch (e) { return String(e.stack || e); } })()`);
    if (r !== 1) throw new Error(`${what}: ${r}`);
  };
  const inGame = async (c) => { await V.enterGame(c); await c.eval(V.PIN_TIMER); };
  const withBot = async (c) => { await inGame(c); await c.eval(V.SPAWN_BOT); await sleep(1500); };
  const menu = async (c) => { await V.waitSandbox(c); };
  const range = (from, to, step) => { const a = []; for (let t = from; t <= to + 1e-9; t += step) a.push(Math.round(t)); return a; };
  const shown = (sel) => `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false;
    const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0 && getComputedStyle(e).visibility !== 'hidden'; })()`;
  // a packet of the sandbox server to the page (v10's own handler table: 'error', 'gameAnnouncement', ...)
  const packet = (...args) => `${SG}.sandbox.send(null, ${args.map((a) => JSON.stringify(a)).join(', ')})`;
  const BOT = V.SERVER_BOT;

  // ---- A. boot / loading / instructions

  // v10:219396 instructionsFadeBG.style.opacity = 0 in the INIT handler; #instructionsFadeBG has
  // `transition: opacity 0.7s ease-in-out` (main.css:2115): the #222 curtain over the 3D view fades
  // over 0.7 s after the first INIT. It only covers the 3D canvas, so the checker stands in for it.
  seq('boot-curtain', {
    boot: true, bg: 'checker', times: [0, 100, 200, 350, 500, 700, 900],
    origin: `instructionsFadeBG.style.opacity === '0'`, originNote: 'the first INIT (v10:219396 instructionsFadeBG.style.opacity = 0)',
    probe: `getComputedStyle(instructionsFadeBG).opacity`,
    note: 'The boot curtain #instructionsFadeBG (#222, opacity 1) fading out over 0.7 s ease-in-out after the first INIT. Booted under virtual time (the page loads in 0 virtual ms: menu parts that wait on timers are not there yet); the checker shows where the 3D view is.',
  });
  // v10:197704 loadGamNm = "Loading <play|host> ..." for ?play= / ?host= links, on #loadingBg until INIT
  seq('loading-map-name', {
    // without custMap: with it the sandbox INITs in the same virtual instant the page loads
    boot: true, bg: '#222222', query: '?play=Kanji', times: [0], preScript: `localStorage.removeItem('custMap')`,
    origin: `/Loading/.test(loadGamNm.innerHTML) && getComputedStyle(loadingBg).display !== 'none'`,
    originNote: 'the first step showing #loadingBg with loadGamNm "Loading Kanji ..." (v10:197704)',
    probe: `loadGamNm.innerHTML`,
    note: '?play=Kanji link: the loading screen with "Loading Kanji ..." before the first INIT.',
  });
  // #instructions `grow 0.8s infinite alternate`: one 1.6 s cycle from the animation's start
  seq('click-to-play-grow', {
    bg: '#222222', prepare: menu, restartAnims: true, times: range(0, 1400, 200),
    probe: `getComputedStyle(instructions).transform`,
    note: 'CLICK TO PLAY (#instructions grow 0.8 s alternate): 8 frames over one 1.6 s cycle, the menu animations restarted at the origin (the native scenario: anim_origin "cycle").',
  });
  // v10:197768 _67300(n): "Auto Respawning in {n-1}" + tick_0 every 1 s, from the death handler
  // when the game's forceSpawn is set (v10:220379), only for a connected socket (v10:209740)
  seq('auto-respawn', {
    unreachable: 'v10 never shows "Auto Respawning in {0}" on a death: the death handler calls _67300(forceSpawn + 1) (v10:220379) while _69619 (in play) is still true, so _67303 stores null (v10:209740) and the 1 s chain stops after its first tick_0; _69619 = !1 comes on the next line. Tried in the sandbox with forceSpawn 3 and a connected socket, locked and unlocked: #instructions stays CLICK TO PLAY. Only the one tick_0 sound plays',
  });
  // v10:221923 _73848 (the "error" packet) -> _73828: the Lost Connection box (not "GAME UPDATING")
  seq('lost-connection', {
    bg: '#222222', prepare: menu, times: [0, 500],
    trigger: (c) => evalOk(c, packet('error', 'DISCONNECTED'), 'error packet'),
    probe: `instructionsUpdate.innerText`,
    note: 'The "error" packet with player.disconnect\'s text ("DISCONNECTED", what a dropped socket shows: v10:221943): #instructionHolder.onError with Lost Connection. No open transition (the 500 ms frame must equal the first).',
  });
  // the same with "GAME UPDATING": random menu art (seeded Math.random), credit, the stream iframes (blocked)
  seq('game-updating', {
    bg: '#222222', prepare: menu, times: [0, 500],
    trigger: (c) => evalOk(c, packet('error', 'GAME UPDATING'), 'error packet'),
    probe: `[instructionsUpdateBG.style.backgroundImage, instructionsUpdateCredit.innerText].join(' | ')`,
    note: 'The "error" packet with "GAME UPDATING" (v10:221859): the Game Updating screen, img/menu-bg/<id>.png art (Math.random is seeded) and its credit; the Twitch iframes stay blank offline.',
  });

  // ---- C/D. in game: death, kill, hit, chat emoji, announcement

  seq('death', {
    bg: 'checker', prepare: inGame, times: [0, 250, 500, 750, 1000, 1500, 2000],
    trigger: (c) => evalOk(c, `${SG}.players.kill(${SP}, null, {})`, 'kill'),
    origin: `getComputedStyle(deathUIHolder).display !== 'none'`, originNote: 'the death screen shown (v10:220377)',
    frameJs: V.PIN_TIMER,
    note: 'Self-kill (what the native console `kill` does): the "You Died" card (.death-ui-bottom-empty, backdrop-filter blur over the checker) with death-ui-moveup / chat-moveup 1.5 s.',
  });
  seq('kill', {
    bg: 'checker', drawnOrigin: true, prepare: withBot, times: [0, 100, 200, 300, 500, 700, 1000, 1500, 2000, 3500, 4000, 4200],
    trigger: (c) => evalOk(c, `${SG}.players.kill(${BOT}, ${SP}, {})`, 'kill the bot'),
    probe: `killFeed.childNodes.length`, frameJs: V.PIN_TIMER,
    note: 'The local player kills a sandbox bot: the kill feed row (4 s, then gone: 3500 / 4000 / 4200), the score popup and the medal; native: `bot 1` then `kill @bots`.',
  });
  // v10:173750 players.changeHealth(victim, doer, dmg): DO_DAMAGE to the doer (hit marker + damage number)
  seq('hit', {
    bg: 'checker', drawnOrigin: true, prepare: withBot, times: [0, 50, 100, 150, 200, 300, 500, 700, 1000],
    trigger: (c) => evalOk(c, `${SG}.players.changeHealth(${BOT}, ${SP}, 40, null, false, {})`, 'damage the bot'),
    frameJs: V.PIN_TIMER,
    note: 'The local player deals 40 damage to a bot (DO_DAMAGE): the hit marker and the damage number over 1 s. The native has no damage command: its scenario injects the hit marker (state.hit) at the origin; damage numbers are not injectable there yet.',
  });
  // typing ':' in the chat opens the emoji panel (v10:202779 $emojiPanelOpen)
  seq('emoji-panel', {
    unreachable: 'the sandbox capture hung after the first frame (CDP timeout at 900 s; the reference holds only 0 ms, not used): the chat Enter then ":" keyup path does not lay out #emojiPanelHolder > * within the budget',
    bg: 'checker', prepare: inGame, times: [0, 100, 200, 300, 500],
    // the panel follows chatInput's keyup handler (v10:202779): ':' typed, then its keyup
    trigger: async (c) => { await V.key(c, 'Enter', 'Enter', 13); await sleep(300); await c.send('Input.insertText', { text: ':' });
      await c.send('Input.dispatchKeyEvent', { type: 'keyUp', key: ':', code: 'Semicolon', windowsVirtualKeyCode: 186, nativeVirtualKeyCode: 186, modifiers: 8 }); },
    origin: shown('#emojiPanelHolder > *'), originNote: 'the emoji panel laid out (slideInUp)',
    frameJs: V.PIN_TIMER,
    note: 'Chat open, ":" typed: the emoji panel (svelte-13g6jqo, slideInUp).',
  });
  // the GAME_ANNOUNCEMENT packet ("gameAnnouncement", v10:209875 _70210) -> $gameAnnouncement
  seq('game-announcement', {
    unreachable: 'the GAME_ANNOUNCEMENT handler (_70210, v10:222148) is registered on the matchmaker socket when it connects; the sandbox socket has no event table (sock.events is null) and the $gameAnnouncement store (_33843) is module-private',
  });
  seq('nuke-flash', {
    unreachable: 'the Nuke is a 25-kill streak pickup (v10:158042) whose flash is drawn on the 3D overlay canvas; neither side has a command that forces it without 25 scripted kills, so it is left to a playtest capture',
  });

  // ---- E. end of match

  seq('end-vic', {
    bg: 'checker', drawnOrigin: true, prepare: inGame, times: range(0, 3000, 100),
    trigger: (c) => evalOk(c, `${SG}.endGame()`, 'endGame'),
    frameJs: V.PIN_TIMER,
    note: 'The sandbox game ends (the "end" packet): the victory title on the overlay canvas and #endUI, every 100 ms for 3 s. Native: console `end` (3 s timer), origin at the end packet.',
  });
  // results: v10 builds them (_72911, GUI.endScreen.init) only for a connected socket (v10:219684)
  seq('end-results', {
    bg: 'checker', drawnOrigin: true, prepare: inGame, times: [0, 2000, 4000, 5000, 5500, 6000, 7000],
    trigger: (c) => evalOk(c, `window.__knScope().sock.connected = true; ${SG}.endGame()`, 'endGame'),
    frameJs: V.PIN_TIMER,
    note: 'The end animation, then the results (tabs, Combat Earnings) with the socket marked connected.',
  });
  // otherData.items with a rarity (v10:203226) -> the Collect Rewards button (_66678, svelte-3svau8)
  // the items of capture-v10.js ingame-endscreen-collect (skins 1 and 2, the 'gift' icon) with v10's
  // store.skins put in (V.STORE_SKINS): without the catalogue the button has no item, stays disabled
  // (opacity 0.5, no countdown) and the click does nothing
  const REWARD_ITEMS = V.COLLECT_ITEMS;
  const endWithRewards = async (c) => {
    await inGame(c);
    if (await c.eval(V.STORE_SKINS()) !== 1) throw new Error('store.skins: not set');
    await evalOk(c, `window.__knScope().sock.connected = true; ${SG}.endGame()`, 'endGame');
    for (let i = 0; i < 40 && !await c.eval(`!!(window.GUI && GUI.endScreen && document.getElementById('endLayoutHolder'))`).catch(() => false); i++) await sleep(250);
  };
  seq('end-collect-button', {
    bg: 'checker', prepare: endWithRewards, times: [0, 500, 1000, 1500, 2000, 2500, 3000],
    trigger: (c) => evalOk(c, `GUI.endScreen.otherData = Object.assign({}, GUI.endScreen.otherData, { items: ${REWARD_ITEMS} }); GUI.endScreen.show()`, 'inject otherData.items'),
    origin: shown('.collect-rewards-button'), originNote: 'the Collect Rewards button laid out',
    frameJs: V.PIN_TIMER,
    note: 'Results with otherData.items injected (two skins with a rarity): the Collect Rewards button and its 3 s pulse.',
  });
  seq('end-rewards-overlay', {
    bg: 'checker', prepare: endWithRewards, times: [0, 250, 500, 750, 1000, 1500, 2000, 2500, 3000, 4000],
    trigger: async (c, vc) => {
      await evalOk(c, `GUI.endScreen.otherData = Object.assign({}, GUI.endScreen.otherData, { items: ${REWARD_ITEMS} }); GUI.endScreen.show()`, 'inject otherData.items');
      for (let i = 0; i < 60 && !await c.eval(shown('.collect-rewards-button')).catch(() => false); i++) {
        await vc.step(1000 / 60);
      }
      await evalOk(c, `document.querySelector('.collect-rewards-button').click()`, 'click Collect');
    },
    frameJs: V.PIN_TIMER,
    note: 'Collect clicked: the rewards overlay (svelte-1sac6ik: the card flips, then the items fly to the backpack, the setTimeout chain of v10:195771+).',
  });

  // ---- F/G/H. windows, menus, notifications

  // the guest "Register" notification: setTimeout 45 s + 15 s * Math.random() after the boot (v10:117375)
  seq('notification-register', {
    bg: '#222222', prepare: menu, times: [0, 100, 200, 300, 500, 1000, 2000],
    origin: shown('.register-notification'), originTimeout: 70000,
    originNote: 'the Register notification laid out (v10:117384, 45-60 s after the boot)',
    note: 'The notification center (svelte-nrigjp: slideIn 0.3 s, the 15 s countdown bar) with the guest Register notification, reached by letting virtual time run.',
  });
  seq('tooltip-loadout', {
    unreachable: 'a guest owns no skins: Change Loadout lists no item with createTip (0 on every tab), and the store window (showWindow(14)) throws without the signed-in store data',
  });
  // openRankedMenu() for a signed-in account below level 10 (v10:113127-113140: "Level Required", forceClose):
  // the message overlay mounts with transition:scale={{start: 0.9, duration: 200}} and fade, both ways
  // (g:114154-114158). The three frames are 80, 120 and 160 ms after it mounted.
  seq('ranked-overlay-open', {
    bg: '#000000', times: [80, 120, 160],
    prepare: async (c) => { await menu(c); },
    trigger: async (c) => {
      await evalOk(c, `window.__knScope().setAcc({ name: 'Sidney', level: 5, isDev: false, funds: 2500, score: 2600000,
        stats: {}, skins: [], badges: [], getSkinCount: () => 0 }); window.openRankedMenu()`, 'openRankedMenu');
    },
    origin: `[...document.querySelectorAll('.overlay-title')].some((e) => e.textContent === 'Level Required')`,
    originNote: 'the Level Required overlay mounts (v10:113127-113140, openRankedMenu for a level 5 account)',
    note: 'The ranked message overlay opening: the root fades (linear) and the box scales 0.9 -> 1 with its opacity (cubicOut), 200 ms, g:114154-114158.',
    unreachable: 'the offline sandbox never mounts v10\'s ranked component: openRankedMenu needs the ranked client the sign-in handler creates (v10:210029), and setAcc alone does not create it (the Level Required overlay never mounts). The uidiff scenario seq-ranked-overlay-open is skipped as unreachable; KrunkNative renders its frames with --render-scenario',
  });
  seq('ranked-leaderboard-open', {
    unreachable: 'the Ranked popup\'s leaderboard needs a signed-in account and matchmaker / API answers; the offline sandbox is a guest and only reaches ranked-guest (doc/ref/krunker-v10.0.2/ranked-guest.png)',
  });
  seq('spin', {
    unreachable: 'under virtual time the first screenshot after the reveal never returns (the spin renderer\'s own WebGL canvas, swiftshader: tried 2026-10-15, the capture sat idle 11 min at origin + 0). The won item\'s reveal is captured in real time instead: capture-v10.js spin-item* (signed in through v10\'s account handler, its own spin reply), native scenarios spin-item*.json',
  });
  seq('store-tooltip', {
    unreachable: 'the store\'s item list comes from the signed-in store API; the guest sandbox shows no store items to hover',
  });
  seq('mobile-menu', {
    bg: '#222222', prepare: async (c) => { await menu(c); await c.eval('window.toggleMobileUI && window.toggleMobileUI(true), 1'); await sleep(800); },
    times: [0],
    note: 'toggleMobileUI(true) on the menu (v10:160965): the mobile layout.',
  });
  seq('menu-1000', {
    size: [1000, 720], bg: '#222222', prepare: menu, restartAnims: true, times: [0],
    note: 'The menu in a 1000 px wide window: the @media (max-width) rules (narrow layout).',
  });
};
