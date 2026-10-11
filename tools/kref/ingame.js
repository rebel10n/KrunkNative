// Drive the real Krunker v3.1.1 client (served by the local game server) into a
// match with headless Chromium over CDP and capture HUD reference screenshots.
'use strict';
const { CDP, sleep } = require('./cdp.js');
const fs = require('fs');
const { spawn } = require('child_process');

const OUT = '/tmp/krunker-ref';
const URL = process.env.URL || 'http://127.0.0.1:6768/';
const SHIM = fs.readFileSync('/tmp/kref-tools/shim.js', 'utf8');
const CX = 640, CY = 360;
const log = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

function launchChrome(port, profile, w = 1280, h = 720) {
  const args = ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    `--window-size=${w},${h}`, `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
    '--autoplay-policy=no-user-gesture-required', '--mute-audio', '--disable-background-timer-throttling',
    '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', 'about:blank'];
  const p = spawn('chromium-browser', args, { detached: true, stdio: ['ignore', fs.openSync(`${OUT}/chrome-${port}.log`, 'a'), fs.openSync(`${OUT}/chrome-${port}.log`, 'a')] });
  p.unref();
  return p.pid;
}

async function openClient(port, tag, w = 1280, h = 720) {
  const c = await CDP.connect(port);
  c.tag = tag;
  await c.send('Page.enable'); await c.send('Runtime.enable');
  c.logs = [];
  c.on(m => {
    if (m.method === 'Runtime.consoleAPICalled') c.logs.push(m.params.args.map(a => a.value !== undefined ? String(a.value) : a.description).join(' ').slice(0, 300));
    if (m.method === 'Runtime.exceptionThrown') c.logs.push('EXC ' + JSON.stringify(m.params.exceptionDetails).slice(0, 400));
  });
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: SHIM });
  await c.setViewport(w, h);
  await c.send('Page.navigate', { url: URL });
  await c.waitFor(`(()=>{const e=document.getElementById('instructions');return !!e && /PLAY/i.test(e.textContent||'')})()`, 90000);
  try { await c.eval('typeof checkTerms==="function" && checkTerms(1)'); } catch (e) { }
  await sleep(500);
  log(tag, 'menu ready');
  return c;
}

const probe = `(()=>{const g=id=>document.getElementById(id);const d=id=>{const e=g(id);return e?e.style.display||getComputedStyle(e).display:'-'};
  return {igu:d('inGameUI'),instr:d('instructionHolder'),menu:d('menuHolder'),kc:d('killCardHolder'),end:d('endUI'),lock:!!document.pointerLockElement,
  hp:(g('healthValue')||{}).textContent,ammo:(g('ammoVal')||{}).textContent,feed:(g('killFeed')||{childNodes:[]}).childNodes.length,timer:(g('timerVal')||{}).textContent}})()`;

async function spawnIn(c) {
  // Click the CLICK TO PLAY area exactly as a user would; the shim makes the
  // pointer lock succeed so pointerlockchange -> enterGame() runs.
  for (let i = 0; i < 15; i++) {
    await c.click(CX, CY);
    try { await c.waitFor(`document.getElementById('inGameUI').style.display==='block'`, 4000); break; } catch (e) { log(c.tag, 'spawn click retry', i); }
  }
  await c.waitFor(`document.getElementById('inGameUI').style.display==='block'`, 10000);
  await sleep(3500); // let the map stream in and the first server sync land
  const st = await c.eval(probe);
  log(c.tag, 'in game', JSON.stringify(st));
  return st;
}

async function capture(c, state, extra) {
  const png = `${OUT}/ingame-${state}.png`;
  await c.shot(png);
  await c.dom(`${OUT}/ingame-${state}.html`);
  const st = await c.eval(probe);
  log(c.tag, 'captured', state, JSON.stringify(st), extra || '');
  fs.appendFileSync(`${OUT}/ingame-captures.log`, `${state}\t${c.tag}\treal-match\t${JSON.stringify(st)}\n`);
}

async function main() {
  fs.mkdirSync(OUT, { recursive: true });
  const pids = [];
  const http = require('http');
  const up = (port) => new Promise(res => http.get(`http://127.0.0.1:${port}/json/version`, r => res(r.statusCode === 200)).on('error', () => res(false)));
  if (!(await up(9333))) pids.push(launchChrome(9333, '/tmp/kref-chrome-A'));
  if (!(await up(9334))) pids.push(launchChrome(9334, '/tmp/kref-chrome-B'));
  fs.appendFileSync(`${OUT}/chrome-pids.txt`, pids.join('\n') + (pids.length ? '\n' : ''));
  log('chromes', pids.join(','));
  const A = await openClient(9333, 'A');
  await capture(A, 'menu-before-play');
  await spawnIn(A);

  // --- plain spawn
  await capture(A, 'plain');

  // --- scoped (right mouse held)
  await A.mouse('mousePressed', CX, CY, 'right');
  await sleep(600);
  await capture(A, 'scoped');
  await A.mouse('mouseReleased', CX, CY, 'right');
  await sleep(400);

  // --- firing
  await A.mouse('mousePressed', CX, CY, 'left');
  await sleep(120);
  await capture(A, 'firing');
  await A.mouse('mouseReleased', CX, CY, 'left');
  await sleep(200);
  await capture(A, 'after-fire');

  // --- reloading (R)
  await A.key('r', 'KeyR', 82);
  await sleep(350);
  await capture(A, 'reloading');
  await sleep(2500);

  // --- scoreboard (Tab held)
  await A.keyDown('Tab', 'Tab', 9);
  await sleep(500);
  await capture(A, 'scoreboard');
  await A.keyUp('Tab', 'Tab', 9);
  await sleep(300);
  log('A after tab', JSON.stringify(await A.eval(probe)));
  if ((await A.eval(probe)).igu !== 'block') { await A.click(CX, CY); await sleep(1500); }

  // --- chat (Enter, type, screenshot, send)
  await A.key('Enter', 'Enter', 13);
  await sleep(300);
  for (const ch of 'gg') await A.key(ch, 'Key' + ch.toUpperCase(), ch.toUpperCase().charCodeAt(0));
  await sleep(200);
  await capture(A, 'chat-open');
  await A.key('Enter', 'Enter', 13);
  await sleep(600);
  await capture(A, 'chat-sent');

  // --- ESC pause: the browser would drop pointer lock; the shim does the same.
  await A.eval('document.exitPointerLock()');
  await sleep(600);
  await capture(A, 'pause-esc');
  await A.click(CX, CY);
  await sleep(1500);

  // --- second client for a real kill
  const B = await openClient(9334, 'B');
  await spawnIn(B);
  await sleep(1500);
  const who = await A.eval(`JSON.stringify(Game.players.list.map(p=>({sid:p.sid,alias:p.alias,you:!!p.isYou,x:p.x,y:p.y,z:p.z,h:p.height,hp:p.health,active:p.active})))`);
  log('players seen by A', who);

  let killed = false;
  for (let round = 0; round < 6 && !killed; round++) {
    // A: aim at B each shot (positions come from the server's k packets), fire.
    for (let i = 0; i < 40 && !killed; i++) {
      const aimed = await A.eval(`(()=>{const t=Game.players.list.find(p=>!p.isYou&&p.active);if(!t)return null;CONTROLS.camLookAt(t.x,t.y+t.height/1.5,t.z);return [t.x,t.y,t.z,t.health]})()`);
      if (!aimed) { log('no target yet'); await sleep(500); continue; }
      await A.mouse('mousePressed', CX, CY, 'left'); await sleep(60); await A.mouse('mouseReleased', CX, CY, 'left');
      await sleep(130);
      const hm = await A.eval(`document.getElementById('hitmarker').style.display`);
      if (hm === 'block' || hm === 'inline-block') {
        await capture(A, 'hitmarker', 'target=' + JSON.stringify(aimed));
      }
      const fb = await A.eval(`document.getElementById('killFeed').childNodes.length`);
      if (fb > 0) { killed = true; await capture(A, 'killfeed', 'target=' + JSON.stringify(aimed)); }
      if (i % 10 === 9) { await A.key('r', 'KeyR', 82); await sleep(1800); }
    }
    if (!killed) {
      // reposition: noclip toward B is fiddly; instead make A respawn-free move forward a bit and retry
      await A.key('w', 'KeyW', 87, { hold: 400 });
      await sleep(300);
    }
  }
  if (killed) {
    // B should be on the death/kill-card screen now.
    try {
      await B.waitFor(`document.getElementById('killCardHolder').style.display==='block'||document.getElementById('menuHolder').style.display==='block'`, 8000);
      await sleep(300);
      await capture(B, 'death');
      await sleep(2500);
      await capture(B, 'death-respawn-menu');
    } catch (e) { log('B death screen not seen', e.message, JSON.stringify(await B.eval(probe))); }
  } else {
    log('NO KILL achieved; falling back later');
  }

  // --- 1920x1080 plain
  await A.setViewport(1920, 1080);
  await A.eval('window.dispatchEvent(new Event("resize"))');
  await sleep(1500);
  if ((await A.eval(probe)).igu !== 'block') { await A.click(960, 540); await sleep(2500); }
  await capture(A, 'plain-1080p');
  await A.setViewport(1280, 720);
  await A.eval('window.dispatchEvent(new Event("resize"))');
  await sleep(800);

  const timer = await A.eval(`({t:(document.getElementById('timerVal')||{}).textContent, gt:Game.gameTimer, cfg:Game.config&&Game.config.gameTime})`);
  log('round timer', JSON.stringify(timer));
  fs.writeFileSync(`${OUT}/A-console.log`, A.logs.join('\n'));
  fs.writeFileSync(`${OUT}/B-console.log`, B.logs.join('\n'));
  A.close(); B.close();
  log('done; browsers left running for the end-of-round waiter');
}

main().catch(e => { console.error('FAILED', e); process.exit(1); });
