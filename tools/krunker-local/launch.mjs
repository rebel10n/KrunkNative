#!/usr/bin/env node
// Krunker Box Office: the official Krunker v10.0.3 client, driven from Node through Chromium's DevTools protocol.
//
//   node tools/krunker-local/launch.mjs --smoke    start the stack, play Quick Match, check the join, exit 0 / 1
//   node tools/krunker-local/launch.mjs            the same, then keep the stack up until SIGINT / SIGTERM
//   make krunker-node-smoke / make krunker-node
//
// What starts, each recorded in <run>/pids/<name>.pid and stopped by its own handle (never by process name):
//   kn_brain (sqlite scratch database), one server_bin game server, v10_bridge, the harness page server
//   (tools/krunker-local/krunker-local.mjs serve, with a config written for this run), headless Chromium.
// Node then drives the page over CDP: it accepts the consent dialog, clicks Quick Match, and records every
// WebSocket frame (Network.webSocketFrameSent / Received) to <run>/frames.jsonl, one JSON object per line.
// window.Game and window.RENDER are read with Runtime.evaluate and written to <run>/result.json.
//
// Loopback only: every host this script binds or dials must be 127.0.0.1 or *.krunker-local.test, and it refuses
// to start otherwise. The official hostnames in the harness's hostRoutes are Chromium host-resolver MAP entries to the
// local TLS listener; every other name maps to NOTFOUND, so nothing leaves the machine.
import fs from "node:fs";
import net from "node:net";
import dgram from "node:dgram";
import path from "node:path";
import os from "node:os";
import crypto from "node:crypto";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

// The bundle does not replace this process's global object (no bundle runs here), but keep one reference anyway.
const proc = process;
const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../..");
const BUILD = path.join(REPO, "cmake-build-debug");
const HARNESS = path.join(HERE, "krunker-local.mjs");
const EXAMPLE = path.join(HERE, "config.example.json");
// The page's name. Chromium resolves it to 127.0.0.1 (host-resolver MAP); Node never dials it. KN_NODE_PAGE_HOST picks an
// alias for the client's host-based environment choice (the beta hosts); every other name stays unresolved.
const PAGE_HOST = process.env.KN_NODE_PAGE_HOST || "krunker-local.test";
const BRIDGE_NAME = "kn-bridge.krunker-local.test";

// Every host this script binds or dials. Refused unless loopback.
export function assertLoopback(name, host) {
  const ok = host === "127.0.0.1" || host === PAGE_HOST || host.endsWith(".krunker-local.test");
  if (!ok) throw new Error(`refusing to start: ${name} host ${host} is not 127.0.0.1 or *.krunker-local.test`);
  return host;
}
const HOSTS = {
  brain: assertLoopback("brain", process.env.KN_NODE_BRAIN_HOST || "127.0.0.1"),
  bridge: assertLoopback("bridge", process.env.KN_NODE_BRIDGE_HOST || "127.0.0.1"),
  game: assertLoopback("game server", process.env.KN_NODE_GAME_HOST || "127.0.0.1"),
  page: "127.0.0.1", // the page name is only a MAP alias (never dialled); its target is checked by hostRules
  bridgeName: assertLoopback("bridge name", BRIDGE_NAME),
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => Date.now();

function freePort(kind) {
  return new Promise((resolve, reject) => {
    if (kind === "udp") {
      const u = dgram.createSocket("udp4");
      u.once("error", reject);
      u.bind(0, "127.0.0.1", () => { const p = u.address().port; u.close(() => resolve(p)); });
      return;
    }
    const s = net.createServer();
    s.once("error", reject);
    s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => resolve(p)); });
  });
}
// A port free for both TCP and UDP (a game server listens on both).
async function freeGamePort() {
  for (let i = 0; i < 20; i++) {
    const p = await freePort("tcp");
    try { await freePort("udp"); return p; } catch (e) { /* UDP taken: try another */ }
  }
  throw new Error("no free TCP/UDP port pair");
}
function tcpOpen(port, host) {
  return new Promise((res) => { const s = net.connect(port, host); s.once("connect", () => { s.destroy(); res(true); }); s.once("error", () => res(false)); });
}
async function waitFor(fn, ms, what) {
  const end = now() + ms;
  while (now() < end) {
    try { const v = await fn(); if (v) return v; } catch (e) { /* not up yet */ }
    await sleep(200);
  }
  throw new Error(`timed out waiting for ${what}`);
}
async function getJSON(url) {
  const r = await fetch(url);
  return { status: r.status, body: await r.json().catch(() => null) };
}

