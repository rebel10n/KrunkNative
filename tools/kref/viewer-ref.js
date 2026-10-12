// Krunker v7.1.4 viewer capture (doc/ref/krunker-v7.1.4/viewer/): public/viewer.html?<query> in
// headless Chromium, screenshotted after `wait` ms. The page is static: serve the reference's
// public/ with any static server (it needs no game server), e.g.
//   python3 -I -m http.server 6790 --bind 127.0.0.1 --directory $KRUNKER_V7/public
// usage: node viewer-ref.js "<query>" <out.png> [width=800] [height=600] [wait=2500]
//   env VIEWER_URL (http://127.0.0.1:6790/viewer.html), CDP_PORT (17891), CHROME_PROFILE, GUI=1 (keep the panel)
//   GUI_ACTIONS="<Name or Folder/Name>[=<value>];...": dat.gui actions after the load, as the user
//     would do them (a select / text box gets the value and a change event, a checkbox / function
//     row / folder title is clicked); client_bin's KRUNKNATIVE_VIEWER_GUI takes the same list
//   FREEZE=1 (default): every animation is pinned at its start so the frame is reproducible:
//     the showcase auto-rotation (OrbitControls.autoRotate, a 120 Hz setInterval) never runs and
//     performance.now / Date.now stand still from the page load on (render deltas are 0: pulse,
//     frames, rgb, clips and ShaderToy iTime stay at their first value); client_bin --viewer with
//     KRUNKNATIVE_VIEWER_FREEZE=1 does the same.
// Nothing in the reference tree is modified.
'use strict';
const { CDP, sleep } = require(require('path').join(__dirname, 'cdp.js'));
const { spawn } = require('child_process');
const http = require('http');

const PORT = Number(process.env.CDP_PORT || 17891);
const URL = process.env.VIEWER_URL || 'http://127.0.0.1:6790/viewer.html';
const PROFILE = process.env.CHROME_PROFILE || (process.env.HOME + '/.cache/kn-viewer-ref/chrome');
const FREEZE = process.env.FREEZE !== '0';
const [query, out, w, h, waitArg] = process.argv.slice(2);
const W = Number(w || 800), H = Number(h || 600), WAIT = Number(waitArg || 2500);
if (!query || !out) {
  console.error('usage: node viewer-ref.js "<query>" <out.png> [width] [height] [wait ms]');
  process.exit(1);
}

// Runs before the page's scripts.
const HOOK = `
(function(){
  // only the viewer's #container (hook.js adds overlays such as a session timer); GUI=1 keeps
  // the dat.gui panel too
  document.addEventListener('DOMContentLoaded', function(){
    var st = document.createElement('style');
    st.textContent = 'body > :not(#container)${process.env.GUI ? ':not(.dg)' : ''}{display:none!important}';
    document.head.appendChild(st);
  });
  if (!${FREEZE}) return;
  var realSetInterval = window.setInterval;
  window.setInterval = function(fn, ms) {
    if (ms && ms < 10) return 0; // OrbitControls.autoRotate's 1000 / 120 ms update
    return realSetInterval.apply(window, arguments);
  };
  var t0 = performance.now(), d0 = Date.now();
  performance.now = function(){ return t0; };
  Date.now = function(){ return d0; };
})();`;

// runs in the page: the dat.gui DOM (li.cr > .property-name, li.folder > .dg > ul > li.title)
function guiActions(list) {
  const errors = [];
  const folderOf = (li) => { const ul = li.closest('ul'); const t = ul && ul.querySelector(':scope > li.title'); return t ? t.textContent : ''; };
  for (const item of list.split(';')) {
    if (!item) continue;
    const eq = item.indexOf('=');
    const path = eq < 0 ? item : item.slice(0, eq), value = eq < 0 ? null : item.slice(eq + 1);
    const slash = path.lastIndexOf('/');
    const folder = slash < 0 ? '' : path.slice(0, slash), name = path.slice(slash + 1);
    const row = [...document.querySelectorAll('.dg li.cr')].find((li) => li.querySelector('.property-name').textContent === name && (!folder || folderOf(li) === folder));
    if (!row) {
      const title = [...document.querySelectorAll('.dg li.title')].find((t) => t.textContent === name);
      if (title) title.click(); else errors.push('no controller ' + path);
      continue;
    }
    const select = row.querySelector('select'), text = row.querySelector('input[type=text]'), box = row.querySelector('input[type=checkbox]');
    if (select) { select.value = value; select.dispatchEvent(new Event('change')); }
    else if (box) { if (value == null || (value === 'true') !== box.checked) box.click(); }
    else if (text) { text.value = value; text.dispatchEvent(new Event('change')); text.dispatchEvent(new Event('blur')); }
    else row.click();
  }
  return errors.join('\n');
}

function up(port) { return new Promise(res => http.get(`http://127.0.0.1:${port}/json/version`, r => res(r.statusCode === 200)).on('error', () => res(false))); }

(async () => {
  if (!(await up(PORT))) {
    const p = spawn('chromium-browser', ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
      `--window-size=${W},${H}`, `--remote-debugging-port=${PORT}`, '--user-data-dir=' + PROFILE, '--mute-audio', '--hide-scrollbars',
      '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', 'about:blank'],
      { detached: true, stdio: 'ignore' });
    p.unref();
    for (let i = 0; i < 40 && !(await up(PORT)); i++) await sleep(250);
  }
  const cdp = await CDP.connect(PORT);
  await cdp.send('Page.enable');
  await cdp.send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
  const hook = await cdp.send('Page.addScriptToEvaluateOnNewDocument', { source: HOOK });
  await cdp.send('Page.navigate', { url: URL + '?' + query });
  await sleep(WAIT);
  if (process.env.GUI_ACTIONS) {
    const r = await cdp.send('Runtime.evaluate', { expression: `(${guiActions})(${JSON.stringify(process.env.GUI_ACTIONS)})`, returnByValue: true });
    if (r.result && r.result.value) console.error(r.result.value);
    await sleep(Number(process.env.ACTION_WAIT || 1500));
  }
  await cdp.shot(out);
  await cdp.send('Page.removeScriptToEvaluateOnNewDocument', { identifier: hook.identifier });
  await cdp.send('Page.navigate', { url: 'about:blank' });
  console.log(out);
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
