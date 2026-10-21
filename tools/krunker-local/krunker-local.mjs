#!/usr/bin/env node
// Krunker v10.0.3 local harness. It copies the official client into a serve directory outside
// git, serves it on localhost, sends the client's matchmaker hosts to our brain, and checks the
// load in headless Chromium. Nothing in the copied client is patched: the official hostnames
// are resolved to a local listener by Chromium (host-resolver-rules), and that listener
// forwards /mm/* to the brain.
//
//   node tools/krunker-local/krunker-local.mjs prepare   copy + fill assets + TLS cert (idempotent)
//   node tools/krunker-local/krunker-local.mjs serve     HTTP (page) + HTTPS (matchmaker hosts)
//   node tools/krunker-local/krunker-local.mjs check     headless load, JSON report on stdout
//
// Inputs are read-only: the v10.0.3 tree (krunkerSrc) and the KrunkNative trees (fillRoots).
// Config: tools/krunker-local/config.json if present, else config.example.json.
import fs from "node:fs";
import path from "node:path";
import http from "node:http";
import https from "node:https";
import net from "node:net";
import os from "node:os";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));

function loadConfig() {
  const local = path.join(HERE, "config.json");
  const file = fs.existsSync(local) ? local : path.join(HERE, "config.example.json");
  const cfg = JSON.parse(fs.readFileSync(file, "utf8"));
  const home = (p) => path.resolve(p.replace(/^~/, os.homedir()));
  cfg._file = file;
  cfg.krunkerSrc = home(cfg.krunkerSrc);
  cfg.knRepo = home(cfg.knRepo);
  cfg.wwwDir = home(cfg.wwwDir);
  cfg.runDir = home(cfg.runDir);
  if (cfg.bridge && cfg.bridge.log) cfg.bridge.log = home(cfg.bridge.log);
  // Nothing leaves the machine for a production host: the brain the matchmaker and API hosts forward to is local.
  const brainHost = new URL(cfg.brainUrl).hostname;
  if (!["127.0.0.1", "localhost"].includes(brainHost)) {
    throw new Error(`brainUrl must be a local kn_brain (127.0.0.1), not ${brainHost}`);
  }
  cfg.localHost = new URL(cfg.localUrl).hostname;
  return cfg;
}

const cfg = loadConfig();
const cmd = process.argv[2] || "help";

// Files of the official tree that are our dev servers, not client content.
const SKIP = new Set(["server.mjs", "serve-v10.mjs", "local.html"]);

const MIME = {
  ".html": "text/html; charset=utf-8", ".js": "application/javascript", ".mjs": "application/javascript",
  ".css": "text/css", ".json": "application/json", ".wasm": "application/wasm", ".png": "image/png",
  ".jpg": "image/jpeg", ".gif": "image/gif", ".svg": "image/svg+xml", ".ttf": "font/ttf",
  ".woff": "font/woff", ".woff2": "font/woff2", ".mp3": "audio/mpeg", ".ogg": "audio/ogg",
  ".webm": "video/webm", ".mp4": "video/mp4", ".jspck": "application/octet-stream", ".txt": "text/plain",
  ".dat": "application/octet-stream",
};

// prepare: copy the official tree (never written to), fill missing paths with symlinks into
// KrunkNative (first root with the file wins), and make the self-signed cert for the HTTPS side.
function prepare() {
  if (!fs.existsSync(cfg.krunkerSrc)) throw new Error(`krunkerSrc missing: ${cfg.krunkerSrc}`);
  fs.mkdirSync(cfg.wwwDir, { recursive: true });
  fs.mkdirSync(cfg.runDir, { recursive: true });
  for (const ent of fs.readdirSync(cfg.krunkerSrc)) {
    if (SKIP.has(ent)) continue;
    fs.cpSync(path.join(cfg.krunkerSrc, ent), path.join(cfg.wwwDir, ent), { recursive: true, force: true });
  }
  const filled = [];
  for (const rel of cfg.fillPaths) {
    const to = path.join(cfg.wwwDir, rel);
    if (fs.existsSync(to)) continue;
    const from = cfg.fillRoots.map((r) => path.join(cfg.knRepo, r, rel)).find((p) => fs.existsSync(p));
    if (!from) continue;
    fs.mkdirSync(path.dirname(to), { recursive: true });
    fs.symlinkSync(from, to);
    filled.push(rel);
  }
  console.log(`prepare: www=${cfg.wwwDir}`);
  console.log(`prepare: filled ${filled.length ? filled.join(", ") : "nothing"}`);
  const key = path.join(cfg.runDir, "tls-key.pem");
  const crt = path.join(cfg.runDir, "tls-cert.pem");
  if (!fs.existsSync(key) || !fs.existsSync(crt)) {
    const r = spawnSync("openssl", ["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
      "-nodes", "-days", "3650", "-subj", "/CN=krunker-local", "-keyout", key, "-out", crt], { stdio: "ignore" });
    if (r.status !== 0) throw new Error("openssl cert generation failed");
    console.log("prepare: wrote a self-signed cert for the local HTTPS side (run dir, not git)");
  }
}

