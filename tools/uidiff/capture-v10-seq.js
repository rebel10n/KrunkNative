#!/usr/bin/env node
// Krunker v10.0.2 FRAME-SEQUENCE references for tools/uidiff (doc/ref/krunker-v10.0.2/sequences/), taken from
// the same OFFLINE v10 sandbox as capture-v10.js (whose sandbox rules apply unchanged: fresh profile, closed
// proxy, Fetch-domain answers from the local mirror only, WebSocket throws, no beacons; see its header).
//
//   node tools/uidiff/capture-v10-seq.js [--out DIR] [--list] [name...]
//
// Every other reference is one frame with the CSS animations paused at 800 ms, so nothing tested timing.
// Here each sequence is a list of frames at origin + t (ms), where the origin is a state change (the
// first INIT, a death, the round end, a window opening) reached through v10's own code:
//   - JS time is CDP virtual time (Emulation.setVirtualTimePolicy pause / advance budget): setTimeout /
//     setInterval chains and Date.now / performance.now follow it exactly (budgets are exact to 0.1 ms),
//     and nothing runs between frames; requestAnimationFrame is driven too (RAF_DRIVER: one rAF per
//     16.7 ms step at the virtual timestamp), so the game loop, the overlay canvas and the kill feed /
//     popup lives run on the same clock (meta.json rafCalls = the rAFs between the first and last frame);
//   - Chromium's animation timeline does NOT follow virtual time (it ticks with real compositor frames),
//     so it is frozen (CDP Animation.setPlaybackRate 0: a transition that starts while a fetch holds
//     virtual time cannot run out in real time) and CSS animations / transitions / WAAPI are driven here:
//     each animation's start is the virtual time it was first seen (polled every 16.7 ms step, the native
//     renderer's frame), and before every step and every screenshot its currentTime is set to virtual
//     now - start (paused around the screenshot, so the compositor cannot move it). `restartAnims` puts
//     the animations already running at the origin to 0 (the infinite menu animations: the native page
//     starts them at clock 0);
//   - the origin is the virtual time of the first 16.7 ms step after which `origin` (a page expression)
//     is true, or the moment `trigger` returned when there is no `origin`: the native side
//     (client/src/ui/ui_scenario.c anim_origin) uses the same frame quantisation, and uidiff's timing check
//     allows +-1 frame.
// `bg: 'checker'` paints the transparent 3D canvas' place with the checker pattern of the native scenario
// background "checker" (32 px cells #8a8a8a / #343434, from the top-left corner: checkerPng() and
// ui_scenario.c checker_pixels() must stay identical), so backdrop-filter / blur show in the references.
//
// Output: <out>/<name>/<t>.png per frame, <name>/meta.json (times, origin, virtual clock of each frame,
// notes), <name>/rects.json (element rects at the last frame). Sequences the sandbox cannot reach are
// listed by --list with the reason and are never faked.
'use strict';
const fs = require('fs');
const path = require('path');
const net = require('net');
const zlib = require('zlib');
const V = require('./capture-v10.js');
const { sleep } = require('../kref/cdp.js');

let OUT = path.join(V.REPO, 'doc', 'ref', 'krunker-v10.0.2', 'sequences');
const FRAME_MS = 1000 / 60;
const log = (m) => { if (process.env.KN_CAPTURE_DEBUG) console.error('[capture-v10-seq] ' + m); };

// ---------------------------------------------------------------- checker background

