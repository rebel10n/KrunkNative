// static_ref.py's browser half: the transcribed page drawn under the same device emulation as FRVR's own captures
// (tools/uidiff/capture-strike.js: 640 x 360 CSS px landscape at deviceScaleFactor 2, mobile), then screenshotted.
//
//   node tools/strike/static_ref_shot.js <cdp port> <page url> <css width> <css height> <dpr> <out.png>
'use strict';
const path = require('path');
const { CDP, sleep } = require(path.join(__dirname, '..', 'kref', 'cdp.js'));

(async () => {
  const [port, url, w, h, dpr, out] = process.argv.slice(2);
  const c = await CDP.connect(+port);
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.send('Emulation.setDeviceMetricsOverride', { width: +w, height: +h, deviceScaleFactor: +dpr, mobile: true,
    screenOrientation: { type: 'landscapePrimary', angle: 90 } });
  await c.send('Page.navigate', { url });
  await c.waitFor('document.readyState === "complete"', 30000);
  await c.eval('document.fonts.ready.then(() => true)');
  await sleep(1500); // the images' decode and a few frames
  await c.shot(out);
  c.close();
  process.exit(0);
})().catch((e) => { console.error('static_ref_shot: ' + e.message); process.exit(1); });
