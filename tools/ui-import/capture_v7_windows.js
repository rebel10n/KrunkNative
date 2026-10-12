#!/usr/bin/env node
// Captures every Krunker v7.1.4 menu window exactly as v7's own client writes it into
// #menuWindow (showWindow(n): header, extraCls, inline width/height and the gen() markup),
// for a returning guest with default settings, from v7's public/ served read-only
// (v7_page.js). Output: tools/ui-import/krunker-windows-v7.json.gz (gzipped JSON), which
// import.py --ref v7 splits into ui/windows/ (windows_v7.py).
//
//   node tools/ui-import/capture_v7_windows.js [out.json.gz]
//
// Needs chromium-browser (KN_CHROME=...) and net-bridge/node_modules (ws, for tools/kref/cdp.js).
'use strict';
const fs = require('fs');
const path = require('path');
const { openPage, CONNECTED_MENU, sleep } = require('./v7_page.js');

const OUT = process.argv[2] || path.join(__dirname, 'krunker-windows-v7.json.gz');
const zlib = require('zlib');

// Capture of the open window: header, #menuWindow class/style/markup, #windowHolder class.
const SNAP = `(() => ({
  header: windowHeader.innerHTML,
  cls: menuWindow.getAttribute('class') || '',
  style: menuWindow.getAttribute('style') || '',
  holderCls: windowHolder.getAttribute('class') || '',
  html: menuWindow.innerHTML,
  open: windowHolder.style.display === 'block'
}))()`;

(async () => {
  const log = (m) => console.error('[capture-v7] ' + m);
  const { c, errors, close } = await openPage({ log });
  try {
    await c.eval(CONNECTED_MENU);
    await sleep(500);
    const count = await c.eval('windows.length');
    const out = { source: 'Krunker v7.1.4 public/ (static), returning guest, default settings', windows: {}, settings: {}, controls: [] };
    for (let i = 0; i < count; i++) {
      const id = i + 1;
      let snap = null, err = null;
      try {
        await c.eval(`(() => { closWind(); showWindow(${id}, true); return 1; })()`);
        await sleep(150);
        snap = await c.eval(SNAP);
      } catch (e) { err = String(e.message || e).slice(0, 300); }
      const meta = await c.eval(`(() => { const w = windows[${i}]; return { label: w.label || null, sticky: !!w.sticky, width: w.width == null ? null : w.width,
        extraCls: w.extraCls || null, hideScroll: !!w.hideScroll, forceScroll: !!w.forceScroll, maxH: w.maxH || null, height: w.height || null, radius: w.radius == null ? null : w.radius }; })()`);
      out.windows[id] = Object.assign({ index: i }, meta, snap || {}, err ? { error: err } : {});
      log(`window ${id} (${meta.label}): ${snap ? snap.html.length + ' bytes' : 'ERROR ' + err}`);
    }
    // Settings (windows[0]): every tab of the basic and the advanced view
    for (const type of ['basic', 'advanced']) {
      const tabs = await c.eval(`windows[0].tabs.${type}.map(t => t.name)`);
      out.settings[type] = [];
      for (let t = 0; t < tabs.length; t++) {
        await c.eval(`(() => { closWind(); windows[0].settingType = '${type}'; windows[0].tabIndex = ${t}; windows[0].settingSearch = null; showWindow(1, true); return 1; })()`);
        await sleep(150);
        const snap = await c.eval(SNAP);
        out.settings[type].push({ name: tabs[t], header: snap.header, cls: snap.cls, style: snap.style, html: snap.html });
        log(`settings ${type}/${tabs[t]}: ${snap.html.length} bytes`);
      }
    }
    await c.eval(`(() => { windows[0].settingType = 'basic'; windows[0].tabIndex = 0; return 1; })()`);
    // Change Controls (windows[6]): the five tabs
    const ctabs = await c.eval('windows[6].tabs.map(t => t.n)');
    for (let t = 0; t < ctabs.length; t++) {
      await c.eval(`(() => { closWind(); windows[6].tabIndex = ${t}; showWindow(7, true); return 1; })()`);
      await sleep(150);
      const snap = await c.eval(SNAP);
      out.controls.push({ name: ctabs[t], html: snap.html });
      log(`controls ${ctabs[t]}: ${snap.html.length} bytes`);
    }
    await c.eval('windows[6].tabIndex = 0, closWind(), 1');
    out.pageErrors = errors.slice(0, 50);
    const text = JSON.stringify(out);
    fs.writeFileSync(OUT, zlib.gzipSync(Buffer.from(text, 'utf8'), { level: 9, mtime: 0 }));
    log(`wrote ${OUT} (${text.length} bytes of JSON, ${errors.length} page errors)`);
  } finally { close(); }
})().catch((e) => { console.error(e); process.exit(1); });