// Child processes: spawned here, recorded in pids/<name>.pid, stopped by their handle only.
class Children {
  constructor(runDir) {
    this.runDir = runDir;
    this.list = [];
    fs.mkdirSync(path.join(runDir, "pids"), { recursive: true });
  }
  spawn(name, cmd, args, { env = proc.env, log = `${name}.log`, cwd = REPO } = {}) {
    const out = fs.openSync(path.join(this.runDir, log), "w");
    const child = spawn(cmd, args, { cwd, env, stdio: ["ignore", out, out] });
    fs.closeSync(out);
    fs.writeFileSync(path.join(this.runDir, "pids", name + ".pid"), String(child.pid));
    this.list.push({ name, child });
    return child;
  }
  // Signals only the handles this process holds and removes their pid files now (the exit events may not run).
  stop() {
    for (const { name, child } of this.list.reverse()) {
      if (child.exitCode === null && child.signalCode === null) child.kill("SIGTERM");
      try { fs.rmSync(path.join(this.runDir, "pids", name + ".pid")); } catch (e) { /* already removed */ }
    }
    this.list = [];
  }
}

function findChrome() {
  if (proc.env.KN_KRUNKER_CHROME) return proc.env.KN_KRUNKER_CHROME;
  const cache = path.join(os.homedir(), ".cache/ms-playwright");
  if (fs.existsSync(cache)) {
    for (const d of fs.readdirSync(cache).filter((x) => x.startsWith("chromium-")).sort().reverse()) {
      const bin = path.join(cache, d, "chrome-linux64", "chrome");
      if (fs.existsSync(bin)) return bin;
    }
  }
  throw new Error("no Chromium found (set KN_KRUNKER_CHROME)");
}

// Starts kn_brain, one server_bin and v10_bridge on free loopback ports. Returns their addresses.
async function startStack(kids, runDir, secret) {
  const db = JSON.parse(spawnSync("python3", ["-I", path.join(REPO, "services/brain/tests/db_scratch.py"), REPO, "create", "sqlite",
    "kn_node_" + proc.pid], { encoding: "utf8" }).stdout || "null");
  if (!db) throw new Error("no scratch database (services/brain/tests/db_scratch.py create sqlite)");
  const env = { ...proc.env, KN_DATABASE_DRIVER: "sqlite", KN_DATABASE_PATH: db.path, KN_BRAIN_INTERNAL_SECRET: secret };
  const brainBin = path.join(BUILD, "services/brain/kn_brain");
  if (spawnSync(brainBin, ["--migrate-only"], { cwd: REPO, env, stdio: "ignore" }).status !== 0) throw new Error("kn_brain --migrate-only failed");
  const brainPort = await freePort("tcp");
  kids.spawn("brain", brainBin, [`--brain.host=${HOSTS.brain}`, `--brain.port=${brainPort}`, "--brain.region=local",
    "--servers.latest_poll_s=0"], { env });
  const brainUrl = `http://${HOSTS.brain}:${brainPort}`;
  await waitFor(async () => (await getJSON(brainUrl + "/mm/game-list")).status === 200, 20000, "kn_brain");

  const gamePort = await freeGamePort();
  const bridgePort = await freePort("tcp");
  const nodeId = "KNN" + crypto.randomBytes(2).toString("hex").toUpperCase();
  const serverCfg = path.join(runDir, "server.json");
  fs.writeFileSync(serverCfg, JSON.stringify({ gameserver: { node_id: nodeId, name: "Krunker Box Office node driver", region: "local",
    listen_port: gamePort, public_host: HOSTS.game, brain_url: `ws://${HOSTS.brain}:${brainPort}/mm/internal/ws`,
    brain_secret: secret, modes: ["ffa"], maps: ["Burg"], max_players: 6, idle_kick_seconds: 0, idle_menu_kick_seconds: 0 } }), { mode: 0o600 });
  kids.spawn("server", path.join(BUILD, "server_bin"), ["--headless", "--config", serverCfg, "--cfg", "none"], { env, log: "server.log" });
  await waitFor(async () => (await getJSON(brainUrl + "/mm/game-list")).body?.games?.some((g) => g[0].startsWith(nodeId + ":")), 30000, "the game server in the brain's list");

  kids.spawn("bridge", path.join(BUILD, "v10_bridge"), [`--v10-listen=${bridgePort}`, `--bind=${HOSTS.bridge}`,
    `--native=${HOSTS.game}:${gamePort}`], { env, log: "bridge.log" });
  await waitFor(() => tcpOpen(bridgePort, HOSTS.bridge), 15000, "v10_bridge");
  return { brainUrl, brainPort, bridgePort, gamePort, nodeId };
}