function sendCors(res) {
  res.setHeader("access-control-allow-origin", "*");
  res.setHeader("access-control-allow-methods", "GET,POST,PUT,DELETE,OPTIONS");
  res.setHeader("access-control-allow-headers", "*");
  res.setHeader("access-control-expose-headers", "content-length,content-range");
}

// Forward to the brain. Matchmaker hosts map their path onto /mm/ (the official client's
// matchmakerAddress is the bare host; our brain serves the same routes under /mm/).
// transform (optional) gets the whole upstream body and the status, and returns the body to send.
function proxy(req, res, pathname, transform = null) {
  const target = new URL(cfg.brainUrl + pathname + new URL(req.url, "http://x").search);
  const headers = { ...req.headers, host: target.host };
  delete headers.origin;
  delete headers.referer;
  if (transform) delete headers["accept-encoding"]; // the transform reads the body as text
  const client = target.protocol === "http:" ? http : https; // brainUrl may be a local http brain
  const up = client.request(target, { method: req.method, headers }, (ur) => {
    const h = { ...ur.headers };
    delete h["access-control-allow-origin"];
    sendCors(res);
    if (transform) {
      const chunks = [];
      ur.on("data", (c) => chunks.push(c));
      ur.on("end", () => {
        const body = transform(Buffer.concat(chunks), ur.statusCode || 502);
        h["content-length"] = String(body.length);
        res.writeHead(ur.statusCode || 502, h);
        res.end(body);
      });
      return;
    }
    res.writeHead(ur.statusCode || 502, h);
    ur.pipe(res);
  });
  up.on("error", (e) => {
    console.log(`[proxy] ${req.method} ${pathname} -> error ${e.message}`);
    if (!res.headersSent) { sendCors(res); res.writeHead(502, { "content-type": "text/plain" }); }
    res.end("proxy error: " + e.message);
  });
  req.pipe(up);
  console.log(`[proxy] ${req.method} ${req.headers.host}${pathname} -> ${target.origin}${target.pathname}`);
}

// The seek answer names the game socket's host. The official client connects to wss://<host>/ws
// (prod hosts drop the port), so the host becomes the bridge's test name, which Chromium maps to the
// TLS listener; the upgrade is then forwarded to the bridge (tls.on("upgrade") in serve).
function rewriteSeek(body, status) {
  if (status !== 200) return body;
  let seek;
  try { seek = JSON.parse(body.toString("utf8")); } catch (e) { return body; }
  if (!seek || typeof seek.host !== "string") return body;
  console.log(`[bridge] seek-game ${seek.host}:${seek.port} -> ${cfg.bridge.host}:443 (bridge ${cfg.bridge.port})`);
  seek.host = cfg.bridge.host;
  seek.port = 443;
  return Buffer.from(JSON.stringify(seek));
}

// The official index.html (the menu's DOM) with its loader module (static/index-*.js, the WASM loader)
// removed, and loader.js inlined before </body>. The bundle then starts from the token path only.
function sketchPage(res) {
  try {
    const html = fs.readFileSync(path.join(cfg.wwwDir, "index.html"), "utf8");
    const module = /<script type="module" crossorigin src="\.\/static\/index-[^"]*"><\/script>/;
    if (!module.test(html) || !html.includes("</body>")) throw new Error("index.html: the loader module is not where the harness expects it");
    const loader = fs.readFileSync(path.join(HERE, "loader.js"), "utf8");
    const page = html.replace(module, "").replace("</body>", `<script>\n${loader}</script></body>`);
    res.writeHead(200, { "content-type": MIME[".html"], "cache-control": "no-cache" });
    res.end(page);
  } catch (e) {
    console.log(`[sketch] ${e.message}`);
    res.writeHead(500, { "content-type": "text/plain" });
    res.end(e.message);
  }
}

