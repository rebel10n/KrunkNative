// Writes tests/data/v7-menu-1280.computed for test_cascade.c: Chrome's getComputedStyle of every
// element with an id in tests/data/v7-menu-1280.html, loaded with the sheets test_cascade loads
// (the shipped v7 sheets, krunknative.css, hud.css, then the page's own <style> blocks), plus
// the changes CSS.forcePseudoState(':hover') makes under each element that has hover rules.
//
//   node client/src/html/engine/tests/capture_computed.js [repo root]
//
// Needs chromium-browser (KN_CHROME; profile dir KN_CHROME_PROFILE, DevTools port KN_CDP_PORT)
// and the ws module tools/kref/cdp.js uses.
// Output, one value per line: <selector> TAB <property> TAB <value>.  A line is written when
// the value differs from the parent element's (so plain inheritance is checked at the top of
// the tree only), always for <html>.  "@hover <selector>" starts a section captured with that
// element forced :hover; it lists only the values the hover changes.
'use strict';
const fs = require('fs');
const http = require('http');
const path = require('path');
const { launchChrome } = require('../../../../../tools/ui-import/v7_page.js');
const { CDP, sleep } = require('../../../../../tools/kref/cdp.js');

const ROOT = path.resolve(process.argv[2] || path.join(__dirname, '../../../../..'));
const DATA = path.join(ROOT, 'client/src/html/engine/tests/data');
const SHEETS = ['main.css', 'bundled/bundledStyles.min.css', 'main_custom.css', 'flag.css', 'material-icons.css',
  'material-icons-outlined.css', 'material-symbols-outlined.css', 'google-play.css', 'krunknative.css', 'hud.css'];
const PROPS = ['color', 'background-color', 'font-size', 'font-weight', 'font-style', 'font-family', 'line-height',
  'letter-spacing', 'text-align', 'text-transform', 'white-space', 'visibility', 'cursor', 'pointer-events',
  'user-select', 'direction', 'image-rendering', 'text-shadow', 'display', 'position', 'float', 'clear',
  'box-sizing', 'overflow-x', 'overflow-y', 'z-index', 'opacity', 'top', 'right', 'bottom', 'left',
  'margin-top', 'margin-right', 'margin-bottom', 'margin-left', 'padding-top', 'padding-right', 'padding-bottom',
  'padding-left', 'border-top-width', 'border-left-width', 'border-bottom-width', 'border-top-style',
  'border-left-style', 'border-bottom-style', 'border-left-color', 'border-bottom-color', 'border-top-left-radius',
  'border-bottom-right-radius', 'outline-style', 'background-image', 'background-size', 'background-repeat',
  'box-shadow', 'object-fit', 'text-decoration-line', 'text-overflow', 'vertical-align', 'flex-direction',
  'justify-content', 'align-items', 'flex-grow', 'row-gap'];
const PORT = +(process.env.KN_CDP_PORT || 9598), HTTP_PORT = PORT - 2700;

// The page: scripts emptied and disabled (the engine ignores them; the elements stay so
// :nth-child paths match the engine's DOM), the sheets linked first in <head>.
function page() {
  let html = fs.readFileSync(path.join(DATA, 'v7-menu-1280.html'), 'utf8');
  html = html.replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '<script type="text/plain"></script>');
  const links = SHEETS.map(s => `<link rel="stylesheet" href="css/${s}">`).join('');
  return html.replace(/<head>/i, '<head>' + links);
}

function serve() {
  const body = page();
  const server = http.createServer((req, res) => {
    const rel = decodeURIComponent(req.url.split('?')[0]);
    if (rel === '/ui/__computed.html') { res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }); return res.end(body); }
    const file = path.join(ROOT, path.normalize(rel));
    if (!file.startsWith(ROOT + '/ui/')) { res.writeHead(404); return res.end(); }
    fs.readFile(file, (err, d) => {
      if (err) { res.writeHead(404); return res.end(); }
      const ext = path.extname(file);
      res.writeHead(200, { 'Content-Type': ext === '.css' ? 'text/css' : ext === '.svg' ? 'image/svg+xml' : 'application/octet-stream' });
      res.end(d);
    });
  });
  return new Promise(r => server.listen(HTTP_PORT, '127.0.0.1', () => r(server)));
}

// In-page helpers: sel(e) addresses an element the way test_cascade's find() can (first
// element with that id, else a child path from the nearest one), snap(root) reads PROPS.
const HELPERS = `window.__kn = {
  PROPS: ${JSON.stringify(PROPS)},
  sel(e) {
    if (e.id && document.getElementById(e.id) === e && /^[A-Za-z][\\w-]*$/.test(e.id)) return '#' + e.id;
    if (e === document.documentElement) return 'html';
    if (e === document.body) return 'body';
    const p = e.parentElement;
    return this.sel(p) + ' > ' + e.localName + ':nth-child(' + (Array.prototype.indexOf.call(p.children, e) + 1) + ')';
  },
  snap(e) {
    const cs = getComputedStyle(e), o = {};
    for (const p of this.PROPS) o[p] = cs.getPropertyValue(p).split(location.origin + '/ui/').join('');
    return o;
  },
}; true`;

