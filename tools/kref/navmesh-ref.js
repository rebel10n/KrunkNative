// Krunker v7.1.4 `show_navmeshes` capture (the dev console's nav mesh visualizer,
// mod/devConsole.ts buildGeneratedNavOverlay) at a pinned camera pose, to compare with
// client_bin's overlay (client/src/nav_overlay.c, doc/console.md).
//
//   node tools/kref/navmesh-ref.js <map.json> "<x,y,z,pitch,yaw>" <out.png> [fovDeg=90]
//
// Needs a v7.1.4 server on $KREF_PORT (default 6781) run from a COPY of the v7 tree (its npm
// server writes accounts.db, server.pid and public/js into the tree; node_modules and the big
// public/ folders can be symlinks):  cd <copy> && PORT=<port> LOBBY_COUNT=1 npm run server
// Headless Chromium with CDP on $KREF_CDP (default 7781) is started when absent (profile
// $KREF_PROFILE, default ~/.cache/kn-navmesh-ref/chrome).
//
// The map is loaded as the offline sandbox's custom map (localStorage custMap), the local player
// enters the game, `show_navmeshes` runs through the console's own form, and RENDER.render is
// wrapped so the camera sits at the pose (YXZ: pitch about X, yaw about Y; the first-person
// layer hidden) before every frame. Everything but the canvas is hidden. The console's reply
// ("nav: generated source-style overlay N areas, ...") is printed. Nothing in the v7 tree is
// modified.
'use strict';
const { CDP, sleep } = require(require('path').join(__dirname, 'cdp.js'));
const fs = require('fs');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const PORT = process.env.KREF_PORT || '6781';
const CDP_PORT = +(process.env.KREF_CDP || 7781);
const PROFILE = process.env.KREF_PROFILE || path.join(process.env.HOME, '.cache/kn-navmesh-ref/chrome');
const URL = `http://127.0.0.1:${PORT}/`;
const [mapFile, pose, out, fov] = process.argv.slice(2);
if (!mapFile || !pose || !out) {
  console.error('usage: node navmesh-ref.js <map.json> "<x,y,z,pitch,yaw>" <out.png> [fovDeg]');
  process.exit(1);
}
const P = pose.split(',').map(Number);

const HOOK = `
(function(){
  var wrapIv = setInterval(function(){
    var r = window.RENDER; if (!r || typeof r.render !== 'function' || r.__navWrapped) return;
    var orig = r.render; r.__navWrapped = true;
    r.render = function() {
      var p = window.__pose, c = this.camera;
      if (p && c) {
        c.rotation.order = 'YXZ';
        for (var o = c.parent; o && o.type !== 'Scene'; o = o.parent) { o.position.set(0, 0, 0); o.rotation.set(0, 0, 0); o.scale.set(1, 1, 1); o.updateMatrix(); }
        c.position.set(p[0], p[1], p[2]); c.rotation.set(p[3], p[4], 0);
        if (p[5]) { c.fov = p[5]; c.updateProjectionMatrix(); }
        c.updateMatrixWorld(true);
        if (this.fpsCamera) this.fpsCamera.layers.set(31);
      }
      return orig.apply(this, arguments);
    };
  }, 20);
})();`;

function up(port) { return new Promise(res => http.get(`http://127.0.0.1:${port}/json/version`, r => res(r.statusCode === 200)).on('error', () => res(false))); }

(async () => {
  if (!(await up(CDP_PORT))) {
    const p = spawn('chromium-browser', ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
      '--window-size=1280,720', `--remote-debugging-port=${CDP_PORT}`, '--user-data-dir=' + PROFILE, '--mute-audio',
      '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', 'about:blank'],
      { detached: true, stdio: 'ignore' });
    p.unref();
    console.log('chromium pid', p.pid);
    for (let i = 0; i < 40 && !(await up(CDP_PORT)); i++) await sleep(250);
  }
  const c = await CDP.connect(CDP_PORT, t => t.type === 'page' && !t.url.startsWith('chrome-extension'));
  await c.send('Page.enable'); await c.send('Runtime.enable');
  const shim = fs.readFileSync(path.join(__dirname, 'shim.js'), 'utf8').replace("'krk_force_offline_sandbox','false'", "'krk_force_offline_sandbox','true'");
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: shim + HOOK });
  await c.setViewport(1280, 720);
  if (!(await c.eval('location.href')).startsWith(URL)) { await c.send('Page.navigate', { url: URL }); await sleep(3000); }
  const data = JSON.stringify(JSON.parse(fs.readFileSync(mapFile, 'utf8')));
  await c.eval(`localStorage.setItem('custMap', ${JSON.stringify(data)}); 1`);
  await c.send('Page.navigate', { url: URL });
  await sleep(2000);
  await c.waitFor(`!!(window.Game && Game.players && window.RENDER && RENDER.scene && typeof enterGame == 'function')`, 120000);
  await sleep(1500);
  if (!(await c.eval(`Game.players.list.some(p => p.isYou && p.active)`))) {
    await c.eval(`enterGame(); 1`);
    await c.waitFor(`Game.players.list.some(p => p.isYou && p.active)`, 30000);
  }
  await sleep(4000); // textures stream in
  const reply = await c.eval(`(()=>{
    const form = document.querySelector('#devConsoleOverlay form'), input = form && form.querySelector('input');
    if (!input) return 'no console form';
    input.value = 'show_navmeshes';
    form.dispatchEvent(new Event('submit', {cancelable: true, bubbles: true}));
    const logs = document.querySelectorAll('#devConsoleOverlay .logs > *');
    return Array.from(logs).slice(-2).map(e => e.textContent).join(' | ');
  })()`);
  await c.eval(`(()=>{window.__pose=${JSON.stringify([...P, Number(fov || 90)])};
    for (const e of document.body.children) if (e.tagName !== 'CANVAS') e.style.visibility='hidden';
    return 1})()`);
  await sleep(1500);
  await c.shot(out);
  console.log('v7 navmesh', path.basename(mapFile), pose, out, '--', reply);
  c.close();
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
