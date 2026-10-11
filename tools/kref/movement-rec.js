// Record Krunker v3.1.1 movement in the real client (singleplayer custom map, so the
// player can be placed freely) with exact per-input state, for KrunkNative movement /
// first-person feel parity.
//
//   node tools/kref/movement-rec.js <scenario> [out_dir]
//
// Expects a v3.1.1 server on $KREF_PORT (default 6780) and a headless Chromium with
// the pointer-lock shim on $KREF_CDP (default 7780), already in a singleplayer match
// (see movement-sp.js). Every procInputs call of the local player (recon=false) is
// recorded: the input array, wall time and the state after the call. Writes
//   <out>/rec-<scenario>.json         full ticks (for plots/tables)
//   <out>/test-<scenario>.json        proc_input_test format ([[inputs],[state]] per tick,
//                                     state BEFORE the input, like input_recorder.js)
'use strict';
const { CDP, sleep } = require('./cdp.js');
const fs = require('fs');

const PORT = process.env.KREF_PORT || '6780';
const CDP_PORT = +(process.env.KREF_CDP || 7780);
const OUT = process.argv[3] || '/tmp/kn-move-ref';
const K = {
  W: ['w', 'KeyW', 87], A: ['a', 'KeyA', 65], S: ['s', 'KeyS', 83], D: ['d', 'KeyD', 68],
  SPACE: [' ', 'Space', 32], X: ['x', 'KeyX', 88], SHIFT: ['Shift', 'ShiftLeft', 16], R: ['r', 'KeyR', 82],
};

// Scenario = start pose + timed actions. t in ms from record start.
// actions: ['down', key] | ['up', key] | ['mdown', 'left'|'right'] | ['mup', ...] | ['look', dyaw, dpitch]
const SCENARIOS = {
  idle: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 800, acts: [] },
  walk: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2400, acts: [[0, 'down', 'W'], [1500, 'up', 'W']] },
  walk_diag: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2400, acts: [[0, 'down', 'W'], [0, 'down', 'D'], [1500, 'up', 'W'], [1500, 'up', 'D']] },
  walk_back: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2000, acts: [[0, 'down', 'S'], [1200, 'up', 'S']] },
  jump: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 1600, acts: [[100, 'down', 'SPACE'], [160, 'up', 'SPACE']] },
  hold_jump: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2600, acts: [[100, 'down', 'SPACE'], [2300, 'up', 'SPACE']] },
  sprint_jump: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2600, acts: [[0, 'down', 'W'], [600, 'down', 'SPACE'], [660, 'up', 'SPACE'], [1800, 'up', 'W']] },
  crouch_slide: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2800, acts: [[0, 'down', 'W'], [600, 'down', 'SPACE'], [660, 'up', 'SPACE'], [800, 'down', 'SHIFT'], [1500, 'up', 'W'], [2300, 'up', 'SHIFT']] },
  crouch_walk: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2600, acts: [[0, 'down', 'SHIFT'], [300, 'down', 'W'], [1500, 'up', 'W'], [2000, 'up', 'SHIFT']] },
  air_strafe: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2200, acts: [[0, 'down', 'W'], [500, 'down', 'SPACE'], [560, 'up', 'SPACE'], [600, 'up', 'W'], [600, 'down', 'D'], [1400, 'up', 'D']] },
  ladder: { map: 'sandstorm', pos: [205, 34, 118], yaw: -Math.PI, dur: 3600, acts: [[0, 'down', 'W'], [3200, 'up', 'W']] },
  aim: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 1600, acts: [[100, 'mdown', 'right'], [800, 'mup', 'right']] },
  tap: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 1200, acts: [[100, 'mdown', 'left'], [140, 'mup', 'left']] },
  inspect: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 1500, acts: [[100, 'down', 'X'], [160, 'up', 'X']] },
  shoot: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 1600, acts: [[100, 'mdown', 'left'], [400, 'mup', 'left']] },
  look: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 1400, acts: [[100, 'look', 0.4, 0], [300, 'look', 0, 0.2]] },
  // viewmodel captures (sandstorm, 640x360 viewport): idle, walking, airborne, aimed, after a yaw flick
  feel_shots: { map: 'sandstorm', pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 4200, acts: [[300, 'shot', 'idle'], [400, 'down', 'W'], [1000, 'shot', 'walk'],
    [1100, 'down', 'SPACE'], [1160, 'up', 'SPACE'], [1350, 'shot', 'air'], [1400, 'up', 'W'], [2400, 'mdown', 'right'], [3000, 'shot', 'aim'], [3100, 'mup', 'right'],
    [3700, 'look', 0.25, 0], [3760, 'shot', 'flick']] },
  jump_pad: { map: 'kanji', pos: [75, -140, -270], yaw: -Math.PI / 2, dur: 2400, acts: [] },
};

const REC_STATE = `function(p){return {x:p.x,y:p.y,z:p.z,vx:p.velX,vy:p.velY,vz:p.velZ,g:p.onGround?1:0,lad:p.onLadder?1:0,
  cr:p.crouchVal,aim:p.aimVal,h:p.height,sl:p.slideTimer,stepVal:p.stepVal,stepChase:p.stepChase,stepDir:p.stepDir,
  landBobY:p.landBobY,landBobYR:p.landBobYR,jumpBobY:p.jumpBobY,jumpRot:p.jumpRot,jumpRotM:p.jumpRotM,bobAnimY:p.bobAnimY,bobAnimZ:p.bobAnimZ,
  leanAnimX:p.leanAnimX,leanAnimY:p.leanAnimY,leanAnimZ:p.leanAnimZ,recoilAnim:p.recoilAnim,recoilAnimY:p.recoilAnimY,recoilForce:p.recoilForce,
  recoilX:p.recoilX,recoilZ:p.recoilZ,recoilTweenY:p.recoilTweenY,recoilTweenYM:p.recoilTweenYM,recoilTweenZ:p.recoilTweenZ,recoilTweenR:p.recoilTweenR,inspectX:p.inspectX,inspecting:p.inspecting?1:0,
  rampFix:p.rampFix,yaw:p.yaw,pitch:p.pitch,zoom:(window.RENDER&&RENDER.camera)?RENDER.camera.zoom:null,
  camRX:(window.RENDER&&RENDER.camera)?RENDER.camera.rotation.x:null}}`;

