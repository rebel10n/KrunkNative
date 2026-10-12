// Deterministic Krunker v3.1.1 movement reference: calls the real client's
// player.procInputs synchronously (inside one Runtime.evaluate, so the game loop
// cannot interleave) with synthetic inputs at a fixed delta, then restores the
// player. Gives frame-rate-controlled trajectories (30/60/144 fps) from the actual
// v3.1.1 physics without depending on the headless browser's frame rate.
//
//   node tools/kref/movement-synth.js <scenario> <delta_ms> [out_dir]
//
// Writes <out>/synth-<scenario>-<delta>.json (ticks with input + state) and
// <out>/test-synth-<scenario>-<delta>.json (proc_input_test format).
'use strict';
const { CDP } = require('./cdp.js');
const fs = require('fs');

const PORT = process.env.KREF_PORT || '6780';
const CDP_PORT = +(process.env.KREF_CDP || 7780);
const OUT = process.argv[4] || '/tmp/kn-move-ref';

// movDirs index (v3.1.1 config.movDirs[i] = -PI + (i + 1) * PI / 4): W=1, S=5, D=3, A=7, WD=2, WA=0
// Each scenario: start pose + function(t_ms) -> {mv, jump (held), crouch, aim, dyaw}
const SC = {
  walk: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2400, f: t => ({ mv: t < 1500 ? 1 : -1 }) },
  walk_diag: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2400, f: t => ({ mv: t < 1500 ? 2 : -1 }) },
  jump: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 1600, f: t => ({ mv: -1, jump: t >= 100 && t < 160 }) },
  hold_jump: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2600, f: t => ({ mv: -1, jump: t >= 100 && t < 2300 }) },
  sprint_jump: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2600, f: t => ({ mv: t < 1800 ? 1 : -1, jump: t >= 600 && t < 660 }) },
  crouch_slide: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2800, f: t => ({ mv: t < 1500 ? 1 : -1, jump: t >= 600 && t < 660, crouch: t >= 800 && t < 2300 }) },
  crouch_walk: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2600, f: t => ({ mv: t >= 300 && t < 1500 ? 1 : -1, crouch: t < 2000 }) },
  aim_walk: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2400, f: t => ({ mv: t < 1600 ? 1 : -1, aim: t >= 200 && t < 1200 }) },
  turn_jump: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 2200, f: t => ({ mv: t < 1400 ? (t < 600 ? 1 : 3) : -1, jump: t >= 500 && t < 560, dyaw: t >= 600 && t < 1200 ? 0.002 : 0 }) },
  look: { pos: [0, 13, 0], yaw: -Math.PI / 2, dur: 1400, f: t => ({ mv: t >= 800 && t < 1100 ? 3 : -1, dyaw: t >= 100 && t < 400 ? 0.003 : t >= 400 && t < 500 ? -0.004 : 0, dpitch: t >= 500 && t < 700 ? -0.002 : 0 }) },
  ladder: { pos: [205, 33.5, 118], yaw: -Math.PI, dur: 3600, f: t => ({ mv: t < 3200 ? 1 : -1 }) },
  jump_pad: { pos: [75, -140, -270], yaw: -Math.PI / 2, dur: 2400, f: t => ({ mv: -1 }) },
  // soul_sanctum: drop onto the spawn disk (cylinder top y 3), stand, walk, stop on it
  soul_spawn: { pos: [1, 4, 0], yaw: -Math.PI / 2, dur: 1600, f: t => ({ mv: t >= 300 && t < 800 ? 1 : -1 }) },
};