// The harness config for this run: the example's routes, with this run's ports, directories and brain.
function writeHarnessConfig(runDir, ports, { pagePort, tlsPort, brainUrl, bridgePort }) {
  const cfg = JSON.parse(fs.readFileSync(EXAMPLE, "utf8"));
  cfg.runDir = path.join(runDir, "harness");
  cfg.port = pagePort;
  cfg.tlsPort = tlsPort;
  cfg.localUrl = `http://${PAGE_HOST}:${pagePort}/`;
  cfg.brainUrl = brainUrl;
  cfg.bridge = { host: BRIDGE_NAME, port: bridgePort, log: path.join(runDir, "bridge.log") };
  cfg.joinWaitMs = Number(proc.env.KN_NODE_JOIN_MS || 90000);
  cfg.loadWaitMs = Number(proc.env.KN_NODE_LOAD_MS || 30000);
  cfg.afterAcceptMs = Number(proc.env.KN_NODE_ACCEPT_MS || 8000);
  fs.mkdirSync(cfg.runDir, { recursive: true });
  const file = path.join(runDir, "harness.config.json");
  fs.writeFileSync(file, JSON.stringify(cfg, null, 2), { mode: 0o600 });
  // TLS for the harness's HTTPS side, generated like the harness's prepare step (in the run dir, not in git)
  const key = path.join(cfg.runDir, "tls-key.pem");
  const crt = path.join(cfg.runDir, "tls-cert.pem");
  if (!fs.existsSync(key) || !fs.existsSync(crt)) {
    const r = spawnSync("openssl", ["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
      "-nodes", "-days", "3650", "-subj", "/CN=krunker-local", "-keyout", key, "-out", crt], { stdio: "ignore" });
    if (r.status !== 0) throw new Error("openssl could not make the local TLS certificate");
  }
  return { file, cfg };
}

// Chromium's host rules, as the harness builds them: the page name, the official hosts and the bridge name map to the
// local listeners; every other name is NOTFOUND.
function hostRules(cfg) {
  return [`MAP ${PAGE_HOST} ${assertLoopback("page map target", "127.0.0.1")}:${cfg.port}`,
    ...Object.keys(cfg.hostRoutes).map((h) => `MAP ${h} 127.0.0.1:${cfg.tlsPort}`),
    `MAP ${BRIDGE_NAME} 127.0.0.1:${cfg.tlsPort}`,
    "MAP * ~NOTFOUND"];
}

// Query values that carry tokens are not written to the logs.
function redact(u) {
  return String(u).replace(/((?:clientKey|at|clientUID|bpt|validationToken|accessToken|dataQuery)=)[^&]*/g, "$1…");
}

// The page's controls, as the harness clicks them (its own strings: the consent accept and the menu's Quick Match).
const ACCEPT_JS = `(() => { const re = /^\\s*I Accept\\s*$/i;
  const hits = [...document.querySelectorAll("button, [role=button], div, span, a")].filter((x) => re.test(x.innerText || ""));
  const b = hits.find((x) => ![...x.children].some((c) => re.test(c.innerText || ""))) || hits[0];
  if (!b) return false; b.click(); return true; })()`;
const CLICK_PLAY_JS = `(() => {
  const els = [...document.querySelectorAll("button, [role=button], .button, a, div")].filter((e) => e.offsetParent && e.innerText && e.innerText.trim().length < 40);
  const re = /quick\\s*match/i;
  const hits = els.filter((x) => re.test(x.innerText));
  const e = hits.find((x) => ![...x.children].some((c) => re.test(c.innerText))) || hits[0];
  if (!e) return null;
  e.click();
  return e.tagName + "." + String(e.className).slice(0, 60);
})()`;
// A visible disconnect dialog (its label alone): the menu also carries the same words hidden.
const LOST_JS = `[...document.querySelectorAll("div, span, h1, h2, h3, p")].some((e) => /^\\s*(Lost Connection|DISCONNECTED)\\s*$/.test(e.innerText || "") && e.getBoundingClientRect().height > 0)`;
// The element behind the visible disconnect text (the one LOST_JS matches): its id, class, outer HTML head, rect, and the
// ancestors with their computed visibility, so the element that shows the overlay can be named.
const LOST_DETAIL_JS = `(() => {
  const e = [...document.querySelectorAll("div, span, h1, h2, h3, p")].find((x) => /^\\s*(Lost Connection|DISCONNECTED)\\s*$/.test(x.innerText || "") && x.getBoundingClientRect().height > 0);
  if (!e) return null;
  const chain = [];
  for (let n = e; n && n !== document.documentElement; n = n.parentElement) {
    const st = getComputedStyle(n);
    chain.push(n.tagName + "#" + n.id + "." + String(n.className).slice(0, 80) + " display=" + st.display + " visibility=" + st.visibility + " opacity=" + st.opacity + " z=" + st.zIndex + " pos=" + st.position + " text=" + JSON.stringify((n.innerText || "").slice(0, 60)));
  }
  return { id: e.id, class: String(e.className), outer: e.outerHTML.slice(0, 400), rect: JSON.stringify(e.getBoundingClientRect()), chain: chain.slice(0, 12) };
})()`;
const STATE_JS = `(() => ({
  game: typeof window.Game, render: typeof window.RENDER,
  canvases: [...document.querySelectorAll("canvas")].map((c) => c.width + "x" + c.height).join(","),
  text: (document.body && document.body.innerText || "").replace(/\\s+/g, " ").trim().slice(0, 200),
}))()`;

