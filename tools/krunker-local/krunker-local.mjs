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
function proxy(req, res, pathname) {
  const target = new URL(cfg.brainUrl + pathname + new URL(req.url, "http://x").search);
  const headers = { ...req.headers, host: target.host };
  delete headers.origin;
  delete headers.referer;
  const up = https.request(target, { method: req.method, headers }, (ur) => {
    const h = { ...ur.headers };
    delete h["access-control-allow-origin"];
    sendCors(res);
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
    return proxy(req, res, p);
  }
  if (url.pathname.startsWith("/mm/")) return proxy(req, res, url.pathname);
  let rel = decodeURIComponent(url.pathname);
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
  const rules = [`MAP ${cfg.localHost} 127.0.0.1:${cfg.port}`,
    ...Object.keys(cfg.hostRoutes).map((h) => `MAP ${h} 127.0.0.1:${cfg.tlsPort}`)];
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

function help() {
  console.log(`usage: krunker-local.mjs prepare|serve|check   (config: ${cfg._file})`);
}

const cmds = { prepare, serve, check, help };
if (!cmds[cmd]) { help(); process.exit(2); }
Promise.resolve(cmds[cmd]()).catch((e) => { console.error("krunker-local:", e.message); process.exit(1); });
