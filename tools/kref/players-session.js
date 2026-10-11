// Start two headless Chromium clients of the real Krunker v3.1.1 game (A = mover,
// B = watcher) in the same online match, for third-person player rendering parity
// (players-ref.js records them). Leaves both browsers running; pids in $OUT/chrome-pids.txt.
//   URL=http://127.0.0.1:6841/ CDP_A=7841 CDP_B=7842 OUT=/tmp/kn-p3p-ref node tools/kref/players-session.js
'use strict';
const { CDP, sleep } = require('./cdp.js');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const http = require('http');

const URL = process.env.URL || 'http://127.0.0.1:6841/';
const OUT = process.env.OUT || '/tmp/kn-p3p-ref';
const PORTS = { A: +(process.env.CDP_A || 7841), B: +(process.env.CDP_B || 7842) };
const SHIM = fs.readFileSync(path.join(__dirname, 'shim.js'), 'utf8');
const up = (port) => new Promise(res => http.get(`http://127.0.0.1:${port}/json/version`, r => res(r.statusCode === 200)).on('error', () => res(false)));

async function launch(port, tag) {
  if (await up(port)) return;
  const p = spawn('chromium-browser', ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--window-size=960,540', `--remote-debugging-port=${port}`, `--user-data-dir=${OUT}/chrome-${tag}`, '--mute-audio',
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', 'about:blank'],
    { detached: true, stdio: 'ignore' });
  p.unref();
  fs.appendFileSync(`${OUT}/chrome-pids.txt`, p.pid + '\n');
  for (let i = 0; i < 40 && !(await up(port)); i++) await sleep(250);
}

async function join(tag) {
  const c = await CDP.connect(PORTS[tag]);
  await c.send('Page.enable'); await c.send('Runtime.enable');
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: SHIM });
  await c.setViewport(960, 540);
  await c.send('Page.navigate', { url: URL });
  await c.waitFor(`(()=>{const e=document.getElementById('instructions');return !!e && /PLAY/i.test(e.textContent||'')})()`, 90000);
  try { await c.eval('typeof checkTerms==="function" && checkTerms(1)'); } catch (e) { }
  await sleep(800);
  for (let i = 0; i < 15; i++) {
    await c.click(480, 270);
    try { await c.waitFor(`document.getElementById('inGameUI').style.display==='block'`, 4000); break; } catch (e) { }
  }
  await sleep(3000);
  console.log(tag, 'in game', await c.eval(`JSON.stringify({map:(document.getElementById('mapInfo')||{}).textContent,players:Game.players.list.map(p=>[p.sid,!!p.isYou,p.active,+p.x.toFixed(1),+p.y.toFixed(1),+p.z.toFixed(1)])})`));
  c.close();
}

(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  await launch(PORTS.A, 'A'); await launch(PORTS.B, 'B');
  await join('A');
  if (process.env.MAP) { // dev chat command (PERMS off): cycle the lobby to that map, then both join it
    const c = await CDP.connect(PORTS.A); await c.send('Runtime.enable');
    await c.eval(`(window.io.send("ct", "/map ${process.env.MAP}"), 1)`); c.close();
    await sleep(6000);
    await join('A');
  }
  await join('B');
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
