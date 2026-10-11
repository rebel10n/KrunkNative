#!/usr/bin/env node
// Same-origin front for the real Krunker v3.1.1 site against kn_brain, like the
// nginx of a deployment: static files from the v3.1.1 public/ dir (read-only),
// /api/*, /social/*, /mm/* (HTTP and WebSocket upgrades) proxied to the brain.
// The v3.1.1 offline hook (public/js/hook.js) points the page's socialURL/apiURL at
// location.origin + "/api", so the social socket is <origin>/api/ws.
//
//   node v311_site_proxy.js <listenPort> <brainPort> <publicDir>
'use strict';
const http = require('http');
const net = require('net');
const fs = require('fs');
const path = require('path');

const [listenPort, brainPort, publicDir] = process.argv.slice(2);
const root = path.resolve(publicDir);
const TYPES = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css', '.png': 'image/png',
  '.jpg': 'image/jpeg', '.svg': 'image/svg+xml', '.json': 'application/json', '.ico': 'image/x-icon',
  '.woff': 'font/woff', '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg',
  '.gif': 'image/gif', '.webp': 'image/webp', '.obj': 'text/plain', '.glb': 'model/gltf-binary' };
const proxied = (u) => /^\/(api|social|mm)(\/|$)/.test(u);

const server = http.createServer((req, res) => {
  if (proxied(req.url)) {
    const up = http.request({ host: '127.0.0.1', port: brainPort, path: req.url, method: req.method,
      headers: Object.assign({}, req.headers, { 'x-forwarded-for': req.socket.remoteAddress }) }, (r) => {
      res.writeHead(r.statusCode, r.headers);
      r.pipe(res);
    });
    up.on('error', () => { res.writeHead(502); res.end(); });
    req.pipe(up);
    return;
  }
  let p = decodeURIComponent(req.url.split('?')[0]);
  if (p === '/') p = '/index.html';
  const file = path.join(root, p);
  if (!file.startsWith(root)) { res.writeHead(403); res.end(); return; }
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
});

server.on('upgrade', (req, socket, head) => {
  if (!proxied(req.url)) { socket.destroy(); return; }
  const up = net.connect(+brainPort, '127.0.0.1', () => {
    let raw = `${req.method} ${req.url} HTTP/1.1\r\n`;
    for (let i = 0; i < req.rawHeaders.length; i += 2) raw += `${req.rawHeaders[i]}: ${req.rawHeaders[i + 1]}\r\n`;
    up.write(raw + '\r\n');
    if (head && head.length) up.write(head);
    up.pipe(socket);
    socket.pipe(up);
  });
  up.on('error', () => socket.destroy());
  socket.on('error', () => up.destroy());
});

server.listen(+listenPort, '127.0.0.1', () => console.log(`v3.1.1 site on http://127.0.0.1:${listenPort} -> brain :${brainPort}`));