async function main() {
  const name = process.argv[2], delta = +(process.argv[3] || 17);
  const sc = SC[name];
  if (!sc) throw new Error('scenario: ' + Object.keys(SC).join(' '));
  const c = await CDP.connect(CDP_PORT, t => t.type === 'page' && t.url.includes(PORT));
  const res = await c.eval(`(()=>{
    const p = Game.players.list.find(p => p.isYou);
    if (!p || !p.active) return {err: 'no player'};
    const proc = p.__origProc || p.procInputs;
    const saved = {}; for (const k of Object.keys(p)) { const v = p[k]; if (v === null || typeof v !== 'object' && typeof v !== 'function') saved[k] = v; }
    const f = ${sc.f.toString()};
    const st = () => ({x:p.x,y:p.y,z:p.z,vx:p.velX,vy:p.velY,vz:p.velZ,g:p.onGround?1:0,lad:p.onLadder?1:0,cr:p.crouchVal,aim:p.aimVal,h:p.height,sl:p.slideTimer,
      rampFix:p.rampFix==null?null:p.rampFix,stepVal:p.stepVal,stepChase:p.stepChase,bobAnimY:p.bobAnimY,bobAnimZ:p.bobAnimZ,leanAnimX:p.leanAnimX,leanAnimY:p.leanAnimY,leanAnimZ:p.leanAnimZ});
    p.x=${sc.pos[0]}; p.y=${sc.pos[1]}; p.z=${sc.pos[2]}; p.velX=p.velY=p.velZ=0; p.onGround=true; p.crouchVal=0; p.aimVal=1; p.slideTimer=0; p.rampFix=null;
    p.yaw=${sc.yaw}; p.pitch=0; p.didJump=false; p.jumpTimer=0; p.canSlide=true; p.height=${11};
    p.stepVal=p.stepChase=0; p.stepDir=1; p.bobAnimY=p.bobAnimZ=0; p.leanAnimX=p.leanAnimY=p.leanAnimZ=0; p.recoilX=p.recoilZ=0;
    let yaw = ${sc.yaw}, pitch = 0, wasJump = false, isn = 1;
    const ticks = [];
    // settle onto the ground with idle inputs (recorded too: the native replay starts at the same pose)
    for (let i = 0; i < 30; i++) {
      const input = [isn++, ${delta}, Math.round(yaw*1000), 0, -1, 0, 0, 0, 0, 0, 0, 0, 0];
      const before = st();
      proc.call(p, input, Game, false, false);
      ticks.push({t: (i - 30) * ${delta}, in: input, before, after: st()});
    }
    for (let t = 0; t < ${sc.dur}; t += ${delta}) {
      const s = f(t);
      yaw += (s.dyaw || 0) * ${delta};
      pitch = Math.max(-Math.PI / 2, Math.min(Math.PI / 2, pitch + (s.dpitch || 0) * ${delta}));
      const jumpPress = !!s.jump && !wasJump; wasJump = !!s.jump; // CONTROLS.didPressed
      const input = [isn++, ${delta}, Math.round(+yaw.toFixed(3)*1000), Math.round(+pitch.toFixed(3)*1000), s.mv, 0, s.aim ? 1 : 0, jumpPress ? 1 : 0, s.crouch ? 1 : 0, 0, 0, 0, 0];
      const before = st();
      proc.call(p, input, Game, false, false);
      ticks.push({t, in: input, before, after: st()});
    }
    for (const k of Object.keys(saved)) p[k] = saved[k];
    return {ticks};
  })()`);
  if (res.err) throw new Error(res.err);
  fs.mkdirSync(OUT, { recursive: true });
  fs.writeFileSync(`${OUT}/synth-${name}-${delta}.json`, JSON.stringify({ scenario: name, delta, ticks: res.ticks }));
  const test = res.ticks.map(t => [[t.in[1], t.in[2], t.in[3], t.in[4], t.in[7], t.in[8], t.in[6]],
    [t.before.x, t.before.y, t.before.z, t.before.vx, t.before.vy, t.before.vz, t.before.rampFix,
     t.before.stepVal, t.before.stepChase, t.before.bobAnimY, t.before.bobAnimZ, t.before.leanAnimX, t.before.leanAnimY, t.before.leanAnimZ]]);
  fs.writeFileSync(`${OUT}/test-synth-${name}-${delta}.json`, JSON.stringify(test));
  const last = res.ticks[res.ticks.length - 1].after;
  console.log(`${name}@${delta}ms: ${res.ticks.length} ticks, end x=${last.x.toFixed(3)} y=${last.y.toFixed(3)} z=${last.z.toFixed(3)}`);
  c.close();
}
main().catch(e => { console.error('FAILED', e.message); process.exit(1); });
