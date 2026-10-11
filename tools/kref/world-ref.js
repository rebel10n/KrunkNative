// Real Krunker v3.1.1 world capture at a pinned camera pose (doc/ref/krunker-v3.1.1/world/).
// usage: node world-ref.js <map> "<x,y,z,pitch,yaw>" <out.png> [fovDeg=90]
//
// Needs the reference server: cd $KRUNKER_REF && PORT=6773 LOBBY_COUNT=1 npm run server.
// Starts (or reuses) headless Chromium on CDP_PORT. The map is loaded as the offline
// sandbox's custom map (localStorage custMap = $KRUNKER_REF/maps/<map>.json, which are
// identical to assets/maps/), the render resolution setting is forced to 1, and the
// renderer (55.js, captured through an Object.prototype "fpsCamera" setter) gets its
// render() wrapped so the camera sits at the pose right before every frame: YXZ Euler
// (pitch about X, yaw about Y), camera parents reset to identity, vertical FOV fovDeg.
// All DOM except the WebGL canvas is hidden. Nothing in the reference tree is modified.
'use strict';
const { CDP, sleep } = require(require('path').join(__dirname, 'cdp.js'));
const fs = require('fs');
const { spawn } = require('child_process');
const http = require('http');

const PORT = Number(process.env.CDP_PORT || 7773), URL = process.env.URL || 'http://127.0.0.1:6773/';
const REF = process.env.KRUNKER_REF || '/home/user/jails/kru/src/Krunker_v3.1.1';
const PROFILE = process.env.CHROME_PROFILE || '/tmp/kn-world-ref/chrome';
const [map, pose, out, fov] = process.argv.slice(2);
// KR_SETTINGS='{"lighting":2}': extra v3.1.1 settings (localStorage kro_setngss_<key>) for the run
const SETTINGS = process.env.KR_SETTINGS || '{}';
const P = pose.split(',').map(Number);

const HOOK = `
(function(){
  try { localStorage.setItem('krk_force_offline_sandbox','true'); localStorage.setItem('kro_setngss_resolution','1');
    var extra = __SETTINGS__; for (var k in extra) localStorage.setItem('kro_setngss_' + k, String(extra[k])); } catch(e) {}
  Object.defineProperty(Object.prototype, 'fpsCamera', { configurable: true,
    set(v) { delete Object.prototype.fpsCamera; Object.defineProperty(this, 'fpsCamera', {value: v, writable: true, configurable: true, enumerable: true}); window.__kr = this; } });
  var wrapIv = setInterval(function(){
    var r = window.__kr; if (!r || typeof r.render !== 'function' || r.__wrapped) return;
    var orig = r.render; r.__wrapped = true;
    r.render = function(a, b) {
      var p = window.__pose;
      if (p && !b) {
        var c = this.camera; c.rotation.order = 'YXZ';
        for (var o = c.parent; o && o.type !== 'Scene'; o = o.parent) { o.position.set(0, 0, 0); o.rotation.set(0, 0, 0); o.scale.set(1, 1, 1); o.updateMatrix(); }
        c.position.set(p[0], p[1], p[2]); c.rotation.set(p[3], p[4], 0);
        if (p[5]) { c.fov = p[5]; c.updateProjectionMatrix(); }
        c.updateMatrixWorld(true);
        if (this.fpsCamera) this.fpsCamera.layers.set(31);
      }
      return orig.apply(this, arguments);
    };
    clearInterval(wrapIv);
  }, 20);
})();`;

function up(port) { return new Promise(res => http.get(`http://127.0.0.1:${port}/json/version`, r => res(r.statusCode === 200)).on('error', () => res(false))); }

(async () => {
  if (!(await up(PORT))) {
    const p = spawn('chromium-browser', ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
      '--window-size=1280,720', `--remote-debugging-port=${PORT}`, '--user-data-dir=' + PROFILE, '--mute-audio',
      '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', 'about:blank'],
      { detached: true, stdio: 'ignore' });
    p.unref(); console.log('chromium pid', p.pid);
    for (let i = 0; i < 40 && !(await up(PORT)); i++) await sleep(250);
  }
  const c = await CDP.connect(PORT);
  await c.send('Page.enable'); await c.send('Runtime.enable');
  const logs = [];
  c.on(m => { if (m.method === 'Runtime.exceptionThrown') logs.push('EXC ' + JSON.stringify(m.params.exceptionDetails).slice(0, 300)); });
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: HOOK.replace('__SETTINGS__', SETTINGS) });
  await c.setViewport(1280, 720);
  const cur = await c.eval('location.href');
  if (!cur.startsWith(URL) || process.env.RELOAD) {
    await c.send('Page.navigate', { url: URL });
    await c.waitFor(`!!(window.__kr && window.__kr.__wrapped && window.__kr.scene)`, 90000);
    await sleep(2000);
  }
  const mapFile = map.endsWith('.json') ? map : `${REF}/maps/${map.toLowerCase()}.json`;
  const want = map.endsWith('.json') ? JSON.parse(fs.readFileSync(mapFile, 'utf8')).name.toLowerCase() : map.toLowerCase();
  const curMap = await c.eval(`(document.getElementById('mapInfo')||{}).textContent||''`);
  if (!curMap.toLowerCase().endsWith('_' + want) || process.env.KR_SETTINGS) {
    const data = fs.readFileSync(mapFile, 'utf8');
    await c.eval(`localStorage.setItem('custMap', ${JSON.stringify(JSON.stringify(JSON.parse(data)))}); 1`);
    await c.send('Page.navigate', { url: URL });
    await sleep(1500);
    await c.waitFor(`!!(window.__kr && window.__kr.__wrapped && window.__kr.scene) && (document.getElementById('mapInfo')||{}).textContent.toLowerCase().endsWith('_${want}')`, 90000);
    await sleep(5000); // textures stream in
  }
  await c.eval(`(()=>{window.__pose=${JSON.stringify([...P, Number(fov || 90)])};
    for (const e of document.body.children) if (e.tagName !== 'CANVAS') e.style.visibility='hidden';
    if (window.devConsole) window.devConsole.visible=false; return 1})()`);
  await sleep(1500);
  await c.shot(out);
  console.log('ref', map, pose, out, (await c.eval(`(document.getElementById('mapInfo')||{}).textContent`)), logs.slice(-3).join('\n'));
  c.close(); process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
