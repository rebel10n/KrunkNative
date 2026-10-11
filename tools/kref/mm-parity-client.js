// Real v3.1.1 client half of mm-parity.js: the jail's public/ site (static, no game
// server) in headless Chromium, its matchmaker traffic captured with CDP Fetch
// interception and answered by the brain under test (the reference behind its nginx
// emulation, or kn_brain behind deploy/nginx's rewrite), so the page sees exactly
// what that brain sends (status, headers incl. CORS, body). Region /ping requests
// (game nodes in v3.1.1, kn_brain or brain.regions[].ping_host in KrunkNative) are
// answered locally: only the de-fra host answers, so getRegion() picks de-fra.
"use strict";
const { spawn } = require("child_process");
const fs = require("fs");
const http = require("http");
const net = require("net");
const os = require("os");
const path = require("path");
const { CDP, sleep } = require("./cdp.js");

const REF = process.env.KRUNKER_REF || "/home/user/jails/kru/src/Krunker_v3.1.1";
const PUBLIC = path.join(REF, "public");
// hook.js (mod/objects.ts) points matchmakerURL at location.origin + "/mm" and
// socialURL / apiURL at location.origin + "/api": same origin as the site.
const MM_PATH = /^(?:\/mm)?\/(ping-list|game-list|game-info|seek-game|host-game|queue\/[a-z-]+)$|^\/(?:social|api)\/player-count$/;
const TYPES = { ".html": "text/html", ".js": "application/javascript", ".css": "text/css", ".png": "image/png", ".jpg": "image/jpeg",
  ".svg": "image/svg+xml", ".json": "application/json", ".woff2": "font/woff2", ".woff": "font/woff", ".ttf": "font/ttf",
  ".mp3": "audio/mpeg", ".ogg": "audio/ogg", ".obj": "text/plain", ".gif": "image/gif", ".webp": "image/webp", ".ico": "image/x-icon" };

function freePort() {
  return new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
}

// The site, and a stand-in game server that records the game socket the client opens.
function siteServer(port, sockets) {
  const srv = http.createServer((req, res) => {
    let p = decodeURIComponent(req.url.split("?")[0]);
    if (p === "/") p = "/index.html";
    const file = path.join(PUBLIC, path.normalize(p).replace(/^(\.\.[/\\])+/, ""));
    if (!file.startsWith(PUBLIC) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { "Content-Type": TYPES[path.extname(file)] || "application/octet-stream" });
    fs.createReadStream(file).pipe(res);
  });
  srv.on("upgrade", (req, sock) => { sockets.push(req.url); sock.destroy(); });
  return new Promise((r) => srv.listen(port, "127.0.0.1", () => r(srv)));
}

function forward(port, method, p) {
  return new Promise((res) => {
    const r = http.request({ host: "127.0.0.1", port, path: p, method, headers: { "accept-language": "en-US,en;q=0.6;", origin: "http://127.0.0.1" } }, (s) => {
      const chunks = [];
      s.on("data", (d) => chunks.push(d));
      s.on("end", () => res({ status: s.statusCode, headers: s.headers, body: Buffer.concat(chunks) }));
    });
    r.on("error", () => res(null));
    r.end();
  });
}

