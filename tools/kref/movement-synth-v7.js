// Deterministic Krunker v7.1.4 movement reference (v7 variant of movement-synth.js):
// calls the real v7 client's player.procInputs synchronously (inside one
// Runtime.evaluate, so the game loop cannot interleave) with synthetic inputs at a
// fixed delta, then restores the player. The physics comes from the real v7.1.4
// client; only the input timeline is synthetic.
//
//   node tools/kref/movement-synth-v7.js <scenario>|all [delta_ms|all] [out_dir]
//   node tools/kref/movement-synth-v7.js --list
//
// Needs (doc/v7/player-viewer.md 1.1.8 recipe):
//  - a v7.1.4 server on $KREF_PORT (default 6781), run from a COPY of the v7 tree
//    (its npm server writes accounts.db, server.pid and public/js into the tree):
//      cd <copy> && PORT=6781 LOBBY_COUNT=1 npm run server
//  - headless Chromium with CDP on $KREF_CDP (default 7781); started here if absent
//    (profile $KREF_PROFILE, default /tmp/kn-move-v7/chrome).
// The map is KrunkNative's own assets/maps/<map>.json (or a test fixture path) loaded as the v7 offline sandbox
// custom map (localStorage custMap), so the geometry is exactly what proc_input_test
// loads and only the physics can differ. Nothing in the v7 tree is modified.
//
// v7 input array (index.js:16833) differs from v3: [2] is pitch and [3] yaw (v3: yaw,
// pitch), [8] reload and [9] crouch (v3: crouch, reload), jump [7] is sent while held,
// [13] reset, [14] interact. The output undoes the swaps:
//
// Writes <out>/v7_<scenario>_<fps>fps.json (33 ms = 30, 17 ms = 60, 7 ms = 144 fps):
//   {"meta": {...}, "ticks": [[[delta, yaw*1000, pitch*1000, movDir, jump, crouch, aim],
//                               [x, y, z, vx, vy, vz, rampFix, stepVal, stepChase, bobAnimY,
//                                bobAnimZ, leanAnimX, leanAnimY, leanAnimZ, wallJumpC,
//                                canSlideT, onPad, onGround, crouchVal, slideTimer(, health)]], ...]}
// (health only in sandbox runs: the server-side player of the offline sandbox, for fall damage)
// The state is the player's BEFORE the tick's input (proc_input_test checks it, then
// applies the input). movDir is v7's config.movDirs index (-1 none). meta carries the
// map, class, weapon and the Game.config values the run pinned.
'use strict';
const { CDP, sleep } = require('./cdp.js');
const fs = require('fs');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const PORT = process.env.KREF_PORT || '6781';
const CDP_PORT = +(process.env.KREF_CDP || 7781);
const PROFILE = process.env.KREF_PROFILE || '/tmp/kn-move-v7/chrome';
const KN = path.resolve(__dirname, '../..');
const URL = `http://127.0.0.1:${PORT}/`;

