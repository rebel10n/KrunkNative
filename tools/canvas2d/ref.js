#!/usr/bin/env node
// Builds a self-contained HTML page that replays a tools/canvas2d scene with the real v7.1.4
// GAME.OVERLAY implementation (extracted at build time from the read-only reference
// src/libs/createKrunkScriptApiContext.js) on a browser Canvas 2D context, for run.sh to
// screenshot in headless Chromium. Return values are written into <pre id="out">.
//
//   node tools/canvas2d/ref.js <scene.txt> <out.html> [v7 root]
'use strict';
const fs = require('fs');
const path = require('path');

const [scenePath, outPath, v7Arg] = process.argv.slice(2);
if (!scenePath || !outPath) {
  console.error('usage: ref.js <scene.txt> <out.html> [v7 root]');
  process.exit(1);
}
const repo = path.resolve(__dirname, '..', '..');
const v7 = v7Arg || '/home/user/jails/kru/src/Krunker_v7.1.4';

// The OVERLAY object literal, verbatim.
const api = fs.readFileSync(path.join(v7, 'src/libs/createKrunkScriptApiContext.js'), 'utf8');
const start = api.indexOf('OVERLAY: {');
if (start < 0) throw new Error('OVERLAY not found in the v7 API context');
let depth = 0, end = -1;
for (let i = api.indexOf('{', start); i < api.length; i++) {
  if (api[i] === '{') depth++;
  else if (api[i] === '}' && --depth === 0) { end = i + 1; break; }
}
const overlaySrc = api.slice(start + 'OVERLAY: '.length, end);

// Scene: same grammar as canvas2d_test.c
const scene = { w: 800, h: 600, scale: 1, dpr: 1, bg: '#000000', images: {}, calls: [] };
let outside = false;
fs.readFileSync(scenePath, 'utf8').split('\n').forEach((raw, i) => {
  const line = raw.trim();
  if (!line || line.startsWith('//')) return;
  if (line.startsWith('@')) {
    const [key, ...rest] = line.slice(1).split(/\s+/);
    if (key === 'size') { scene.w = +rest[0]; scene.h = +rest[1]; }
    else if (key === 'scale') scene.scale = +rest[0];
    else if (key === 'dpr') scene.dpr = +rest[0];
    else if (key === 'font') scene.font = rest.join(' ');
    else if (key === 'background') scene.bg = rest[0];
    else if (key === 'outside') outside = true;
    else if (key === 'image') {
      const file = path.resolve(repo, rest[1]);
      scene.images[rest[0]] = 'data:image/png;base64,' + fs.readFileSync(file).toString('base64');
    }
    return;
  }
  const m = /^(\S+)\s*(.*)$/.exec(line);
  const args = [];
  const re = /"((?:[^"\\]|\\.)*)"|([^\s,]+)/g;
  let t;
  while ((t = re.exec(m[2]))) {
    if (t[1] !== undefined) args.push(JSON.parse('"' + t[1] + '"'));
    else if (t[2] === 'true' || t[2] === 'false') args.push(t[2] === 'true');
    else if (t[2] === 'null') args.push(null);
    else if (t[2] === 'undefined') args.push(undefined);
    else if (t[2] === '{}') args.push({});
    else args.push(Number(t[2]));
  }
  scene.calls.push({ line: i + 1, method: m[1], args, outside });
});

const font = fs.readFileSync(path.join(repo, 'assets/css/fonts/font2.ttf')).toString('base64');
const html = `<!doctype html><html><head><meta charset="utf-8"><style>
@font-face { font-family: "GameFont"; src: url(data:font/ttf;base64,${font}); }
html, body { margin: 0; padding: 0; overflow: hidden; background: ${scene.bg}; }
canvas { position: absolute; left: 0; top: 0; }
pre { position: absolute; left: 0; top: ${scene.h}px; margin: 0; }
</style></head><body><canvas id="c"></canvas><pre id="out"></pre><script>
const SCENE = ${JSON.stringify(scene, (k, v) => (v === undefined ? '__undef__' : v))};
for (const c of SCENE.calls) c.args = c.args.map(a => (a === '__undef__' ? undefined : a));
const canvas = document.getElementById('c');
// @dpr: a device-resolution backing store under the v7 canvas-px coordinates, which is what
// KN renders (v7 itself sizes the canvas in CSS px and lets the browser upscale it).
canvas.width = SCENE.w * SCENE.dpr; canvas.height = SCENE.h * SCENE.dpr;
canvas.style.width = SCENE.w + 'px'; canvas.style.height = SCENE.h + 'px';
const images = {};
const UTILS = {
  isNumber: p => p !== null && p !== undefined && typeof p == 'number' && !isNaN(p) && isFinite(p),
  isString: p => p && typeof p == 'string',
  isHexColor: p => /^#([0-9A-F]{3}){1,2}$/i.test(p),
  userAssetsUrl: p => 'asset:' + p,
};
const p17042 = {
  canvas: { width: SCENE.w, height: SCENE.h }, ctx: canvas.getContext('2d'), scale: SCENE.scale,
  HELPER: { loadImg: url => images[url.slice(6, -12)] || { isReady: false } },
};
const OVERLAY = ${overlaySrc};
async function run() {
  await document.fonts.load('10px GameFont');
  for (const id in SCENE.images) {
    const img = new Image();
    img.src = SCENE.images[id];
    await img.decode();
    img.isReady = true;
    images[id] = img;
  }
  const ctx = p17042.ctx, s = SCENE.scale, out = [];
  ctx.scale(SCENE.dpr, SCENE.dpr);
  if (SCENE.font) ctx.font = SCENE.font;
  // overlay.render: clear in overlay units, then the script render under ctx.scale(scale)
  ctx.save(); ctx.scale(s, s); ctx.clearRect(0, 0, SCENE.w / s, SCENE.h / s); ctx.restore();
  let inRender = false;
  for (const c of SCENE.calls) {
    if (!c.outside && !inRender) { ctx.save(); ctx.scale(s, s); inRender = true; }
    else if (c.outside && inRender) { ctx.restore(); inRender = false; }
    let r;
    try { r = OVERLAY[c.method](...c.args); } catch (e) { r = 'error'; }
    if (typeof r == 'number') out.push(c.line + ': ' + c.method + ' -> ' + r.toFixed(3));
    else if (r === 'error') out.push(c.line + ': ' + c.method + ' -> error');
    else if (r && typeof r == 'object') out.push(c.line + ': ' + c.method + ' -> ' + r.width.toFixed(3) + ' x ' + r.height.toFixed(3));
  }
  if (inRender) ctx.restore();
  document.getElementById('out').textContent = out.join('\\n');
  document.title = 'done';
}
run();
</script></body></html>`;
fs.writeFileSync(outPath, html);