async function drive(label, mmPort, verbose) {
  const sitePort = await freePort(), cdpPort = await freePort();
  const sockets = [], log = [];
  const site = await siteServer(sitePort, sockets);
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "kn-mmp-chrome-"));
  const env = { ...process.env };
  delete env.DISPLAY; delete env.WAYLAND_DISPLAY; // headless only, never the user's display
  const chrome = spawn("chromium-browser", ["--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
    "--enable-unsafe-swiftshader", "--window-size=1280,720", `--remote-debugging-port=${cdpPort}`, `--user-data-dir=${profile}`,
    "--mute-audio", "--disable-background-timer-throttling", "about:blank"], { env, stdio: "ignore", detached: true });
  const out = { label, requests: log, sockets, screens: {} };
  let c;
  try {
    c = await CDP.connect(cdpPort);
    await c.send("Page.enable");
    await c.send("Runtime.enable");
    await c.send("Network.enable");
    await c.send("Page.addScriptToEvaluateOnNewDocument", { source: fs.readFileSync(path.join(__dirname, "shim.js"), "utf8") });
    await c.send("Fetch.enable", { patterns: [{ urlPattern: "*", requestStage: "Request" }] });
    c.on(async (m) => {
      if (verbose && (m.method === "Runtime.consoleAPICalled" || m.method === "Runtime.exceptionThrown"))
        console.error(`[${label}] console`, JSON.stringify(m.params.args ? m.params.args.map((a) => a.value || a.description) : m.params.exceptionDetails).slice(0, 300));
      if (m.method === "Network.webSocketCreated") sockets.push(m.params.url.replace(/^wss?:/, ""));
      if (m.method !== "Fetch.requestPaused") return;
      const { requestId, request } = m.params;
      const u = new URL(request.url);
      if (verbose) console.error(`[${label}] ${request.method} ${request.url.slice(0, 160)}`);
      try {
        if (u.pathname === "/ping") {
          // a region ping: only the de-fra host answers
          log.push({ path: "/ping", host: u.host });
          if (u.hostname === "fra.mmparity.invalid") {
            await c.send("Fetch.fulfillRequest", { requestId, responseCode: 200, responseHeaders: [{ name: "Content-Type", value: "application/json" },
              { name: "Access-Control-Allow-Origin", value: "*" }], body: Buffer.from('{"success":"true"}').toString("base64") });
          } else await c.send("Fetch.failRequest", { requestId, errorReason: "ConnectionRefused" });
          return;
        }
        if (MM_PATH.test(u.pathname)) {
          // the client form (no /mm): the reference front adds it, kn_brain has both
          const route = u.pathname.replace(/^\/mm\//, "/");
          const r = await forward(mmPort, request.method, route + u.search);
          log.push({ method: request.method, path: u.pathname, query: [...u.searchParams.entries()], status: r && r.status, body: r && r.body.toString() });
          if (!r) { await c.send("Fetch.failRequest", { requestId, errorReason: "ConnectionRefused" }); return; }
          const responseHeaders = [];
          for (const [k, v] of Object.entries(r.headers)) {
            if (/^(connection|keep-alive|transfer-encoding)$/i.test(k)) continue;
            for (const one of [].concat(v)) responseHeaders.push({ name: k, value: String(one) });
          }
          await c.send("Fetch.fulfillRequest", { requestId, responseCode: r.status, responseHeaders, body: r.body.toString("base64") });
          return;
        }
        await c.send("Fetch.continueRequest", { requestId });
      } catch (e) { if (verbose) console.error(label, "intercept", e.message); }
    });
    await c.setViewport(1280, 720);

    const text = `(()=>{const e=document.getElementById('instructions');return e?e.textContent.trim().slice(0,80):''})()`;
    // 1. startup seek (no ?game): ping-list -> pings -> seek-game -> game socket
    await c.send("Page.navigate", { url: `http://127.0.0.1:${sitePort}/` });
    for (let i = 0; i < 120 && !sockets.length; i++) await sleep(250);
    await sleep(500);
    out.screens.startup = await c.eval(text);
    // 2. Servers window: game-list + player-count; checkedSwitchServer -> game-info
    try {
      await c.eval("window.showWindow(2), 1");
      await sleep(2500);
      out.screens.servers = await c.eval(`(()=>{const e=document.getElementById('serverHolder')||document.getElementById('menuWindow');return e?e.textContent.replace(/\\s+/g,' ').trim().slice(0,200):''})()`);
      await c.eval("(()=>{const s=document.createElement('span');document.body.appendChild(s);window.checkedSwitchServer('NY:bbbbb', s);window.__pc=s;return 1})()");
      await sleep(1500);
      out.screens.checkedFull = await c.eval("window.__pc.textContent");
    } catch (e) { out.screens.servers = "ERR " + e.message; }
    // 3. ?game=<full lobby> (autoChangeGame=false), 4. ?game=<unknown id>
    for (const [k, g] of [["joinFull", "NY:bbbbb"], ["joinUnknown", "XX:nope"], ["joinRoom", "NY:aaaaa"]]) {
      const before = sockets.length;
      await c.send("Page.navigate", { url: `http://127.0.0.1:${sitePort}/?game=${encodeURIComponent(g)}` });
      for (let i = 0; i < 40 && sockets.length === before; i++) await sleep(250);
      await sleep(1000);
      out.screens[k] = (await c.eval(text)) + (sockets.length > before ? `  [socket ${sockets[sockets.length - 1]}]` : "");
    }
  } finally {
    if (c) c.close();
    try { process.kill(-chrome.pid, "SIGKILL"); } catch (_) {}
    site.close();
    await sleep(300);
    fs.rmSync(profile, { recursive: true, force: true });
  }
  return out;
}

function norm(r) {
  if (!r.method) return `GET ${r.path} @${r.host}`;
  const q = r.query.map(([k, v]) => `${k}=${k === "hostname" ? "<host>" : v}`).join("&");
  let body = r.body;
  try { const j = JSON.parse(body); if (j && j.clientId) j.clientId = "<clientId>"; delete j.region; delete j.webUrl; body = JSON.stringify(j); } catch (_) {}
  return `${r.method} ${r.path}?${q} -> ${r.status} ${body.slice(0, 160)}`;
}

async function run({ refFront, kn, verbose, nodes }) {
  console.log("real v3.1.1 client (headless Chromium):");
  const a = await drive("reference", refFront, verbose);
  const b = await drive("kn_brain", kn, verbose);
  const ra = a.requests.map(norm), rb = b.requests.map(norm);
  const n = Math.max(ra.length, rb.length);
  let diffs = 0;
  for (let i = 0; i < n; i++) {
    const same = ra[i] === rb[i];
    if (!same) diffs++;
    console.log(`  ${same ? "same" : "DIFF"}  ${ra[i] || "(none)"}${same ? "" : "\n        kn: " + (rb[i] || "(none)")}`);
  }
  for (const k of Object.keys(a.screens)) {
    const same = a.screens[k] === b.screens[k];
    console.log(`  ${same ? "same" : "DIFF"}  screen ${k}: ref "${a.screens[k]}"${same ? "" : `  kn "${b.screens[k]}"`}`);
  }
  return { diffs, a, b };
}

module.exports = { run };