// movDirs index (config.movDirs[i] = -PI + (i + 1) * PI / 4): W=1, S=5, D=3, A=7, WD=2, WA=0, SD=4, SA=6
// yaw: -PI/2 faces +x, -PI faces +z, PI/2 faces -x, 0 faces -z.
// Scenario: map, start pose, duration (ms), class (cls), weapon override (wep),
// Game.config overrides (cfg), per-tick function f(t) -> {mv, jump (held), crouch, aim,
// dyaw, dpitch (rad/ms)}, and the deltas to record.
const S = '311_sandstorm'; // the v3.1.1 Sandstorm (assets/maps/311_sandstorm.json)
const M = 'shared/tests/maps/v7_player_movement'; // a path: KrunkNative test fixture, else assets/maps/<map>.json
const W = -Math.PI / 2;
const SC = {
  // v3 synth scenarios, re-recorded
  walk: { map: S, pos: [0, 13, 0], yaw: W, dur: 2400, deltas: [17, 7], f: t => ({ mv: t < 1500 ? 1 : -1 }) },
  walk_diag: { map: S, pos: [0, 13, 0], yaw: W, dur: 2400, deltas: [17, 7], f: t => ({ mv: t < 1500 ? 2 : -1 }) },
  jump: { map: S, pos: [0, 13, 0], yaw: W, dur: 1600, deltas: [17, 7], f: t => ({ mv: -1, jump: t >= 100 && t < 160 }) },
  hold_jump: { map: S, pos: [0, 13, 0], yaw: W, dur: 2600, deltas: [17, 7], f: t => ({ mv: -1, jump: t >= 100 && t < 2300 }) },
  sprint_jump: { map: S, pos: [0, 13, 0], yaw: W, dur: 2600, deltas: [33, 7], f: t => ({ mv: t < 1800 ? 1 : -1, jump: t >= 600 && t < 660 }) },
  crouch_slide: { map: S, pos: [0, 13, 0], yaw: W, dur: 2800, deltas: [7, 17, 33], f: t => ({ mv: t < 1500 ? 1 : -1, jump: t >= 600 && t < 660, crouch: t >= 800 && t < 2300 }) },
  crouch_walk: { map: S, pos: [0, 13, 0], yaw: W, dur: 2600, deltas: [33, 7], f: t => ({ mv: t >= 300 && t < 1500 ? 1 : -1, crouch: t < 2000 }) },
  crouch: { map: S, pos: [0, 13, 0], yaw: W, dur: 1600, deltas: [17, 7], f: t => ({ mv: -1, crouch: t >= 100 && t < 700 }) },
  aim_walk: { map: S, pos: [0, 13, 0], yaw: W, dur: 2400, deltas: [17, 7], f: t => ({ mv: t < 1600 ? 1 : -1, aim: t >= 200 && t < 1200 }) },
  scope: { map: S, pos: [0, 13, 0], yaw: W, dur: 1600, deltas: [17, 7], f: t => ({ mv: -1, aim: t >= 100 && t < 900 }) },
  turn_jump: { map: S, pos: [0, 13, 0], yaw: W, dur: 2200, deltas: [7], f: t => ({ mv: t < 1400 ? (t < 600 ? 1 : 3) : -1, jump: t >= 500 && t < 560, dyaw: t >= 600 && t < 1200 ? 0.002 : 0 }) },
  look: { map: S, pos: [0, 13, 0], yaw: W, dur: 1400, deltas: [17], f: t => ({ mv: t >= 800 && t < 1100 ? 3 : -1, dyaw: t >= 100 && t < 400 ? 0.003 : t >= 400 && t < 500 ? -0.004 : 0, dpitch: t >= 500 && t < 700 ? -0.002 : 0 }) },
  // ladder (prefab 3 at 205,33,130, d 3): climb up, then hold S to climb back down past the bottom (L1).
  // yaw -3.1, not -PI: facing the ladder within a milliradian moves x by less than half a float
  // ulp per tick, and KrunkNative's float positions would round every step to a whole ulp
  ladder: { map: S, pos: [205, 33.5, 118], yaw: -3.1, dur: 5600, deltas: [17, 7], f: t => ({ mv: t < 3200 ? 1 : t >= 3400 && t < 5400 ? 5 : -1 }) },
  // walk into the wall z 12..26 (object 288) and keep pushing
  collide: { map: S, pos: [0, 13, 0], yaw: -Math.PI, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 1300 ? 1 : -1 }) },
  // up the ramp (object 8, rises toward +x) and on to its top
  ramp: { map: S, pos: [10, 13, -4], yaw: W, dur: 2600, deltas: [17, 7], f: t => ({ mv: t < 2200 ? 1 : -1 }) },
  spawn: { map: S, pos: [0, 15, 0], yaw: W, dur: 800, deltas: [17, 7], settle: 0, f: t => ({ mv: -1 }) },
  jump_pad: { map: '311_kanji', pos: [75, -140, -270], yaw: W, dur: 2400, deltas: [33, 7], f: t => ({ mv: -1 }) },
  // 311_soul_sanctum: drop onto the spawn disk (cylinder top y 3), stand, walk, stop on it
  soul_spawn: { map: '311_soul_sanctum', pos: [1, 4, 0], yaw: W, dur: 1600, deltas: [33, 17], f: t => ({ mv: t >= 300 && t < 800 ? 1 : -1 }) },

  // new v7 scenarios (doc/v7/player-viewer.md 1.1.8)
  // W1: Run N Gun (wallJM 2): jump at the wall z 12..26, then tap jump while on it
  wall_jump: { map: S, pos: [0, 13, 4], yaw: -Math.PI, dur: 1800, deltas: [17, 7], cls: 2,
    f: t => ({ mv: t < 1400 ? 1 : -1, jump: (t >= 50 && t < 100) || (t >= 400 && t < 434) }) },
  // (k = tick index: tap jump on every other tick while on the wall)
  wall_jump_limit: { map: S, pos: [0, 13, 4], yaw: -Math.PI, dur: 2400, deltas: [17, 7], cls: 2,
    f: (t, k) => ({ mv: t < 2000 ? 1 : -1, jump: (t >= 50 && t < 100) || (t >= 300 && t < 1100 && k % 2 == 0) }) },
  wall_jump_agent: { map: S, pos: [0, 13, 4], yaw: -Math.PI, dur: 2400, deltas: [17, 7], cls: 8,
    f: (t, k) => ({ mv: t < 2000 ? 1 : -1, jump: (t >= 50 && t < 100) || (t >= 300 && t < 1100 && k % 2 == 0) }) },
  // landing resets the wall-jump counter: two wall-jump sequences with a landing between
  wall_jump_reset: { map: S, pos: [0, 13, 4], yaw: -Math.PI, dur: 4000, deltas: [17], cls: 2,
    f: (t, k) => ({ mv: t < 3600 ? 1 : -1, jump: (t >= 50 && t < 100) || (t >= 300 && t < 700 && k % 2 == 0) ||
      (t >= 2000 && t < 2050) || (t >= 2250 && t < 2650 && k % 2 == 0) }) },
  // wall slide: crouched and falling on a wall, gravity x wallJD
  wall_slide: { map: S, pos: [0, 13, 4], yaw: -Math.PI, dur: 1800, deltas: [17, 7], cls: 2,
    f: t => ({ mv: t < 1600 ? 1 : -1, jump: t >= 50 && t < 100, crouch: t >= 450 && t < 1500 }) },
  // S1: walk down the ramp and crouch half way (v3 slid at the next grounded step down; v7 must not)
  ramp_crouch_descend: { map: S, pos: [100, 33, -4], yaw: Math.PI / 2, dur: 3600, deltas: [17, 7],
    f: t => ({ mv: t < 3200 ? 1 : -1, crouch: t >= 700 }) },
  // S2: slide on landing, release crouch to crouchVal <= 0.5 and crouch again (re-slide lock)
  slide_relock: { map: S, pos: [0, 13, 0], yaw: W, dur: 2600, deltas: [17, 7],
    f: t => ({ mv: t < 1800 ? 1 : -1, jump: (t >= 300 && t < 350) || (t >= 1350 && t < 1400), crouch: (t >= 500 && t < 1290) || (t >= 1380 && t < 2300) }) },
  // S5: turn during a slide: the velocity follows the look direction
  slide_steer: { map: S, pos: [0, 13, 0], yaw: W, dur: 2200, deltas: [17, 7],
    f: t => ({ mv: t < 700 ? 1 : -1, jump: t >= 300 && t < 350, crouch: t >= 500 && t < 1800, dyaw: t >= 1100 && t < 1500 ? 0.002 : 0 }) },
  // S6 / class speeds: Hunter (1.0), Spray N Pray (0.9, slA 0.98), Trooper (0.9, slT 1.1)
  slide_class_snp: { map: S, pos: [0, 13, 0], yaw: W, dur: 2400, deltas: [17, 7], cls: 3,
    f: t => ({ mv: t < 1200 ? 1 : -1, jump: t >= 500 && t < 550, crouch: t >= 700 && t < 2000 }) },
  slide_class_trooper: { map: S, pos: [0, 13, 0], yaw: W, dur: 2400, deltas: [17, 7], cls: 13,
    f: t => ({ mv: t < 1200 ? 1 : -1, jump: t >= 500 && t < 550, crouch: t >= 700 && t < 2000 }) },
  class_walk_hunter: { map: S, pos: [0, 13, 0], yaw: W, dur: 1800, deltas: [17, 7], cls: 1, f: t => ({ mv: t < 1200 ? 1 : -1 }) },
  class_walk_snp: { map: S, pos: [0, 13, 0], yaw: W, dur: 1800, deltas: [17, 7], cls: 3, f: t => ({ mv: t < 1200 ? 1 : -1 }) },
  class_walk_trooper: { map: S, pos: [0, 13, 0], yaw: W, dur: 1800, deltas: [17, 7], cls: 13, f: t => ({ mv: t < 1200 ? 1 : -1 }) },
  // R1: air strafe (Game.config.airStrf) on pure strafe and forward diagonals
  air_strafe: { map: S, pos: [0, 13, 0], yaw: W, dur: 2600, deltas: [17, 7], cfg: { airStrf: true },
    f: t => ({ mv: t < 500 ? 1 : t < 1000 ? 3 : t < 1500 ? 2 : t < 2000 ? 0 : -1, jump: (t >= 450 && t < 500) || (t >= 1450 && t < 1500),
      dyaw: t >= 550 && t < 950 ? 0.002 : t >= 1550 && t < 1750 ? 0.002 : t >= 1750 && t < 1950 ? -0.002 : 0 }) },
  // J4: autoJump with jump held across several landings
  auto_jump_hold: { map: S, pos: [0, 13, 0], yaw: W, dur: 3000, deltas: [17, 7], cfg: { autoJump: true }, f: t => ({ mv: -1, jump: t >= 100 && t < 2600 }) },
  // P1: walk onto the edge of Kanji's pad (hitbox from the map scale)
  jump_pad_edge: { map: '311_kanji', pos: [75, -140, -258], yaw: 0, dur: 2600, deltas: [17, 7], f: t => ({ mv: t < 500 ? 1 : -1 }) },
  // C1: jump onto a border wall (object 257, bo: 1): the box reaches borderH = 500 higher
  border_jump: { map: S, pos: [206, 33, -2], yaw: 0, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 900 ? 1 : -1, jump: t >= 100 && t < 150 }) },
  // C1: drop into a border's extension (object 257, top 87): v3.1.1's box reached 250 above the
  // top, v7's 500, so a player 300 above it is inside the box and falls through it
  border_drop: { map: S, pos: [206, 387, -15], yaw: 0, dur: 2500, deltas: [17], settle: 0, f: t => ({ mv: -1 }) },
  // A6: fixMov (exact exponential integration)
  fixmov_walk: { map: S, pos: [0, 13, 0], yaw: W, dur: 2400, deltas: [17, 7], cfg: { fixMov: true },
    f: t => ({ mv: t < 1500 ? 1 : -1, jump: t >= 600 && t < 650 }) },
  // J2: the Minigun (weapon 25, noJump) cannot jump
  minigun_nojump: { map: S, pos: [0, 13, 0], yaw: W, dur: 1600, deltas: [17, 7], wep: 25,
    f: t => ({ mv: t < 800 ? 1 : -1, jump: (t >= 100 && t < 150) || (t >= 900 && t < 950) }) },
  // P1-P4 on the fixture map shared/tests/maps/v7_player_movement.json (one pad per direction)
  pad_down: { map: M, pos: [-100, 50, 0], yaw: W, dur: 1400, deltas: [17, 7], settle: 0, f: t => ({ mv: -1 }) },
  pad_x: { map: M, pos: [-62, 2, 0], yaw: W, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 300 ? 1 : -1 }) },
  pad_z: { map: M, pos: [0, 2, 38], yaw: -Math.PI, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 300 ? 1 : -1 }) },
  pad_neg_x: { map: M, pos: [62, 2, 0], yaw: Math.PI / 2, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 300 ? 1 : -1 }) },
  pad_neg_z: { map: M, pos: [0, 2, -38], yaw: 0, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 300 ? 1 : -1 }) },
  pad_bounce_zero: { map: M, pos: [88, 2, 0], yaw: W, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 600 ? 1 : -1 }) },
  pad_on_block: { map: M, pos: [100, 8, 91], yaw: -Math.PI, dur: 2000, deltas: [17, 7], f: t => ({ mv: t < 250 ? 1 : -1 }) },
  pad_crouch: { map: M, pos: [-115, 2, 100], yaw: W, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 400 ? 1 : -1, crouch: true }) },
  // S2: land crouched on the 0.05-high slab (crouchVal <= 0.5 keeps canSlide), slide off its edge
  // and land again on the floor inside the 100 ms canSlideT lock
  slide_lock: { map: M, pos: [10, 2.05, -100], yaw: W, dur: 2200, deltas: [17, 7],
    f: t => ({ mv: t < 1600 ? 1 : -1, jump: t >= 300 && t < 350, crouch: t >= 1060 && t < 1130 }) },
  // C2: walk into the 2-unit gap between two cylinders (both push in one tick)
  cylinder_side: { map: M, pos: [45, 2, -54], yaw: W, dur: 1600, deltas: [17, 7], f: t => ({ mv: t < 1300 ? 1 : -1 }) },
  // C4: crouch under a 10-high ceiling, stand up under it, jump (headClipFix off / on)
  head_clip: { map: M, pos: [-82, 2, -60], yaw: W, dur: 2200, deltas: [17, 7],
    f: t => ({ mv: t < 900 ? 1 : -1, crouch: t < 1100, jump: t >= 1500 && t < 1550 }) },
  head_clip_fix: { map: M, pos: [-82, 2, -60], yaw: W, dur: 2200, deltas: [17, 7], cfg: { headClipFix: true },
    f: t => ({ mv: t < 900 ? 1 : -1, crouch: t < 1100, jump: t >= 1500 && t < 1550 }) },
  // C7: fall damage is server-only (players.js isServer): recorded on the sandbox authority
  // (sandboxPlayer / sandboxGame), with health in the state row
  fall_damage: { map: S, pos: [0, 200, 0], yaw: W, dur: 3000, deltas: [33, 17], settle: 0, sandbox: true,
    cfg: { fallDmg: 10, fallDmgThr: 0.1 }, f: t => ({ mv: -1 }) },
  // A1: zero-delta frames integrate as 0.1 ms
  delta_zero: { map: S, pos: [0, 13, 0], yaw: W, dur: 600, deltas: [17], zeroEvery: 3, f: t => ({ mv: t < 400 ? 1 : -1, jump: t >= 100 && t < 150 }) },
};