const CHECKER = { cell: 32, light: [0x8a, 0x8a, 0x8a], dark: [0x34, 0x34, 0x34] };
function checkerPng(w, h) {
  const row = 1 + w * 3;
  const raw = Buffer.alloc(row * h);
  for (let y = 0; y < h; y++) {
    raw[y * row] = 0; // filter: none
    for (let x = 0; x < w; x++) {
      const c = ((Math.floor(x / CHECKER.cell) + Math.floor(y / CHECKER.cell)) % 2) ? CHECKER.dark : CHECKER.light;
      raw.set(c, y * row + 1 + x * 3);
    }
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2; // 8-bit RGB
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
// The hidden 3D canvas draws nothing: its WebGL context's draw calls become no-ops (swiftshader renders
// the scene at ~1 s a frame, and with one rAF per virtual step that would make a 4 s sequence take an hour)
const NO_3D = `(() => { const cv = ${V.GAME_CANVAS}; const gl = cv && (cv.getContext('webgl2') || cv.getContext('webgl'));
  if (gl) for (const k of ['drawElements', 'drawArrays', 'drawElementsInstanced', 'drawArraysInstanced', 'drawRangeElements', 'clear'])
    if (gl[k]) gl[k] = function () {};
  return !!gl; })()`;
// the page background: '#rrggbb' or 'checker'; the 3D canvas made transparent (capture-v10.js flatPage)
function backgroundJs(bg, w, h) {
  if (bg !== 'checker') return V.flatPage(bg);
  const url = 'data:image/png;base64,' + checkerPng(w, h).toString('base64');
  return `(() => { const cv = ${V.GAME_CANVAS}; if (cv) cv.style.opacity = '0';
    const d = document.documentElement.style; d.background = '#343434 url(${url}) 0 0 / ${w}px ${h}px no-repeat'; d.backgroundAttachment = 'fixed';
    document.body.style.background = 'transparent'; return 1; })()`;
}

// ---------------------------------------------------------------- virtual time + animation drive

class VClock {
  constructor(c) {
    this.c = c; this.waiter = null;
    c.on((m) => { if (m.method === 'Emulation.virtualTimeBudgetExpired' && this.waiter) { const f = this.waiter; this.waiter = null; f(); } });
  }
  pause() { return this.c.send('Emulation.setVirtualTimePolicy', { policy: 'pause' }); }
  async advance(ms, policy = 'advance') {
    const p = new Promise((res, rej) => { this.waiter = res; setTimeout(() => rej(new Error('virtual time budget did not expire')), 120000); });
    await this.c.send('Emulation.setVirtualTimePolicy', { policy, budget: Math.max(0.1, ms) });
    await p;
  }
  now() { return this.c.eval('performance.now()'); }
}
// window.__knVT: animation -> {start (virtual ms), ct0}; sync() sets every animation to its virtual time
const ANIM_DRIVER = `(() => { if (window.__knVT) return 0; const T = window.__knVT = { m: new Map() };
  T.adopt = (restart) => { const now = performance.now(); for (const a of document.getAnimations()) T.m.set(a, { start: now, ct0: restart ? 0 : (a.currentTime || 0) }); return T.m.size; };
  T.sync = (freeze) => { const now = performance.now(); let n = 0;
    for (const a of document.getAnimations()) { let r = T.m.get(a); if (!r) { r = { start: now, ct0: 0 }; T.m.set(a, r); }
      if (freeze) { r.wasRunning = a.playState === 'running'; if (r.wasRunning) a.pause(); }
      try { a.currentTime = r.ct0 + (now - r.start); n++; } catch (e) {} }
    return n; };
  T.thaw = () => { for (const a of document.getAnimations()) { const r = T.m.get(a); if (r && r.wasRunning) { const ct = a.currentTime; a.play(); a.currentTime = ct; r.wasRunning = false; } } return 1; };
  return 1; })()`;

// ---------------------------------------------------------------- sequences

const SEQ = {};
// opts: size, bg, times [ms], boot (virtual time from before the first page script), query, storage,
// preScript, settle (real-time ms after navigation), prepare(c) (real time), trigger(c, vc) (paused virtual
// time; vc.step(ms) advances it like a frame), origin (page expression), originTimeout (virtual ms, default
// 15000), restartAnims, frameJs (eval before each
// screenshot: pins), probe (page expression recorded per frame in meta.json: the evidence of what moved),
// note, unreachable (reason: listed, never captured)
function seq(name, opts) { SEQ[name] = Object.assign({ name, size: [1280, 720], bg: null, times: [0], settle: 10000 }, opts); }

const freePort = () => new Promise((res, rej) => {
  const s = net.createServer(); s.on('error', rej);
  s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
});

// requestAnimationFrame under virtual time: Chromium runs rAF callbacks on real compositor frames, so
// with virtual time a step of 16.7 ms rarely gets one (measured: 0 rAF in 60 steps) and the game loop
// (overlay canvas, kill feed / popup lives, the 3D renderer) would stand still. Before any page script
// rAF is wrapped: it passes through to the real one until __knRafVirtual(true), then callbacks queue
// and run exactly once per virtual step (__knRafFlush) with the virtual timestamp.
const RAF_DRIVER = `(() => { const real = window.requestAnimationFrame.bind(window), cancel = window.cancelAnimationFrame.bind(window);
  let virt = false, q = new Map(), id = 1e9; window.__knRafCalls = 0;
  window.requestAnimationFrame = function (f) { if (!virt) return real(f); const i = ++id; q.set(i, f); return i; };
  window.cancelAnimationFrame = function (i) { if (q.has(i)) q.delete(i); else cancel(i); };
  window.__knRafVirtual = (on) => { virt = !!on; return 1; };
  window.__knRafFlush = () => { const run = q; q = new Map(); const t = performance.now();
    for (const f of run.values()) { window.__knRafCalls++; try { f(t); } catch (e) { setTimeout(() => { throw e; }); } } return run.size; };
})()`;

async function runSequence(sq) {
  const [w, h] = sq.size;
  const port = await freePort();
  let vc = null;
  const o = { w, h, port, settle: sq.boot ? 0 : sq.settle, query: sq.query, storage: sq.storage, map: sq.map, mode: sq.mode,
    preScript: RAF_DRIVER + ';' + (sq.preScript || '') };
  const freeze = (c) => c.send('Animation.enable').then(() => c.send('Animation.setPlaybackRate', { playbackRate: 0 }));
  if (sq.boot) {
    o.beforeNavigate = async (c) => { vc = new VClock(c); await vc.pause(); await freeze(c); };
    o.preScript += ';window.__knRafVirtual(true)';
  }
  const { c, close, errors, blocked } = await V.openPage(o);
  const dir = path.join(OUT, sq.name);
  // one 60 Hz step (or a shorter one landing on a capture time): timers, then one rAF, then the CSS clock
  const policy = sq.boot ? 'pauseIfNetworkFetchesPending' : 'advance';
  const step = async (ms) => {
    await vc.advance(ms, policy);
    if (sq.boot) await c.eval(ANIM_DRIVER).catch(() => 0);
    await c.eval('window.__knRafFlush && window.__knRafFlush()').catch(() => 0);
    await c.eval('window.__knVT && window.__knVT.sync(false)').catch(() => 0);
  };
  vc && (vc.step = step);
  try {
    if (!vc) {
      vc = new VClock(c); vc.step = step;
      if (sq.prepare) await sq.prepare(c);
      await vc.pause(); await freeze(c);
      await c.eval('window.__knRafVirtual(true)');
    } else await c.eval('1').catch(() => {});
    if (sq.bg && !sq.boot) { await c.eval(backgroundJs(sq.bg, w, h)); await c.eval(NO_3D); }
    const v0 = await vc.now().catch(() => 0);
    if (!sq.boot) { await c.eval(ANIM_DRIVER); await c.eval(`window.__knVT.adopt(${!!sq.restartAnims})`); }
    if (sq.trigger) await sq.trigger(c, vc);
    let origin = null;
    if (!sq.origin) origin = await vc.now();
    const limit = (sq.originTimeout || 15000);
    // until the origin: 60 Hz steps (with boot, the page's own load: virtual time waits for fetches)
    while (origin === null) {
      await step(FRAME_MS);
      let hit = false;
      try { hit = await c.eval(sq.origin); } catch (e) { hit = false; }
      const now = await vc.now();
      if (hit) { origin = now; break; }
      if (now - v0 > limit) throw new Error(`${sq.name}: origin not reached in ${limit} virtual ms: ${sq.origin}`);
    }
    log(`${sq.name}: origin at ${origin.toFixed(1)} (virtual)`);
    if (sq.bg && sq.boot) { await c.eval(backgroundJs(sq.bg, w, h)); await c.eval(NO_3D); }
    fs.rmSync(dir, { recursive: true, force: true });
    fs.mkdirSync(dir, { recursive: true });
    const frames = {}, probes = {};
    const raf0 = await c.eval('window.__knRafCalls').catch(() => 0);
    for (const t of sq.times) {
      for (;;) {
        const now = await vc.now();
        const left = origin + t - now;
        if (left < 0.05) break;
        await step(Math.min(FRAME_MS, left));
      }
      if (sq.frameJs) await c.eval(sq.frameJs);
      await c.eval('window.__knVT.sync(true)');
      await c.shot(path.join(dir, `${t}.png`));
      frames[t] = +((await vc.now()) - origin).toFixed(1);
      if (sq.probe) probes[t] = await c.eval(sq.probe).catch((e) => String(e).slice(0, 200));
      await c.eval('window.__knVT.thaw()');
    }
    const rafCalls = (await c.eval('window.__knRafCalls').catch(() => 0)) - raf0;
    fs.writeFileSync(path.join(dir, 'rects.json'), JSON.stringify(await c.eval(V.RECTS)) + '\n');
    const meta = { name: sq.name, size: sq.size, background: sq.bg || 'page', times: sq.times, origin: sq.originNote || sq.origin || 'the trigger',
      restartAnims: !!sq.restartAnims, frames, rafCalls, probe: sq.probe ? { expr: sq.probe, values: probes } : undefined, note: sq.note || '', pageErrors: errors.length, requestsRefused: blocked.length,
      tool: 'tools/uidiff/capture-v10-seq.js' };
    fs.writeFileSync(path.join(dir, 'meta.json'), JSON.stringify(meta, null, 1) + '\n');
    console.error(`[capture-v10-seq] ${sq.name}: ${sq.times.length} frames (${errors.length} page errors, ${blocked.length} requests refused)`);
    if (process.env.KN_CAPTURE_ERRORS) for (const e of errors.slice(0, 20)) console.error('  ' + String(e).split('\n')[0]);
  } finally { close(); }
}

require('./capture-v10-seq-list.js')(seq, V);

if (require.main === module) (async () => {
  const args = process.argv.slice(2);
  const names = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--out') OUT = path.resolve(args[++i]);
    else if (args[i] === '--list') {
      for (const s of Object.values(SEQ)) console.log(`${s.name}${s.unreachable ? '  UNREACHABLE: ' + s.unreachable : '  [' + s.times.join(', ') + '] ms'}`);
      return;
    } else names.push(args[i]);
  }
  const list = names.length ? names.map((n) => { if (!SEQ[n]) throw new Error('unknown sequence ' + n); return SEQ[n]; })
    : Object.values(SEQ).filter((s) => !s.unreachable);
  let failed = 0;
  for (const sq of list) {
    if (sq.unreachable) { console.error(`[capture-v10-seq] ${sq.name}: not captured: ${sq.unreachable}`); continue; }
    try { await runSequence(sq); } catch (e) { failed++; console.error(`[capture-v10-seq] ${sq.name} FAILED: ${e.stack || e}`); }
  }
  process.exit(failed ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });

module.exports = { SEQ, checkerPng, CHECKER };