const STATIC = `(() => {
  const out = [], html = document.documentElement, cs = getComputedStyle(html);
  const vars = []; for (let i = 0; i < cs.length; i++) if (cs[i].startsWith('--')) vars.push(cs[i]);
  for (const v of vars.sort()) out.push(['html', v, cs.getPropertyValue(v).trim()]);
  const all = [html, ...document.querySelectorAll('[id]')];
  const seen = new Set();
  for (const e of all) {
    if (seen.has(e) || (e.id && document.getElementById(e.id) !== e) || e.closest('svg, head')) continue;
    seen.add(e);
    const s = __kn.sel(e), v = __kn.snap(e), pv = e === html ? null : __kn.snap(e.parentElement);
    for (const p of __kn.PROPS) if (!pv || pv[p] !== v[p]) out.push([s, p, v[p]]);
  }
  return out;
})()`;

// Elements a :hover rule can change: the subjects (or ancestors) of every :hover selector.
const HOVER_CANDIDATES = `(() => {
  const out = new Set();
  const walk = (rules) => { for (const r of rules) {
    if (r.cssRules && !r.selectorText) { walk(r.cssRules); continue; }
    if (!r.selectorText || !r.selectorText.includes(':hover')) continue;
    for (const part of r.selectorText.split(',')) {
      const m = part.split(':hover')[0].trim();
      if (!m || /::|:not\\(|:is\\(/.test(m)) continue;
      let list; try { list = document.querySelectorAll(m); } catch (e) { continue; }
      for (const e of list) if (!e.closest('svg, head')) out.add(e);
    }
  } };
  for (const sh of document.styleSheets) { try { walk(sh.cssRules); } catch (e) { } }
  return [...out].map(e => __kn.sel(e));
})()`;

function subtreeSnap(sel) {
  return `(() => { const r = document.querySelector(${JSON.stringify(sel)}); const o = {};
    for (const e of [r, ...r.querySelectorAll('*')]) { if (e.closest('svg')) continue; o[__kn.sel(e)] = __kn.snap(e); } return o; })()`;
}

(async () => {
  const srv = await serve();
  const prof = process.env.KN_CHROME_PROFILE || path.join(process.env.HOME, '.cache', 'kn-capture-computed');
  const ch = launchChrome(PORT, 1280, 720, prof);
  const c = await CDP.connect(PORT);
  try {
    await c.send('Page.enable'); await c.send('Runtime.enable'); await c.send('DOM.enable'); await c.send('CSS.enable');
    await c.setViewport(1280, 720);
    await c.send('Page.navigate', { url: `http://127.0.0.1:${HTTP_PORT}/ui/__computed.html` });
    await c.waitFor('document.readyState === "complete" && document.styleSheets.length >= 10');
    await c.eval('document.fonts.ready.then(() => true)');
    await sleep(500);
    await c.eval(HELPERS);
    const lines = (await c.eval(STATIC)).map(r => r.join('\t'));
    const cands = [...new Set(await c.eval(HOVER_CANDIDATES))];
    const doc = await c.send('DOM.getDocument', { depth: -1 });
    let sections = 0;
    for (const sel of cands) {
      const q = await c.send('DOM.querySelector', { nodeId: doc.root.nodeId, selector: sel });
      if (!q.nodeId) continue;
      const before = await c.eval(subtreeSnap(sel));
      await c.send('CSS.forcePseudoState', { nodeId: q.nodeId, forcedPseudoClasses: ['hover'] });
      const after = await c.eval(subtreeSnap(sel));
      await c.send('CSS.forcePseudoState', { nodeId: q.nodeId, forcedPseudoClasses: [] });
      const diff = [];
      for (const s of Object.keys(after)) for (const p of PROPS) if (before[s][p] !== after[s][p]) diff.push([s, p, after[s][p]].join('\t'));
      if (diff.length) { lines.push('@hover ' + sel, ...diff); sections++; }
    }
    const head = `# selector\tproperty\tgetComputedStyle value (${await c.eval('navigator.userAgent.match(/Chrome\\/[\\d.]+/)[0]')}, 1280x720, ` +
      'v7-menu-1280.html; written by tests/capture_computed.js)\n';
    fs.writeFileSync(path.join(DATA, 'v7-menu-1280.computed'), head + lines.join('\n') + '\n');
    console.log(`${lines.length - sections} values, ${sections} hover sections (${cands.length} candidates)`);
  } finally {
    c.close(); process.kill(-ch.pid, 'SIGKILL'); srv.close();
  }
})().catch(e => { console.error(e); process.exit(1); });