// file names keep the v3 convention: 33 ms = 30 fps, 17 ms = 60 fps, 7 ms = 144 fps
const FPS = { 33: 30, 17: 60, 7: 144 };

function up(port) { return new Promise(res => http.get(`http://127.0.0.1:${port}/json/version`, r => res(r.statusCode === 200)).on('error', () => res(false))); }

async function connect() {
  if (!(await up(CDP_PORT))) {
    const p = spawn('chromium-browser', ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
      '--window-size=1280,720', `--remote-debugging-port=${CDP_PORT}`, '--user-data-dir=' + PROFILE, '--mute-audio',
      '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', 'about:blank'],
      { detached: true, stdio: 'ignore' });
    p.unref();
    for (let i = 0; i < 40 && !(await up(CDP_PORT)); i++) await sleep(250);
  }
  const c = await CDP.connect(CDP_PORT, t => t.type === 'page' && !t.url.startsWith('chrome-extension'));
  await c.send('Page.enable'); await c.send('Runtime.enable');
  // pointer-lock shim; v7 reads the same krk_force_offline_sandbox key (mod/mods.ts), keep it on
  const shim = fs.readFileSync(path.join(__dirname, 'shim.js'), 'utf8').replace("'krk_force_offline_sandbox','false'", "'krk_force_offline_sandbox','true'");
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: shim });
  return c;
}

