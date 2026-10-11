// Record how the real Krunker v3.1.1 client renders ANOTHER player (third person), frame
// by frame, for KrunkNative player-mesh parity. Needs two clients in one online match
// (players-session.js): A moves through a scripted scenario, B watches A side-on and
// records per rendered frame A's interpolated state and the world matrices of its mesh
// nodes (objInstances / lowerBody / upperBody / held weaponMeshes[i] / weapon + arm
// meshes) plus screenshots at marked times.
//   node tools/kref/players-ref.js [out_dir]      (CDP_A / CDP_B as for players-session.js)
// Writes <out>/ref-frames.json and <out>/ref-<mark>.png.
'use strict';
const { CDP, sleep } = require('./cdp.js');
const fs = require('fs');

const OUT = process.argv[2] || '/tmp/kn-p3p-ref';
const PORTS = { A: +(process.env.CDP_A || 7841), B: +(process.env.CDP_B || 7842) };
const DIST = +(process.env.DIST || 22);
const K = { W: ['w', 'KeyW', 87], A: ['a', 'KeyA', 65], S: ['s', 'KeyS', 83], D: ['d', 'KeyD', 68], SHIFT: ['Shift', 'ShiftLeft', 16],
  R: ['r', 'KeyR', 82], X: ['x', 'KeyX', 88], SPACE: [' ', 'Space', 32], Q: ['q', 'KeyQ', 81], 1: ['1', 'Digit1', 49], 2: ['2', 'Digit2', 50], 3: ['3', 'Digit3', 51] };
// [t_ms, action, ...]: down/up key, mdown/mup button, look dyaw dpitch (radians, relative), shot name, end.
// The same file drives the native run (tools/kref/players-native.sh converts it to an input script).
const SCENARIO = JSON.parse(fs.readFileSync(process.env.SCENARIO || require('path').join(__dirname, 'players-scenario.json'), 'utf8'));

const RECORDER = `(()=>{
  const p = Game.players.list.find(q => !q.isYou && q.active);
  if (!p) return 'no remote';
  const M = o => (o.updateMatrixWorld(true), Array.from(o.matrixWorld.elements).map(v => +v.toFixed(5)));
  const box = (o, rel) => { if (!o || !o.geometry) return null; o.geometry.computeBoundingBox(); const b = o.geometry.boundingBox;
    const inv = new o.matrixWorld.constructor().getInverse ? new o.matrixWorld.constructor().getInverse(rel.matrixWorld) : rel.matrixWorld.clone().invert();
    const m = inv.multiply(o.matrixWorld); const e = m.elements; let mn = [1e9,1e9,1e9], mx = [-1e9,-1e9,-1e9];
    for (let i = 0; i < 8; i++) { const v = [i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z];
      const w = [0,1,2].map(k => e[k] * v[0] + e[4 + k] * v[1] + e[8 + k] * v[2] + e[12 + k]);
      for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], w[k]); mx[k] = Math.max(mx[k], w[k]); } }
    return [mn.map(v => +v.toFixed(3)), mx.map(v => +v.toFixed(3))]; };
  window.__rec = { on: true, frames: [], last: -1, p };
  if (!RENDER.__p3pWrapped) {
    const orig = RENDER.render; RENDER.__p3pWrapped = true;
    RENDER.render = function () {
      const r = window.__rec, ft = document.timeline.currentTime;
      if (r && r.on && ft !== r.last && r.p.objInstances) { r.last = ft; const q = r.p; const wm = q.weaponMeshes[q.weaponIndex];
        r.frames.push({ t: performance.now(), x: q.x, y: q.y, z: q.z, yaw: q.yaw, pitch: q.pitch, cr: q.crouchVal, step: q.stepVal, chase: q.stepChase,
          g: q.onGround ? 1 : 0, wi: q.weaponIndex, aim: q.aimVal, x2: q.x2, z2: q.z2, xd2: q.xDir2, yd2: q.yDir2, dt: q.dt, h: q.height,
          root: M(q.objInstances), lower: M(q.lowerBody), upper: M(q.upperBody), wm: wm ? M(wm) : null,
          legs: q.legMeshes.map(l => +l.rotation.x.toFixed(4)), legVis: q.legMeshes.map(l => l.visible ? 1 : 0),
          kids: wm ? wm.children.map(k => ({ m: M(k), box: box(k, wm), vis: k.visible ? 1 : 0, n: k.geometry ? (k.geometry.attributes && k.geometry.attributes.position ? k.geometry.attributes.position.count : 0) : -1 })) : null });
        if (window.__look) { CONTROLS.camLookAt(q.x, q.y + 6, q.z); }
      }
      if (this.fpsCamera) this.fpsCamera.layers.set(31); // no first-person weapon in B's view
      return orig.apply(this, arguments);
    };
  }
  for (const e of document.body.children) if (e.tagName !== 'CANVAS') e.style.visibility = 'hidden';
  return 'ok sid ' + p.sid;
})()`;