// The overlay builder's arguments at entry (its own parameter names): the reason text, the code and the error.
const OVERLAY_ARGS_JS = `(() => { const s = (x) => x === null ? "null" : x === undefined ? "undefined" : typeof x === "object" ? "obj:" + JSON.stringify(x).slice(0, 200) : String(x).slice(0, 200);
  try { return JSON.stringify({ a: s(iíIîïiì), b: s(iìÎïííï), c: s(iíÌîíïi), d: s(iìÍîíií) }); } catch (e) { return "probe " + e.message; } })()`;
// The disconnect routine's breakpoint (the join owner's probe, tools/krunker-local/krunker-local.mjs): its column in
// game.js (line 2 of the script as CDP numbers it) and the probe of its three arguments (reason, code, error).
const DISCONNECT_COLUMN = 10493103;
const DISCONNECT_FN = "iïiìïîï";
// The overlay builder (game.js function iìïíïiì(reason, code, error, state)): it sets instructionsUpdate's
// "Lost Connection" text. Its three call sites are the connected-elsewhere path and the disconnect routine.
const OVERLAY_COLUMN = 10487158;
const OVERLAY_FN = "iìïíïiì";
// The matchmaker answer handler's call into the error screen (game.js column 10499149): the response it was given.
const HANDLER_COLUMN = 10499149;
const HANDLER_ARGS_JS = `(() => { const s = (x) => { try { return x === null ? "null" : x === undefined ? "undefined" : typeof x === "object" ? "obj:" + Object.keys(x).join(",").slice(0, 200) : String(x).slice(0, 200); } catch (e) { return "?"; } };
  const out = { message: s(iïÎììïì), code: s(iiÏîìïí), response: s(iïÎíïíì) };
  try { const r = iïÎíïíì; out.status = s(r && r.status); out.url = s(r && r.config && r.config.url); out.method = s(r && r.config && r.config.method);
    out.data = JSON.stringify(r && r.data).slice(0, 500); out.respData = JSON.stringify(r && r.response && r.response.data).slice(0, 500); } catch (e) { out.probeErr = String(e.message); }
  return JSON.stringify(out); })()`;
const DISCONNECT_PROBE_JS = `(() => {
  const s = (x) => x === null ? "null" : x === undefined ? "undefined" : typeof x === "object" ? "obj:" + JSON.stringify(x).slice(0, 160) : String(x).slice(0, 160);
  try { return JSON.stringify({ reason: s(iïÌïîïî), code: s(iìÌìîiï), err: s(iíÍïíiì) }); } catch (e) { return "probe " + e.message; }
})()`;