// Load KN's map as the sandbox custom map and spawn the local player (window.enterGame).
async function ensureMap(c, map) {
  const cur = await c.eval(`(()=>{try{return location.href.startsWith(${JSON.stringify(URL)}) && window.Game && Game.players ? localStorage.getItem('kn_map') : null}catch(e){return null}})()`);
  if (cur !== map) {
    if (!(await c.eval('location.href')).startsWith(URL)) { await c.send('Page.navigate', { url: URL }); await sleep(3000); }
    const data = fs.readFileSync(map.includes('/') ? path.join(KN, map + '.json') : path.join(KN, 'assets/maps', map + '.json'), 'utf8');
    await c.eval(`localStorage.setItem('custMap', ${JSON.stringify(JSON.stringify(JSON.parse(data)))}); localStorage.setItem('kn_map', ${JSON.stringify(map)}); 1`);
    await c.send('Page.navigate', { url: URL });
    await sleep(2000);
    await c.waitFor(`!!(window.Game && Game.players && window.sandboxGame && sandboxGame.map && typeof enterGame == 'function')`, 120000);
    await sleep(1000);
  }
  if (!(await c.eval(`Game.players.list.some(p => p.isYou && p.active)`))) {
    await c.eval(`enterGame(); 1`);
    await c.waitFor(`Game.players.list.some(p => p.isYou && p.active)`, 30000);
    await sleep(500);
  }
}