// isMatchmakerHost is the official host name a request came in on, or "" for the page itself.
function handle(req, res, isMatchmakerHost) {
  const url = new URL(req.url, "http://x");
  sendCors(res);
  if (req.method === "OPTIONS") { res.writeHead(204); return res.end(); }
  if (isMatchmakerHost) {
    // hostRoutes[host] is the brain path prefix for that official host ("/mm" for matchmaker,
    // "" for the account/data API, which the brain answers at its root).
    const prefix = cfg.hostRoutes[isMatchmakerHost];
    const p = url.pathname.startsWith(prefix + "/") ? url.pathname : prefix + url.pathname;
    return proxy(req, res, p, cfg.bridge && p.endsWith("/seek-game") ? rewriteSeek : null);
  }
  if (url.pathname.startsWith("/mm/")) return proxy(req, res, url.pathname);
  let rel = decodeURIComponent(url.pathname);
  // "/" is the official page with the sketch loader (loader.js) in place of its WASM loader module.
  // cfg.loader "wasm" serves the official index.html unchanged.
  if (rel === "/" && cfg.loader !== "wasm") return sketchPage(res);
  if (rel === "/") rel = "/index.html";
  const file = path.join(cfg.wwwDir, rel);
  if (!file.startsWith(cfg.wwwDir + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    console.log(`[404] ${req.method} ${url.pathname}`);
    res.writeHead(404, { "content-type": "text/plain" });
    return res.end("404 " + url.pathname);
  }
  res.writeHead(200, { "content-type": MIME[path.extname(file)] || "application/octet-stream", "cache-control": "no-cache" });
  fs.createReadStream(file).pipe(res);
}

function serve() {
  const http1 = http.createServer((req, res) => handle(req, res, false));
  http1.listen(cfg.port, "127.0.0.1", () => {
    console.log(`krunker-local: http://127.0.0.1:${cfg.port}/  (Host ${cfg.localHost})  www=${cfg.wwwDir}`);
  });
  const tls = https.createServer({
    key: fs.readFileSync(path.join(cfg.runDir, "tls-key.pem")),
    cert: fs.readFileSync(path.join(cfg.runDir, "tls-cert.pem")),
  }, (req, res) => {
    const host = (req.headers.host || "").split(":")[0];
    handle(req, res, Object.hasOwn(cfg.hostRoutes, host) ? host : "");
  });
  // The bridge's host: a WebSocket upgrade is handed to net-bridge as a raw byte stream (its /ws).
  tls.on("upgrade", (req, socket, head) => {
    const host = (req.headers.host || "").split(":")[0];
    if (!cfg.bridge || host !== cfg.bridge.host) { socket.destroy(); return; }
    const up = net.connect(cfg.bridge.port, "127.0.0.1", () => {
      let raw = `${req.method} ${req.url} HTTP/${req.httpVersion}\r\n`;
      for (let i = 0; i < req.rawHeaders.length; i += 2) raw += `${req.rawHeaders[i]}: ${req.rawHeaders[i + 1]}\r\n`;
      up.write(raw + "\r\n");
      if (head && head.length) up.write(head);
      socket.pipe(up);
      up.pipe(socket);
    });
    up.on("error", (e) => { console.log(`[bridge] upstream ${e.message}`); socket.destroy(); });
    socket.on("error", () => up.destroy());
    console.log(`[bridge] ws ${req.url} -> 127.0.0.1:${cfg.bridge.port}`);
  });
  tls.listen(cfg.tlsPort, "127.0.0.1", () => {
    console.log(`krunker-local: https://127.0.0.1:${cfg.tlsPort}/ for ${Object.keys(cfg.hostRoutes).join(", ")} -> brain ${cfg.brainUrl}`);
  });
}

function findChrome() {
  if (process.env.KN_KRUNKER_CHROME) return process.env.KN_KRUNKER_CHROME;
  const cache = path.join(os.homedir(), ".cache/ms-playwright");
  if (fs.existsSync(cache)) {
    for (const d of fs.readdirSync(cache).filter((x) => x.startsWith("chromium-")).sort().reverse()) {
      const bin = path.join(cache, d, "chrome-linux64", "chrome");
      if (fs.existsSync(bin)) return bin;
    }
  }
  for (const b of ["chromium-browser", "chromium", "google-chrome"]) {
    for (const dir of (process.env.PATH || "").split(":")) {
      if (fs.existsSync(path.join(dir, b))) return path.join(dir, b);
    }
  }
  throw new Error("no Chromium found (set KN_KRUNKER_CHROME)");
}

function getJSON(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (r) => {
      let d = "";
      r.on("data", (c) => (d += c));
      r.on("end", () => { try { resolve(JSON.parse(d)); } catch (e) { reject(e); } });
    }).on("error", reject);
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function freePort() {
  return new Promise((resolve) => {
    const s = net.createServer();
    s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => resolve(p)); });
  });
}