// The DevTools session on one page target: request/response by id, events handed to the listener.
function cdpSession(wsUrl, onEvent) {
  const ws = new WebSocket(wsUrl);
  let id = 0;
  const pending = new Map();
  const ready = new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && pending.has(msg.id)) { const { res } = pending.get(msg.id); pending.delete(msg.id); res(msg); return; }
    if (msg.method) onEvent(msg.method, msg.params || {});
  };
  const send = (method, params = {}) => new Promise((res) => {
    const i = ++id;
    pending.set(i, { res });
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  return { ready, send, close: () => ws.close() };
}

// The frame log: one JSON object per frame, binary payloads base64 (replayable byte for byte).
function frameWriter(file, t0) {
  const fd = fs.openSync(file, "a");
  let count = { in: 0, out: 0 };
  let closed = false;
  return {
    count: () => count,
    write(dir, requestId, url, response) {
      const rec = { t: now() - t0, dir, socket: redact(url || requestId).replace(/^wss?:\/\/([^/]+).*$/, "$1"), opcode: response.opcode, len: 0 };
      if (response.opcode === 2) { const b = Buffer.from(response.payloadData, "base64"); rec.len = b.length; rec.b64 = response.payloadData; }
      else { rec.len = Buffer.byteLength(response.payloadData || ""); rec.text = String(response.payloadData || "").slice(0, 2000); }
      fs.writeSync(fd, JSON.stringify(rec) + "\n");
      count[dir]++;
    },
    close: () => { if (!closed) { closed = true; fs.closeSync(fd); } },
  };
}

// Plays one run: the stack, the page, Quick Match, and the join check. Resolves with the result object.
export async function run(opts = {}) {
  const runDir = opts.runDir || path.join(os.homedir(), ".cache/kn-krunker-node/run", String(proc.pid));
  fs.mkdirSync(runDir, { recursive: true });
  const kids = new Children(runDir);
  const frames = frameWriter(path.join(runDir, "frames.jsonl"), now());
  const t0 = now();
  const result = { runDir, joined: false, reason: "", frames: frames.count(), lost: false, clicked: null, game: null, render: null, text: "" };
  let chrome = null;
  let page = null;
  const stopAll = () => {
    try { if (page) page.close(); } catch (e) { /* closed */ }
    try { if (chrome) chrome.kill("SIGTERM"); } catch (e) { /* gone */ }
    try { fs.rmSync(path.join(runDir, "pids", "chrome.pid")); } catch (e) { /* none */ }
    kids.stop();
    frames.close();
  };
  const onSignal = () => { stopAll(); proc.exit(143); };
  proc.on("SIGINT", onSignal);
  proc.on("SIGTERM", onSignal);
  try {
    const secret = crypto.randomBytes(24).toString("base64url");
    fs.writeFileSync(path.join(runDir, "secret"), secret, { mode: 0o600 });
    const stack = await startStack(kids, runDir, secret);
    result.stack = { brainPort: stack.brainPort, bridgePort: stack.bridgePort, gamePort: stack.gamePort, nodeId: stack.nodeId };

    const pagePort = await freePort("tcp");
    const tlsPort = await freePort("tcp");
    const { file, cfg } = writeHarnessConfig(runDir, null, { pagePort, tlsPort, brainUrl: stack.brainUrl, bridgePort: stack.bridgePort });
    kids.spawn("harness", proc.execPath, [HARNESS, "serve"], { env: { ...proc.env, KN_KRUNKER_LOCAL_CONFIG: file }, log: "harness.log" });
    await waitFor(() => tcpOpen(pagePort, "127.0.0.1") && tcpOpen(tlsPort, "127.0.0.1"), 20000, "the harness page server");

    const cdpPort = await freePort("tcp");
    chrome = spawn(findChrome(), [
      "--headless=new", "--no-sandbox", "--disable-dev-shm-usage", "--use-gl=angle", "--use-angle=swiftshader",
      "--enable-unsafe-swiftshader", "--window-size=1280,720", "--disable-background-networking", "--disable-component-update",
      "--no-first-run", "--disable-sync", `--host-resolver-rules=${hostRules(cfg).join(", ")}`, "--ignore-certificate-errors",
      `--user-data-dir=${path.join(runDir, "chrome-profile")}`, `--remote-debugging-port=${cdpPort}`, "about:blank",
    ], { env: { ...proc.env, DISPLAY: "", WAYLAND_DISPLAY: "" }, stdio: "ignore" });
    fs.writeFileSync(path.join(runDir, "pids", "chrome.pid"), String(chrome.pid));
    let targets = [];
    await waitFor(async () => { targets = await getJSON(`http://127.0.0.1:${cdpPort}/json/list`).then((r) => r.body || []).catch(() => []); return targets.some((t) => t.type === "page"); }, 30000, "a Chromium page");
    const target = targets.find((t) => t.type === "page");

    const reqUrl = new Map();
    const errors = [];
    const eventCounts = {};
    const consoleLog = [];
    const kwrites = [];
    const ovl = [];
    const disconnects = [];
    const pauses = [];
    const handlers = [];
    let appScript = null;
    const onEvent = (method, p) => {
      eventCounts[method] = (eventCounts[method] || 0) + 1;
      switch (method) {
        case "Network.requestWillBeSent": reqUrl.set(p.requestId, p.request.url); break;
        case "Network.webSocketCreated": reqUrl.set(p.requestId, p.url); break;
        case "Network.webSocketFrameSent": frames.write("out", p.requestId, reqUrl.get(p.requestId), p.response); break;
        case "Network.webSocketFrameReceived": frames.write("in", p.requestId, reqUrl.get(p.requestId), p.response); break;
        case "Network.loadingFailed": errors.push(`failed ${redact(reqUrl.get(p.requestId) || p.requestId).slice(0, 120)}: ${p.errorText}`); break;
        case "Debugger.scriptParsed":
          if (/krunker\.io\/js\/app\.js$/.test(p.url || "") && !appScript) {
            appScript = p.scriptId;
            page.send("Debugger.setBreakpoint", { location: { scriptId: appScript, lineNumber: 2, columnNumber: OVERLAY_COLUMN } })
              .then((r) => { result.overlayBreakpoint = r.result ? r.result.actualLocation : r.error; });
            page.send("Debugger.setBreakpoint", { location: { scriptId: appScript, lineNumber: 2, columnNumber: HANDLER_COLUMN } })
              .then((r) => { result.handlerBreakpoint = r.result ? r.result.actualLocation : r.error; });
          }
          break;
        case "Debugger.paused": {
          const top = p.callFrames[0];
          if (pauses.length < 10) pauses.push({ reason: p.reason, fn: top.functionName || "?", line: top.location.lineNumber, col: top.location.columnNumber, t: now() - t0 });
          if (top.location.lineNumber === 2 && Math.abs(top.location.columnNumber - HANDLER_COLUMN) <= 16) {
            (async () => {
              const r = await page.send("Debugger.evaluateOnCallFrame", { callFrameId: top.callFrameId, expression: HANDLER_ARGS_JS, returnByValue: true });
              handlers.push({
                args: r.result && r.result.result ? r.result.result.value : r.error || r,
                stack: p.callFrames.slice(0, 14).map((f) => `${f.functionName || "?"}@${f.location.lineNumber}:${f.location.columnNumber}`),
                t: now() - t0,
              });
              await page.send("Debugger.resume");
            })();
          } else if (top.functionName === OVERLAY_FN) {
            (async () => {
              const r = await page.send("Debugger.evaluateOnCallFrame", { callFrameId: top.callFrameId, expression: OVERLAY_ARGS_JS, returnByValue: true });
              disconnects.push({
                args: r.result && r.result.result ? r.result.result.value : r.error || r,
                stack: p.callFrames.slice(0, 14).map((f) => `${f.functionName || "?"}@${f.location.lineNumber}:${f.location.columnNumber}`),
                t: now() - t0,
              });
              await page.send("Debugger.resume");
            })();
          } else if (top.functionName === DISCONNECT_FN) {
            (async () => {
              const r = await page.send("Debugger.evaluateOnCallFrame", { callFrameId: top.callFrameId, expression: DISCONNECT_PROBE_JS, returnByValue: true });
              disconnects.push({
                args: r.result && r.result.result ? r.result.result.value : r.error || r,
                stack: p.callFrames.slice(0, 10).map((f) => `${f.functionName || "?"}@${f.location.lineNumber}:${f.location.columnNumber}`),
                t: now() - t0,
              });
              await page.send("Debugger.resume");
            })();
          } else {
            page.send("Debugger.resume");
          }
          break;
        }
        case "Runtime.consoleAPICalled":
          if (p.args && p.args.length && String(p.args[0].value || "").startsWith("[kwrite]")) kwrites.push(String(p.args[0].value).slice(0, 900));
          if (p.args && p.args.length && String(p.args[0].value || "").startsWith("[ovl]")) ovl.push(String(p.args[0].value).slice(0, 1400));
          if (p.type === "error" || p.type === "warning" || /local|hook|FAIL/.test(JSON.stringify(p.args).slice(0, 300))) consoleLog.push(`${p.type} ${JSON.stringify(p.args).slice(0, 220)}`);
          break;
        case "Runtime.exceptionThrown": errors.push("exception " + String((p.exceptionDetails.exception || {}).description || p.exceptionDetails.text).split("\n")[0].slice(0, 200)); break;
        default: break;
      }
    };
    page = cdpSession(target.webSocketDebuggerUrl, onEvent);
    await page.ready;
    for (const d of ["Page.enable", "Network.enable", "Runtime.enable"]) await page.send(d);
    // Harness-only trace, loopback page: every write and removal of the four account keys, with the stack (the first
    // one is the write that starts the account). Values are cut to 12 characters. Runs before the page's own scripts.
    await page.send("Page.addScriptToEvaluateOnNewDocument", { source: `(() => { const S = Storage.prototype; const si = S.setItem, ri = S.removeItem;
      const ACCOUNT = /^krunker_(id|haslogin|token|username)$/;
      const log = (kind, k, v) => { if (!ACCOUNT.test(String(k))) return;
        const stack = String(new Error().stack).split("\\n").slice(2, 9).map((l) => l.trim().slice(0, 120)).join(" | ");
        console.log("[kwrite] " + kind + " " + k + " = " + String(v).slice(0, 12) + " @ " + stack); };
      S.setItem = function (k, v) { log("set", k, v); return si.call(this, k, v); };
      S.removeItem = function (k) { log("del", k, ""); return ri.call(this, k); }; })();` });
    // The disconnect breakpoint: set when the page's app script is parsed; the paused frame's arguments and callers are
    // recorded, then the page carries on.
    await page.send("Debugger.enable");
    await page.send("Debugger.setPauseOnExceptions", { state: "none" });
    // The overlay trace (overlay-hook.js): the first writes of the disconnect overlay, before the page's own scripts.
    await page.send("Page.addScriptToEvaluateOnNewDocument", { source: fs.readFileSync(path.join(HERE, "overlay-hook.js"), "utf8") });
    await page.send("Page.navigate", { url: cfg.localUrl });

    // the consent dialog comes back until its vendor check settles: accept it as a user would
    const evalJS = async (expression) => { const r = await page.send("Runtime.evaluate", { expression, returnByValue: true }); return r.result && r.result.result ? r.result.result.value : null; };
    // poll: accept the consent dialog whenever it is up, click Quick Match once the menu shows it
    const menuDeadline = now() + cfg.loadWaitMs;
    let reloaded = false;
    while (now() < menuDeadline && !result.clicked) {
      await sleep(2000);
      await evalJS(ACCEPT_JS);
      result.clicked = await evalJS(CLICK_PLAY_JS);
      // half-way with no menu: reload the same profile once (it now holds this run's storage), and keep polling
      if (!result.clicked && !reloaded && now() > menuDeadline - cfg.loadWaitMs / 2) {
        reloaded = true;
        result.reloadedAt = now() - t0;
        await page.send("Page.reload", {});
      }
    }
    await sleep(3000);

    // the join: in-game when the bridge has joined the server and the page shows no lost connection
    const deadline = now() + cfg.joinWaitMs;
    let state = null;
    while (now() < deadline) {
      await sleep(3000);
      state = await evalJS(STATE_JS);
      result.text = state && state.text;
      result.lost = !!(await evalJS(LOST_JS));
      if (result.lost) break;
      if (state && state.game === "object" && state.render === "object") break;
    }
    if (!result.clicked) {
      result.menuProbe = await evalJS(`[...document.querySelectorAll("*")].filter((e) => /quick\\s*match/i.test(e.innerText || "")).slice(0, 10).map((e) => [e.tagName, String(e.className).slice(0, 60), !!e.offsetParent, (e.innerText || "").length, (e.innerText || "").trim().slice(0, 60)])`);
      result.newUser = await evalJS("String(window.isNewUser) + ' storage=' + localStorage.length");
      result.hiddenBy = await evalJS(`(() => { const c = [...document.querySelectorAll("span.cardLabel, .actionCard")].find((e) => /quick\\s*match/i.test(e.innerText || "")); const out = []; for (let n = c; n && n !== document.body; n = n.parentElement) { const s = getComputedStyle(n); if (s.display === "none" || s.visibility === "hidden" || s.opacity === "0") out.push(n.tagName + "#" + n.id + "." + String(n.className).slice(0, 50) + " display=" + s.display + " visibility=" + s.visibility + " opacity=" + s.opacity); } return out.slice(0, 6); })()`);
      result.consentOpen = await evalJS(`(() => { const e = document.getElementById("consentBlock"); return !!e && getComputedStyle(e).display !== "none"; })()`);
    }
    if (result.lost) result.lostElement = await evalJS(LOST_DETAIL_JS);
    result.game = await evalJS("typeof window.Game");
    result.render = await evalJS("typeof window.RENDER");
    result.text = (await evalJS(STATE_JS) || {}).text || result.text;
    result.frames = frames.count();
    result.eventCounts = eventCounts;
    result.console = consoleLog.slice(0, 12);
    result.kwrites = kwrites.slice(0, 40);
    result.disconnects = disconnects.slice(0, 5);
    result.pauses = pauses;
    result.ovl = ovl.slice(0, 8);
    result.handlers = handlers.slice(0, 4);
    result.errors = errors.slice(0, 10);
    result.bridgeLog = fs.existsSync(path.join(runDir, "bridge.log"))
      ? fs.readFileSync(path.join(runDir, "bridge.log"), "utf8").split("\n").filter((l) => /v10|bridge/.test(l)).slice(-6) : [];
    result.serverLog = fs.existsSync(path.join(runDir, "server.log"))
      ? fs.readFileSync(path.join(runDir, "server.log"), "utf8").split("\n").slice(-8) : [];
    // a join counts only with frames both ways, no lost connection, and the page's game objects present
    result.joined = result.frames.in > 0 && result.frames.out > 0 && !result.lost && result.game === "object" && result.render === "object";
    result.reason = result.joined ? "joined" : (result.lost ? "lost connection on the page" : (result.game !== "object" ? "window.Game is not set by the client" : "no join"));
    fs.writeFileSync(path.join(runDir, "result.json"), JSON.stringify(result, null, 2));
    return result;
  } finally {
    proc.off("SIGINT", onSignal);
    proc.off("SIGTERM", onSignal);
    stopAll();
    result.elapsedMs = now() - t0;
  }
}

// The join owner's harness join (krunker-local.mjs join), run from here: the stack, the harness page server (serve) and
// the join child are each a pid this process started; the join's JSON report is read from its stdout. The harness
// sets no window.__kn_game and no state.inGame on origin/main, so the report is what is measured, not a game object.
async function runHarnessJoin(opts = {}) {
  // KN_NODE_PERSIST_PROFILE names a directory that outlives the run: the harness keeps its Chromium profile in
  // <runDir>/join-profile, so a second run on the same directory starts from the first run's browser state.
  const runDir = path.join(os.homedir(), ".cache/kn-krunker-node/run", String(proc.pid) + "-harness");
  fs.mkdirSync(runDir, { recursive: true });
  const kids = new Children(runDir);
  const persist = proc.env.KN_NODE_PERSIST_PROFILE;
  const secret = crypto.randomBytes(24).toString("base64url");
  fs.writeFileSync(path.join(runDir, "secret"), secret, { mode: 0o600 });
  let serve = null;
  let join = null;
  const stopAll = () => {
    if (join && join.exitCode === null) join.kill("SIGTERM");
    if (serve && serve.exitCode === null) serve.kill("SIGTERM");
    kids.stop();
  };
  proc.on("SIGINT", () => { stopAll(); proc.exit(143); });
  proc.on("SIGTERM", () => { stopAll(); proc.exit(143); });
  try {
    const stack = await startStack(kids, runDir, secret);
    const pagePort = await freePort("tcp");
    const tlsPort = await freePort("tcp");
    const { file, cfg: hcfg } = writeHarnessConfig(runDir, null, { pagePort, tlsPort, brainUrl: stack.brainUrl, bridgePort: stack.bridgePort });
    if (persist) {
      hcfg.runDir = persist;
      fs.mkdirSync(persist, { recursive: true });
      fs.writeFileSync(file, JSON.stringify(hcfg, null, 2), { mode: 0o600 });
      fs.copyFileSync(path.join(runDir, "harness", "tls-key.pem"), path.join(persist, "tls-key.pem"));
      fs.copyFileSync(path.join(runDir, "harness", "tls-cert.pem"), path.join(persist, "tls-cert.pem"));
    }
    const env = { ...proc.env, KN_KRUNKER_LOCAL_CONFIG: file };
    serve = kids.spawn("harness", proc.execPath, [HARNESS, "serve"], { env, log: "harness.log" });
    await waitFor(() => tcpOpen(pagePort, "127.0.0.1") && tcpOpen(tlsPort, "127.0.0.1"), 20000, "the harness page server");
    // the join child's stdout is the report (JSON); its stderr goes to a log in the run dir
    join = spawn(proc.execPath, [HARNESS, "join"], { env, cwd: REPO, stdio: ["ignore", "pipe", fs.openSync(path.join(runDir, "join.err"), "w")] });
    fs.writeFileSync(path.join(runDir, "pids", "join.pid"), String(join.pid));
    let out = "";
    join.stdout.on("data", (c) => { out += c; });
    const code = await new Promise((res) => join.on("exit", res));
    fs.rmSync(path.join(runDir, "pids", "join.pid"), { force: true });
    let report = null;
    try { report = JSON.parse(out); } catch (e) { report = { parseError: out.slice(0, 300) }; }
    const sockets = (report && report.sockets) || [];
    const result = {
      runDir, exitCode: code, frames: { in: report && report.framesIn, out: report && report.framesOut },
      seek: report && report.seek, sockets: sockets.length, clicked: report && report.clicked, lost: /Lost Connection/.test((report && report.state && report.state.text) || ""),
      inGame: !!(report && report.state && report.state.inGame), gameExposed: !!(report && report.game && report.game.exposed),
      bridgeLog: report && report.bridgeLog, timeline: report && report.timeline && report.timeline.slice(-3),
    };
    fs.writeFileSync(path.join(runDir, "result.json"), JSON.stringify(result, null, 2));
    return result;
  } finally {
    stopAll();
  }
}

async function main() {
  if (proc.argv.includes("--harness")) {
    const r = await runHarnessJoin();
    console.log(`[krunker-node] harness join: frames in ${r.frames.in} out ${r.frames.out}; sockets ${r.sockets}; inGame ${r.inGame}; window.__kn_game exposed ${r.gameExposed}; run ${r.runDir}`);
    proc.exit(r.frames.in > 0 && r.frames.out > 0 ? 0 : 1);
  }
  const smoke = proc.argv.includes("--smoke");
  const url = (await import("node:url")).pathToFileURL(HERE).href;
  if (!smoke) {
    console.log(`[krunker-node] starting; ctrl-c stops the stack`);
  }
  const result = await run();
  console.log(`[krunker-node] run dir ${result.runDir}`);
  console.log(`[krunker-node] joined: ${result.joined} (${result.reason}); frames in ${result.frames.in} out ${result.frames.out}; game ${result.game}; render ${result.render}`);
  for (const e of (result.errors || []).slice(0, 5)) console.log(`[krunker-node] page: ${e}`);
  if (smoke) proc.exit(result.joined ? 0 : 1);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((e) => { console.error(`krunker-node: ${e.message}`); proc.exit(1); });
}
