// Minimal CDP client over `ws` (from KrunkNative/net-bridge/node_modules).
'use strict';
const WebSocket = require('/home/user/src/KrunkNative/net-bridge/node_modules/ws');
const http = require('http');
const fs = require('fs');

function getJSON(url) {
  return new Promise((res, rej) => {
    http.get(url, (r) => { let d = ''; r.on('data', c => d += c); r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } }); }).on('error', rej);
  });
}

class CDP {
  constructor(ws) { this.ws = ws; this.id = 0; this.pending = new Map(); this.handlers = []; this.sessions = new Map();
    ws.on('message', (m) => {
      const msg = JSON.parse(m.toString());
      if (msg.id && this.pending.has(msg.id)) { const { res, rej } = this.pending.get(msg.id); this.pending.delete(msg.id); msg.error ? rej(new Error(JSON.stringify(msg.error))) : res(msg.result); }
      else if (msg.method) { for (const h of this.handlers) h(msg); }
    });
  }
  static async connect(port, pick) {
    let targets = []; for (let i = 0; i < 60; i++) { try { targets = await getJSON(`http://127.0.0.1:${port}/json/list`); if (targets.length) break; } catch (e) { } await sleep(500); }
    const t = (pick && targets.find(pick)) || targets.find(t => t.type === 'page');
    if (!t) throw new Error('no page target: ' + JSON.stringify(targets));
    const ws = new WebSocket(t.webSocketDebuggerUrl, { perMessageDeflate: false, maxPayload: 512 * 1024 * 1024 });
    await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
    return new CDP(ws);
  }
  send(method, params = {}) { const id = ++this.id; this.ws.send(JSON.stringify({ id, method, params })); return new Promise((res, rej) => this.pending.set(id, { res, rej })); }
  on(fn) { this.handlers.push(fn); }
  async eval(expr, opts = {}) {
    const r = await this.send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true, userGesture: true, ...opts });
    if (r.exceptionDetails) throw new Error('eval: ' + (r.exceptionDetails.exception && r.exceptionDetails.exception.description || JSON.stringify(r.exceptionDetails)));
    return r.result.value;
  }
  async shot(path, opts = {}) {
    const r = await this.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false, ...opts });
    fs.writeFileSync(path, Buffer.from(r.data, 'base64')); return path;
  }
  async dom(path) { const html = await this.eval('document.documentElement.outerHTML'); fs.writeFileSync(path, html); return html.length; }
  async key(key, code, keyCode, opts = {}) {
    const text = key.length === 1 ? key : undefined;
    await this.send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', key, code, windowsVirtualKeyCode: keyCode, nativeVirtualKeyCode: keyCode, text, unmodifiedText: text, ...opts });
    if (opts.hold) await sleep(opts.hold);
    await this.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: keyCode, nativeVirtualKeyCode: keyCode });
  }
  async keyDown(key, code, keyCode) { await this.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key, code, windowsVirtualKeyCode: keyCode, nativeVirtualKeyCode: keyCode }); }
  async keyUp(key, code, keyCode) { await this.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: keyCode, nativeVirtualKeyCode: keyCode }); }
  async mouse(type, x, y, button = 'left', opts = {}) {
    const buttons = type === 'mouseReleased' ? 0 : (button === 'left' ? 1 : button === 'right' ? 2 : 0);
    await this.send('Input.dispatchMouseEvent', { type, x, y, button, buttons, clickCount: type === 'mouseMoved' ? 0 : 1, ...opts });
  }
  async click(x, y, button = 'left') { await this.mouse('mouseMoved', x, y, 'none'); await this.mouse('mousePressed', x, y, button); await sleep(40); await this.mouse('mouseReleased', x, y, button); }
  async setViewport(w, h) { await this.send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false }); }
  async waitFor(expr, timeout = 30000, step = 250) {
    const t0 = Date.now();
    while (Date.now() - t0 < timeout) { let v = false; try { v = await this.eval(expr); } catch (e) { } if (v) return v; await sleep(step); }
    throw new Error('timeout waiting for: ' + expr);
  }
  close() { this.ws.close(); }
}
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
module.exports = { CDP, sleep };