async function record(c, name, delta) {
  const sc = SC[name];
  await ensureMap(c, sc.map);
  const res = await c.eval(`(()=>{
    const G = ${sc.sandbox ? 'sandboxGame' : 'Game'};
    const p = ${sc.sandbox ? 'sandboxPlayer' : 'Game.players.list.find(p => p.isYou)'};
    if (!p || !p.active) return {err: 'no player'};
    const proc = p.procInputs;
    const saved = {}; for (const k of Object.keys(p)) { const v = p[k]; if (v === null || typeof v !== 'object' && typeof v !== 'function') saved[k] = v; }
    const savedVel = p.velocity.clone(), savedCls = p.classIndex, savedCfg = {};
    const cfg = ${JSON.stringify(sc.cfg || {})};
    for (const k in cfg) { savedCfg[k] = G.config[k]; G.config[k] = cfg[k]; }
    const sw = () => { G.players.swapWeapon(p, null, true, undefined, 0, true); p.swapTime = 0; p.reloadTimer = 0; };
    ${sc.cls != null ? `p.setClass(G, ${sc.cls}, null, true); sw();` : ''}
    ${sc.wep != null ? `p.loadout[0] = ${sc.wep}; sw();` : ''}
    const f = ${sc.f.toString()};
    const n = v => v == null ? null : +v;
    const st = () => [p.x, p.y, p.z, p.velocity.x, p.velocity.y, p.velocity.z, p.rampFix == null ? null : p.rampFix,
      p.stepVal, p.stepChase, p.bobAnimY, p.bobAnimZ, p.leanAnimX, p.leanAnimY, p.leanAnimZ,
      p.wallJumpC, p.canSlideT, p.onPad ? 1 : 0, p.onGround ? 1 : 0, p.crouchVal, p.slideTimer${sc.sandbox ? ', p.health' : ''}];
    p.x=${sc.pos[0]}; p.y=${sc.pos[1]}; p.z=${sc.pos[2]}; p.oldX=p.x; p.oldY=p.y; p.oldZ=p.z;
    p.velocity.set(0, 0, 0); p.onGround=true; p.wasOnGround=true; p.crouchVal=0; p.aimVal=1; p.slideTimer=0; p.rampFix=null;
    p.xDire=${sc.yaw}; p.yDire=0; p.didJump=false; p.didJumpW=false; p.jumpCooldown=0; p.canSlide=true; p.canSlideT=0;
    p.wallJumpC=0; p.onPad=false; p.wasOnPad=false; p.stopCrouch=false; p.wasStopCrouch=false; p.grapple=null;
    p.onWall=0; p.onLadder=false; p.onTerrain=false; p.xVelC=0; p.zVelC=0; p.airTime=0; p.aimTime=0;
    G.players.updateHeight(p); p.heightOld=p.height; ${sc.sandbox ? 'p.health = p.maxHealth;' : ''}
    p.stepVal=p.stepChase=0; p.stepDir=1; p.bobAnimY=p.bobAnimZ=0; p.leanAnimX=p.leanAnimY=p.leanAnimZ=0; p.recoilX=p.recoilZ=0;
    p.recoilAnim=p.recoilAnimY=p.recoilForce=0; p.spreadPlus=0; p.inspecting=false; p.inspectX=0;
    const meta = {map: ${JSON.stringify(sc.map)}, scenario: ${JSON.stringify(name)}, delta: ${delta}, version: 'v7.1.4',
      classIndex: p.classIndex, className: p.config.name, classSpeed: p.speed, wallJump: p.wallJump, wallJumpMax: p.wallJumpMax,
      weapon: p.weapon.id, weaponName: p.weapon.name, weaponSpdMlt: p.weapon.spdMlt, mode: G.mode && G.mode.id, authority: ${!!sc.sandbox}, health: p.health,
      config: Object.fromEntries(['canSlide','slidSpd','slidTime','strafeSpd','airStrf','autoJump','fixMov','headClipFix','gravMlt','jumpMlt','wallJP','disableB','fallDmg','fallDmgThr','impulseMlt'].map(k => [k, G.config[k]]))};
    let yaw = ${sc.yaw}, pitch = 0, isn = 1;
    const ticks = [];
    // settle onto the ground with idle inputs (recorded too: the native replay starts at the same pose)
    const settle = ${sc.settle == null ? 30 : sc.settle};
    for (let i = 0; i < settle; i++) {
      const input = [isn++, ${delta}, 0, Math.round(yaw*1000), -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
      ticks.push([[${delta}, input[3], input[2], -1, 0, 0, 0], st()]);
      proc.call(p, input, G, false, false);
    }
    let k = 0;
    for (let t = 0; t < ${sc.dur}; t += ${delta}, k++) {
      const s = f(t, k);
      const d = ${sc.zeroEvery ? `k % ${sc.zeroEvery} == 1 ? 0 : ${delta}` : delta};
      yaw += (s.dyaw || 0) * ${delta};
      pitch = Math.max(-Math.PI / 2, Math.min(Math.PI / 2, pitch + (s.dpitch || 0) * ${delta}));
      // v7 index.js: [isn, delta, pitch, yaw, movDir, shoot, aim, jump (held), reload, crouch, scroll, swap, speedLmt, reset, interact]
      const input = [isn++, d, Math.round(+pitch.toFixed(3)*1000), Math.round(+yaw.toFixed(3)*1000), s.mv, 0, s.aim ? 1 : 0, s.jump ? 1 : 0, 0, s.crouch ? 1 : 0, 0, 0, 0, 0, 0];
      ticks.push([[d, input[3], input[2], input[4], input[7], input[9], input[6]], st()]);
      proc.call(p, input, G, false, false);
    }
    const end = st();
    for (const k in savedCfg) G.config[k] = savedCfg[k];
    ${sc.cls != null || sc.wep != null ? `p.setClass(G, savedCls, null, true); sw();` : ''}
    for (const k of Object.keys(saved)) p[k] = saved[k];
    p.velocity.copy(savedVel);
    return {meta, ticks, end};
  })()`);
  if (res.err) throw new Error(res.err);
  return res;
}