// Chromium host rules: the page's .test name, the official matchmaker/API hosts and the bridge's host
// all resolve to the local listeners (the port in a MAP rule is the listener's).
function hostRules() {
  return [`MAP ${cfg.localHost} 127.0.0.1:${cfg.port}`,
    ...Object.keys(cfg.hostRoutes).map((h) => `MAP ${h} 127.0.0.1:${cfg.tlsPort}`),
    ...(cfg.bridge ? [`MAP ${cfg.bridge.host} 127.0.0.1:${cfg.tlsPort}`] : []),
    // every other host (the official CDNs, analytics, consent vendors, the real krunker.io hosts) does not resolve:
    // the page cannot reach production, and a request for one fails in the report's "failed" list
    "MAP * ~NOTFOUND"];
}

// Query values that carry tokens or ids are not printed.
function redact(u) {
  return String(u).replace(/((?:clientKey|at|clientUID|bpt|validationToken|accessToken|dataQuery)=)[^&]*/g, "$1…");
}

// Headless load of the served client. Reports 404s, failed or >=400 requests, matchmaker calls,
// console errors and exceptions as one JSON document on stdout, and writes a screenshot.
async function check() {
  const shots = path.join(cfg.runDir, "shots");
  fs.mkdirSync(shots, { recursive: true });
  const chrome = findChrome();
  const cdpPort = await freePort();
  const profile = path.join(cfg.runDir, "chrome-profile");
  // The client decides its matchmaker from its hostname, so the page is served from a .test name.
  // The official matchmaker hostnames resolve to the HTTPS side of serve (port tlsPort).
  const rules = hostRules();
  const child = spawn(chrome, [
    "--headless=new", "--no-sandbox", "--disable-dev-shm-usage", "--use-gl=angle",
    "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--window-size=1280,720",
    `--host-resolver-rules=${rules.join(", ")}`, "--ignore-certificate-errors",
    `--user-data-dir=${profile}`, `--remote-debugging-port=${cdpPort}`, "about:blank",
  ], { stdio: "ignore", env: { ...process.env, DISPLAY: "", WAYLAND_DISPLAY: "" } });
  try {
    let targets = [];
    for (let i = 0; i < 60 && !targets.some((t) => t.type === "page"); i++) {
      try { targets = await getJSON(`http://127.0.0.1:${cdpPort}/json/list`); } catch (e) { /* not up yet */ }
      await sleep(250);
    }
    const page = targets.find((t) => t.type === "page");
    if (!page) throw new Error("no page target from Chromium");
    const ws = new WebSocket(page.webSocketDebuggerUrl);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
    let id = 0;
    const pending = new Map();
    const reqs = new Map();
    const report = { url: cfg.localUrl, notFound: [], failed: [], httpErrors: [], matchmakerCalls: [],
      localRequests: [], consoleErrors: [], exceptions: [], menu: null };
    ws.onmessage = (m) => {
      const msg = JSON.parse(m.data);
      if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); return; }
      const p = msg.params || {};
      const officialCall = (u) => Object.keys(cfg.hostRoutes).some((h) => u.includes("//" + h + "/"));
      switch (msg.method) {
        case "Network.requestWillBeSent": {
          const u = new URL(p.request.url);
          reqs.set(p.requestId, p.request.url);
          if (u.hostname === cfg.localHost) report.localRequests.push(u.pathname);
          break;
        }
        case "Network.responseReceived": {
          const st = p.response.status;
          const u = p.response.url;
          if (st >= 400) report.httpErrors.push(`${st} ${u}`);
          if (st === 404) report.notFound.push(u);
          if (officialCall(u)) report.matchmakerCalls.push(`${st} ${u}`);
          break;
        }
        case "Network.loadingFailed": report.failed.push(`${reqs.get(p.requestId) || p.requestId}: ${p.errorText}`); break;
        case "Runtime.exceptionThrown": report.exceptions.push((p.exceptionDetails.exception || {}).description || p.exceptionDetails.text); break;
        case "Runtime.consoleAPICalled": if (p.type === "error") report.consoleErrors.push(p.args.map((a) => a.value ?? a.description).join(" ").slice(0, 300)); break;
        case "Log.entryAdded": if (p.entry.level === "error" && p.entry.source !== "network") report.consoleErrors.push(p.entry.text.slice(0, 300)); break;
      }
    };
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; pending.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
    await send("Page.enable"); await send("Network.enable"); await send("Runtime.enable"); await send("Log.enable");
    await send("Page.navigate", { url: cfg.localUrl });
    await sleep(Number(process.env.KN_KRUNKER_LOAD_MS) || cfg.loadWaitMs);
    const globals = await send("Runtime.evaluate", { expression: `({ game: typeof window.Game, render: typeof window.RENDER, loader: typeof window.__wasm, spinners: document.querySelectorAll(".loader,.spinner,[class*=load]").length })`, returnByValue: true });
    report.globals = globals.result && globals.result.result ? globals.result.result.value : null;
    // The official consent dialog sits over the menu until accepted; accept it as a user would.
    const acc = await send("Runtime.evaluate", { expression: `(() => { const b = [...document.querySelectorAll("button")].find(x => /^\\s*I Accept\\s*$/i.test(x.innerText)); if (!b) return false; b.click(); return true; })()`, returnByValue: true });
    report.consentAccepted = !!(acc.result && acc.result.result && acc.result.result.value);
    if (report.consentAccepted) await sleep(cfg.afterAcceptMs);
    const ev = await send("Runtime.evaluate", { expression: `(() => { const t = document.body ? document.body.innerText.replace(/\\s+/g, " ").trim() : ""; return { title: document.title, textChars: t.length, sample: t.slice(0, 120), canvases: document.querySelectorAll("canvas").length, visibleButtons: [...document.querySelectorAll("button,.button,[role=button]")].filter(b => b.offsetParent).length }; })()`, returnByValue: true });
    report.menu = ev.result && ev.result.result ? ev.result.result.value : ev;
    const shot = await send("Page.captureScreenshot", { format: "png" });
    const shotPath = path.join(shots, "menu.png");
    if (shot.result && shot.result.data) fs.writeFileSync(shotPath, Buffer.from(shot.result.data, "base64"));
    report.screenshot = shotPath;
    ws.close();
    console.log(JSON.stringify(report, null, 2));
  } finally {
    try { child.kill("SIGTERM"); } catch (e) { /* already gone */ }
  }
}