async function main() {
  const name = process.argv[2];
  const sc = SCENARIOS[name];
  if (!sc) throw new Error('unknown scenario; one of ' + Object.keys(SCENARIOS).join(' '));
  const c = await CDP.connect(CDP_PORT, t => t.type === 'page' && t.url.includes(PORT));
  await c.send('Runtime.enable');
  // KREF_VIEWPORT=WxH: the emulated viewport only lasts while this CDP session is open.
  if (process.env.KREF_VIEWPORT) {
    const [w, h] = process.env.KREF_VIEWPORT.split('x').map(Number);
    await c.setViewport(w, h);
    await c.eval('window.dispatchEvent(new Event("resize"))');
    await sleep(1500);
  }
  const cur = await c.eval(`(Game.map.manager && Game.map.manager.name) || (Game.map.maps[Game.mapIndex]||{}).name || ''`);
  if (!new RegExp(sc.map, 'i').test(cur)) throw new Error(`map is "${cur}", scenario wants ${sc.map} (run movement-sp.js ${sc.map})`);
  const ok = await c.eval(`(()=>{const p=Game.players.list.find(p=>p.isYou);return !!(p&&p.active)})()`);
  if (!ok) throw new Error('no active local player');

  // Place the player and let it settle on the ground.
  await c.eval(`(()=>{const p=Game.players.list.find(p=>p.isYou);
    p.x=${sc.pos[0]};p.y=${sc.pos[1]};p.z=${sc.pos[2]};p.velX=p.velY=p.velZ=0;
    CONTROLS.object.rotation.y=${sc.yaw}; CONTROLS.xDr=${sc.yaw}; CONTROLS.pitchObj.rotation.x=0; CONTROLS.yDr=0; return 1})()`);
  await sleep(1200);

  await c.eval(`(()=>{const p=Game.players.list.find(p=>p.isYou);const st=${REC_STATE};
    window.__rec={on:true,t0:performance.now(),ticks:[]};
    if(!p.__origProc){p.__origProc=p.procInputs;
      p.procInputs=function(input,game,recon,moveLock){
        const r=window.__rec; let before=null;
        if(r&&r.on&&!recon) before=st(this);
        const out=p.__origProc.apply(this,arguments);
        if(r&&r.on&&!recon) r.ticks.push({t:performance.now()-r.t0,in:Array.from(input).slice(0,13),moveLock:!!moveLock,before,after:st(this)});
        return out;};}
    window.__recState=st; return 1})()`);

  const t0 = Date.now();
  const acts = sc.acts.slice().sort((a, b) => a[0] - b[0]);
  for (const a of acts) {
    const wait = a[0] - (Date.now() - t0);
    if (wait > 0) await sleep(wait);
    if (a[1] === 'down') await c.keyDown(...K[a[2]].slice(0, 2), K[a[2]][2]).then(() => c.send('Input.dispatchKeyEvent', { type: 'char', text: K[a[2]][0].length === 1 ? K[a[2]][0] : '' }).catch(() => { }));
    else if (a[1] === 'up') await c.keyUp(...K[a[2]]);
    else if (a[1] === 'mdown') await c.mouse('mousePressed', 640, 300, a[2]);
    else if (a[1] === 'mup') await c.mouse('mouseReleased', 640, 300, a[2]);
    else if (a[1] === 'shot') { await c.shot(`${OUT}/shot-${name}-${a[2]}.png`); }
    else if (a[1] === 'look') await c.eval(`(()=>{CONTROLS.object.rotation.y+=${a[2]};CONTROLS.pitchObj.rotation.x+=${a[3]};return 1})()`);
  }
  const rest = sc.dur - (Date.now() - t0);
  if (rest > 0) await sleep(rest);
  const rec = await c.eval(`(()=>{window.__rec.on=false;return JSON.stringify(window.__rec.ticks)})()`);
  const ticks = JSON.parse(rec);
  fs.mkdirSync(OUT, { recursive: true });
  fs.writeFileSync(`${OUT}/rec-${name}.json`, JSON.stringify({ scenario: name, map: sc.map, ticks }, null, 0));
  // proc_input_test format: [delta, yaw, pitch, movDir, jump, crouch, aim] / [x,y,z,vx,vy,vz,rampFix] (state before input)
  const test = ticks.map(t => [[t.in[1], t.in[2], t.in[3], t.in[4], t.in[7], t.in[8], t.in[6]],
    [t.before.x, t.before.y, t.before.z, t.before.vx, t.before.vy, t.before.vz, t.before.rampFix]]);
  fs.writeFileSync(`${OUT}/test-${name}.json`, JSON.stringify(test));
  const dts = ticks.map(t => t.in[1]);
  console.log(`${name}: ${ticks.length} ticks, delta ms min/avg/max ${Math.min(...dts)}/${(dts.reduce((a, b) => a + b, 0) / dts.length).toFixed(1)}/${Math.max(...dts)}`);
  c.close();
}
main().catch(e => { console.error('FAILED', e.message); process.exit(1); });