async function main() {
  if (process.argv[2] === '--list') { for (const k in SC) console.log(k, SC[k].map, SC[k].deltas.join(',')); return; }
  const which = process.argv[2] === 'all' ? Object.keys(SC) : [process.argv[2]];
  const out = process.argv[4] || path.join(KN, 'shared/tests/inputs');
  for (const name of which) if (!SC[name]) throw new Error('scenario: ' + Object.keys(SC).join(' '));
  const c = await connect();
  fs.mkdirSync(out, { recursive: true });
  for (const name of which) {
    const deltas = !process.argv[3] || process.argv[3] === 'all' ? SC[name].deltas : [+process.argv[3]];
    for (const delta of deltas) {
      const res = await record(c, name, delta);
      const file = path.join(out, `v7_${name}_${FPS[delta] || Math.round(1000 / delta)}fps.json`);
      fs.writeFileSync(file, JSON.stringify({ meta: res.meta, ticks: res.ticks }));
      const e = res.end;
      console.log(`${name}@${delta}ms: ${res.ticks.length} ticks, end x=${e[0].toFixed(3)} y=${e[1].toFixed(3)} z=${e[2].toFixed(3)} wjc=${e[14]} -> ${path.relative(KN, file)}`);
    }
  }
  c.close();
  process.exit(0);
}
main().catch(e => { console.error('FAILED', e.message); process.exit(1); });