// join: the official menu to a game on the bridge. Accepts consent, clicks Quick Play (or Play), then
// watches the client's sockets and the screen until the game is on screen or joinWaitMs runs out.
// Screenshots go to runDir/shots/join-<step>.png; the JSON report lists the sockets (tokens redacted),
// the seek answers, frame counts, errors, and the bridge's log lines.
// the accept control: the deepest element labelled "I Accept" (a button or a styled div), in the page
const ACCEPT_JS = `(() => {
  const re = /^\\s*I Accept\\s*$/i;
  const hits = [...document.querySelectorAll("button, [role=button], div, span, a")].filter((x) => re.test(x.innerText || ""));
  const b = hits.find((x) => ![...x.children].some((c) => re.test(c.innerText || ""))) || hits[0];
  if (!b) return false;
  b.click();
  return b.tagName + "." + String(b.className).slice(0, 40);
})()`;
// consentBlock is position: fixed, so offsetParent is null for it: test the computed box instead
// Logs every WebSocket.close the page makes, with the call site (the bundle's own names, the source is game.js):
// the report's "hooks" list says who closed the game socket. It only logs; the call goes on as it was.
const WS_CLOSE_HOOK = `(() => { const close = WebSocket.prototype.close; WebSocket.prototype.close = function (...a) {
  try { console.log("[hook] ws.close " + JSON.stringify(a) + " " + String(new Error().stack).split("\\n").slice(1, 7).join(" | ")); } catch (e) {}
  return close.apply(this, a); }; })();`;