(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  const A = await CDP.connect(PORTS.A), B = await CDP.connect(PORTS.B);
  for (const c of [A, B]) { await c.send('Runtime.enable'); await c.send('Page.enable'); }
  await A.setViewport(320, 180); // A only drives; keep its rendering cheap
  // Teleport B DIST units from A along +x of A, then face A's profile: A looks along -z (yaw 0) or as set below.
  const a = await A.eval(`(()=>{const p=Game.players.list.find(p=>p.isYou);return [p.x,p.y,p.z]})()`);
  const env = (k, d) => (process.env[k] != null && process.env[k] !== "" ? +process.env[k] : d);
  const ax = env("AX", a[0]), ay = env("AY", a[1]), az = env("AZ", a[2]);
  await A.eval(`(window.io.send("tp", ${ax}, ${ay} + 9.5, ${az}), 1)`);
  await B.eval(`(window.io.send("tp", ${ax + DIST}, ${ay} + 9.5, ${az}), 1)`);
  await sleep(1500);
  const yawA = +(process.env.YAW_A || 0); // 0 faces -z: B (at +x) sees A's left side
  await A.eval(`(()=>{CONTROLS.object.rotation.y=${yawA};CONTROLS.xDr=${yawA};CONTROLS.pitchObj.rotation.x=0;CONTROLS.yDr=0;return 1})()`);
  await B.eval(`(()=>{CONTROLS.object.rotation.y=Math.PI/2;CONTROLS.xDr=Math.PI/2;CONTROLS.pitchObj.rotation.x=0;CONTROLS.yDr=0;return 1})()`);
  await sleep(1500);
  // B's camera: look at A each frame (CONTROLS.camLookAt) is off by default; we keep B still and level.
  console.log('B recorder', await B.eval(RECORDER));
  const t0 = Date.now(), marks = [];
  for (const ev of SCENARIO) {
    const wait = ev[0] - (Date.now() - t0); if (wait > 0) await sleep(wait);
    const [t, act, arg] = ev;
    if (act === 'down') await A.keyDown(...K[arg]);
    else if (act === 'up') await A.keyUp(...K[arg]);
    else if (act === 'mdown') await A.mouse('mousePressed', 160, 90, arg);
    else if (act === 'mup') await A.mouse('mouseReleased', 160, 90, arg);
    else if (act === 'look') await A.eval(`(()=>{CONTROLS.object.rotation.y+=${arg};CONTROLS.pitchObj.rotation.x+=${ev[3]};return 1})()`);
    else if (act === 'shot') { const n = await B.eval('window.__rec.frames.length'); marks.push({ name: arg, t: Date.now() - t0, frame: n }); await B.shot(`${OUT}/ref-${arg}.png`); }
    else if (act === 'end') break;
  }
  const frames = JSON.parse(await B.eval('(()=>{window.__rec.on=false;return JSON.stringify(window.__rec.frames)})()'));
  fs.writeFileSync(`${OUT}/ref-frames.json`, JSON.stringify({ marks, frames }));
  console.log('frames', frames.length, 'marks', marks.map(m => m.name + '@' + m.frame).join(' '));
  A.close(); B.close(); process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
