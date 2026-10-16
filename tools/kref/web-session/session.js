'use strict';
// Web e2e session helpers: one page on the stack's Chromium (tools/kref/web-session/stack.sh), with
// every console error, uncaught exception, failed request, HTTP >= 400 response and WebGL error
// recorded per category, plus DOM helpers that click like a user (CDP mouse events).
//   const s = require('./session.js'); const page = await s.openPage(env.BASE + '/?offline=1');
const fs = require('fs');
const path = require('path');
const { CDP, sleep } = require('../cdp.js');

// The stack writes RUN/env; RUN comes from KN_WEB_RUN or the newest /tmp/kn-web-session-* dir.
function stackEnv() {
  let run = process.env.KN_WEB_RUN;
  if (!run) throw new Error('KN_WEB_RUN is not set (the stack prints its RUN dir)');
  const env = {};
  for (const tok of fs.readFileSync(path.join(run, 'env'), 'utf8').trim().split(/\s+/)) {
    const i = tok.indexOf('='); if (i > 0) env[tok.slice(0, i)] = tok.slice(i + 1);
  }
  env.BASE = `http://127.0.0.1:${env.BRIDGE}`;
  return env;
}

// Console lines that are known and expected in a local stack (no WebRTC/WebTransport gateway, no
// Twitch streams, Chromium's own notices). Add a line here only with a reason next to it.
const EXPECTED = [
  /\/rtc\.json\b/, // no WebRTC gateway configured: the client falls back to the bridge WebSocket
  /\/wt\.json\b/, // no WebTransport relay configured
  /\/streams\/top\b/, // the brain's answer without a Twitch key is 503
  /ScriptProcessorNode is deprecated/,
];

async function openPage(url, { viewport = [1280, 720] } = {}) {
  const env = stackEnv();
  const cdp = await CDP.connect(Number(env.CDPP), (t) => t.type === 'page');
  const log = { errors: [], warnings: [], exceptions: [], failed: [], http: [], gl: [], all: [] };
  const note = (kind, text) => {
    log.all.push(kind + ' ' + text);
    if (EXPECTED.some((re) => re.test(text))) return;
    if (kind === 'exception') log.exceptions.push(text);
    else if (kind === 'error') log.errors.push(text);
    else if (kind === 'warn') log.warnings.push(text);
    else if (kind === 'failed') log.failed.push(text);
    else if (kind === 'http') log.http.push(text);
    if (/GL_INVALID_|GL_OUT_OF_MEMORY|WebGL: too many errors|CONTEXT_LOST|WebGL: INVALID/.test(text)) log.gl.push(text);
  };
  cdp.on((m) => {
    const p = m.params;
    if (m.method === 'Runtime.consoleAPICalled') {
      const text = p.args.map((a) => (a.value !== undefined ? a.value : a.description)).join(' ');
      if (p.type === 'error' || p.type === 'assert') note('error', text);
      else if (p.type === 'warning') note('warn', text);
    } else if (m.method === 'Runtime.exceptionThrown') {
      const d = p.exceptionDetails;
      note('exception', (d.exception && d.exception.description) || d.text);
    } else if (m.method === 'Log.entryAdded') {
      const e = p.entry;
      if (e.source === 'network') return; // the failed/HTTP lines come from Network.* below
      note(e.level === 'error' ? 'error' : 'warn', '[' + e.source + '] ' + e.text + (e.url ? ' ' + e.url : ''));
    } else if (m.method === 'Network.loadingFailed' && !p.canceled) {
      note('failed', p.errorText + ' ' + (p.requestId || ''));
    } else if (m.method === 'Network.responseReceived' && p.response.status >= 400) {
      note('http', p.response.status + ' ' + p.response.url);
    }
  });
  for (const d of ['Runtime', 'Log', 'Page', 'Network']) await cdp.send(d + '.enable');
  await cdp.setViewport(viewport[0], viewport[1]);
  await cdp.send('Page.navigate', { url });
  return {
    cdp, log, env,
    mark() { return { errors: log.errors.length, exceptions: log.exceptions.length, failed: log.failed.length, http: log.http.length, gl: log.gl.length }; },
    since(snap) {
      return { errors: log.errors.slice(snap.errors), exceptions: log.exceptions.slice(snap.exceptions), failed: log.failed.slice(snap.failed), http: log.http.slice(snap.http), gl: log.gl.slice(snap.gl) };
    },
    // [x, y, w, h] of the first visible element matching sel, or null
    async rect(sel) {
      return cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; const b = e.getBoundingClientRect(); const s = getComputedStyle(e); if (!b.width || !b.height || s.display === 'none' || s.visibility === 'hidden') return null; return [b.x + b.width / 2, b.y + b.height / 2, b.width, b.height]; })()`);
    },
    async clickSel(sel) { const r = await this.rect(sel); if (!r) return false; await cdp.click(r[0], r[1]); return true; },
    async clickId(id) { return this.clickSel('#' + id); },
    async text(sel) { return cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? (e.innerText || e.textContent || '').trim().slice(0, 400) : null; })()`); },
    async visible(sel) { return cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; const s = getComputedStyle(e); const b = e.getBoundingClientRect(); return s.display !== 'none' && s.visibility !== 'hidden' && b.width > 0 && b.height > 0; })()`); },
    shot(file) { return cdp.shot(file); },
    close() { cdp.close(); },
  };
}

module.exports = { openPage, stackEnv, sleep, CDP };