// The breakpoint probe: runs inside the paused frame of the init handler (the bundle's own names, its decoder
// IïIììíí maps the hex keys). Reports the keys and the values the disconnect test reads. Nothing is changed.
const PAUSE_PROBE_JS = `(() => {
  const out = {};
  const k = (x) => { try { return IïIììíí(x); } catch (e) { return "k?" + e.message; } };
  try {
    out.fn = typeof IïIììíí; out.key1994 = k(0x1994); out.key1f6 = k(0x1f6);
    out.cfgType = typeof iîÍiìïì;
    out.cfgKeys = iîÍiìïì && typeof iîÍiìïì === "object" ? Object.keys(iîÍiìïì).slice(0, 40) : null;
    out.v1994 = iîÍiìïì ? String(iîÍiìïì[k(0x1994)]) : null;
    out.gameFlag = String(iiïìîii[k(0x1f6)]);
    out.args = Array.from(arguments).map((a) => a === null ? "null" : typeof a + (Array.isArray(a) ? "[" + a.length + "]" : ""));
  } catch (e) { out.err = String(e.message); }
  return JSON.stringify(out);
})()`;
// The exception probe: the frame that threw (the bundle's own names). The lookup that fails is modes[index].
const EXC_PROBE_JS = `(() => {
  const o = {};
  try { o.idx = [typeof IÌíiîïì === "undefined" ? "undef" : IÌíiîïì, typeof IIíïiíí === "undefined" ? "undef" : typeof IIíïiíí]; } catch (e) { o.e1 = e.message; }
  try { o.modes = iìiiìíi.length; } catch (e) { o.e2 = e.message; }
  try { o.self = this && typeof this === "object" ? [this.modeIndex, this.mapIndex, this.eventIndex] : String(this); } catch (e) { o.e3 = e.message; }
  return JSON.stringify(o);
})()`;
const INIT_HANDLER_COLUMN = 10307050; // the disconnect call in the init handler (game.js line 1, 0-based column)
const CONSENT_OPEN_JS =`(() => { const e = document.getElementById("consentBlock"); if (!e) return false; const s = getComputedStyle(e); return s.display !== "none" && s.visibility !== "hidden" && e.getBoundingClientRect().height > 0; })()`;
const CLICK_PLAY_JS = `(() => {
  const els = [...document.querySelectorAll("button, [role=button], .button, a, div")]
    .filter((e) => e.offsetParent && e.innerText && e.innerText.trim().length < 40);
  // v10's menu button is "QUICK MATCH" (the bolt icon is text too); the deepest element with the label is clicked
  for (const re of [/quick\\s*match/i, /^\\s*play\\s*$/i]) {
    const hits = els.filter((x) => re.test(x.innerText));
    const e = hits.find((x) => ![...x.children].some((c) => re.test(c.innerText))) || hits[0];
    if (e) { e.click(); return re.source + " -> " + e.tagName + "." + String(e.className).slice(0, 60); }
  }
  return "none: " + els.slice(0, 60).map((e) => e.innerText.trim()).join(" | ").slice(0, 400);
})()`;
const STATE_JS = `(() => ({
  game: typeof window.Game, render: typeof window.RENDER,
  canvases: [...document.querySelectorAll("canvas")].map((c) => c.width + "x" + c.height).join(","),
  text: (document.body.innerText || "").replace(/\\s+/g, " ").slice(0, 160),
}))()`;

function bridgeLines() {
  if (!cfg.bridge.log || !fs.existsSync(cfg.bridge.log)) return [];
  return fs.readFileSync(cfg.bridge.log, "utf8").split("\n").filter((l) => /v10-proxy|net-bridge|\[bridge\]/.test(l)).slice(-40);
}

async function join() {
  if (!cfg.bridge) throw new Error('join needs the "bridge" config');
  const shots = path.join(cfg.runDir, "shots");
  fs.mkdirSync(shots, { recursive: true });
  const cdpPort = await freePort();
  const child = spawn(findChrome(), [
    "--headless=new", "--no-sandbox", "--disable-dev-shm-usage", "--use-gl=angle",
    "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--window-size=1280,720",
    `--host-resolver-rules=${hostRules().join(", ")}`, "--ignore-certificate-errors",
    `--user-data-dir=${path.join(cfg.runDir, "join-profile")}`, `--remote-debugging-port=${cdpPort}`, "about:blank",
  ], { stdio: "ignore", env: { ...process.env, DISPLAY: "", WAYLAND_DISPLAY: "" } });
  const report = { url: cfg.localUrl, bridge: `${cfg.bridge.host} -> 127.0.0.1:${cfg.bridge.port}`, consentAccepted: false,
    clicked: null, seek: [], sockets: [], framesIn: 0, framesOut: 0, bytesIn: 0, consoleErrors: [], exceptions: [],
    failed: [], httpErrors: [], hooks: [], pauses: [], breakpoint: null, timeline: [], state: null, shots: [], bridgeLog: [] };
  const reqUrl = new Map();
  let appScript = null;
  const hostsSeen = new Set();
  try {
    let targets = [];
    for (let i = 0; i < 60 && !targets.some((t) => t.type === "page"); i++) {
      try { targets = await getJSON(`http://127.0.0.1:${cdpPort}/json/list`); } catch (e) { /* not up yet */ }
      await sleep(250);
    }
    const page = targets.find((t) => t.type === "page");
    if (!page) throw new Error("no page target from Chromium");
    const ws = new WebSocket(page.webSocketDebuggerUrl);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
    let id = 0;
    const pending = new Map();
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; pending.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
    ws.onmessage = (m) => {
      const msg = JSON.parse(m.data);
      if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); return; }
      const p = msg.params || {};
      switch (msg.method) {
        case "Network.webSocketCreated": report.sockets.push(redact(p.url)); break;
        case "Network.webSocketFrameSent": report.framesOut++; break;
        case "Network.webSocketFrameReceived": report.framesIn++; report.bytesIn += Buffer.from(p.response.payloadData, "base64").length; break;
        case "Network.requestWillBeSent":
          reqUrl.set(p.requestId, p.request.url);
          hostsSeen.add(new URL(p.request.url).hostname);
          break;
        case "Network.loadingFailed": report.failed.push(`${redact(reqUrl.get(p.requestId) || p.requestId).slice(0, 120)}: ${p.errorText}`); break;
        case "Network.responseReceived":
          if (/seek-game/.test(p.response.url)) report.seek.push(`${p.response.status} ${redact(p.response.url)}`);
          if (p.response.status >= 400) report.httpErrors.push(`${p.response.status} ${redact(p.response.url).slice(0, 120)}`);
          break;
        case "Runtime.exceptionThrown": {
          const d = p.exceptionDetails;
          const frames = ((d.stackTrace && d.stackTrace.callFrames) || []).slice(0, 3).map((f) => `${f.functionName || "?"}@${f.lineNumber}:${f.columnNumber}`);
          report.exceptions.push(`${((d.exception || {}).description || d.text || "").split("\n")[0].slice(0, 160)} [${frames.join(" < ")}]`);
          break;
        }
        case "Debugger.scriptParsed":
          if (/krunker\.io\/js\/app\.js$/.test(p.url || "") && !appScript) {
            appScript = p.scriptId;
            send("Debugger.setBreakpoint", { location: { scriptId: appScript, lineNumber: 2, columnNumber: INIT_HANDLER_COLUMN } })
              .then((r) => { report.breakpoint = r.result ? r.result.actualLocation : r; });
          }
          break;
        case "Debugger.paused": {
          const top = p.callFrames[0];
          if (p.reason === "exception") {
            (async () => {
              const r = await send("Debugger.evaluateOnCallFrame", { callFrameId: top.callFrameId, expression: EXC_PROBE_JS, returnByValue: true });
              report.pauses.push({ exception: top.functionName, probe: r.result && r.result.result ? r.result.result.value : r.error || r });
              await send("Debugger.resume");
            })();
            break;
          }
          (async () => {
            if (report.pauses.length < 6) {
              const r = await send("Debugger.evaluateOnCallFrame", { callFrameId: top.callFrameId, expression: PAUSE_PROBE_JS, returnByValue: true });
              report.pauses.push({ fn: top.functionName, probe: r.result && r.result.result ? r.result.result.value : r });
            }
            await send("Debugger.resume");
          })();
          break;
        }
        case "Runtime.consoleAPICalled": {
          const text = p.args.map((a) => a.value ?? a.description).join(" ");
          if (p.type === "error") report.consoleErrors.push(text.slice(0, 200));
          if (text.startsWith("[hook] ")) report.hooks.push(text.slice(7, 600));
          break;
        }
      }
    };
    const evalJS = async (expression) => {
      const r = await send("Runtime.evaluate", { expression, returnByValue: true });
      return r.result && r.result.result ? r.result.result.value : null;
    };
    const shot = async (step) => {
      const r = await send("Page.captureScreenshot", { format: "png" });
      const file = path.join(shots, `join-${step}.png`);
      if (r.result && r.result.data) { fs.writeFileSync(file, Buffer.from(r.result.data, "base64")); report.shots.push(file); }
    };
    await send("Page.enable"); await send("Network.enable"); await send("Runtime.enable");
    await send("Page.addScriptToEvaluateOnNewDocument", { source: WS_CLOSE_HOOK });
    await send("Debugger.enable");
    await send("Debugger.setPauseOnExceptions", { state: "uncaught" });
    await send("Page.navigate", { url: cfg.localUrl });
    await sleep(cfg.loadWaitMs);
    // the consent window can come back after an accept (its vendor check finishes late): accept until it stays hidden
    for (let i = 0; i < 10 && (await evalJS(CONSENT_OPEN_JS)); i++) {
      report.consentAccepted = !!(await evalJS(ACCEPT_JS)) || report.consentAccepted;
      await sleep(2000);
    }
    report.consentOpenAfter = await evalJS(CONSENT_OPEN_JS);
    report.consentDom = await evalJS(`(() => { const e = document.getElementById("consentBlock"); const s = e && getComputedStyle(e);
      return { exists: !!e, display: s && s.display, visibility: s && s.visibility, z: s && s.zIndex, rect: e && JSON.stringify(e.getBoundingClientRect()),
        html: e && e.outerHTML.slice(0, 160), iframes: document.querySelectorAll("iframe").length, dialogs: document.querySelectorAll("dialog").length }; })()`);
    await sleep(cfg.afterAcceptMs);
    await shot("1-menu");
    report.clicked = await evalJS(CLICK_PLAY_JS);
    await sleep(3000);
    await shot("2-clicked");
    // a timeline of the join: the page's text, the seeks and sockets so far, and the frames received
    const t0 = Date.now();
    const deadline = t0 + (cfg.joinWaitMs || 90000);
    while (Date.now() < deadline) {
      await sleep(3000);
      report.state = await evalJS(STATE_JS);
      report.timeline.push([Math.round((Date.now() - t0) / 1000), report.state && report.state.text.slice(0, 48),
        report.seek.length, report.sockets.length, report.framesIn]);
      if (report.state && report.state.inGame) break;
    }
    report.topAt = await evalJS(`(() => { const e = document.elementFromPoint(640, 240); return e ? e.tagName + "#" + e.id + "." + String(e.className).slice(0, 60) : null; })()`);
    report.iframes = await evalJS(`[...document.querySelectorAll("iframe")].map((f) => (f.src || "").slice(0, 80) + " " + JSON.stringify(f.getBoundingClientRect()))`);
    report.visibleButtons = await evalJS(`[...document.querySelectorAll("button,[role=button],.button,a")].filter((e) => e.getBoundingClientRect().width > 0 && getComputedStyle(e).display !== "none").map((e) => e.innerText.trim().slice(0, 30)).filter(Boolean).slice(0, 80)`);
    await shot("3-final");
    report.bridgeLog = bridgeLines();
    // every host the page asked for: the local ones only (the rest fail with NOTFOUND, see hostRules)
    const local = new Set([cfg.localHost, "localhost", "127.0.0.1", cfg.bridge.host, ...Object.keys(cfg.hostRoutes)]);
    report.hosts = [...hostsSeen].sort();
    report.nonLocalHosts = report.hosts.filter((h) => !local.has(h));
    ws.close();
    console.log(JSON.stringify(report, null, 2));
  } finally {
    try { child.kill("SIGTERM"); } catch (e) { /* already gone */ }
  }
}

function help() {
  console.log(`usage: krunker-local.mjs prepare|serve|check|join   (config: ${cfg._file})`);
}

const cmds = { prepare, serve, check, join, help };
if (!cmds[cmd]) { help(); process.exit(2); }
Promise.resolve(cmds[cmd]()).catch((e) => { console.error("krunker-local:", e.message); process.exit(1); });
