#!/usr/bin/env node
// Krunker Box Office: the official Krunker v10.0.3 client run in Node, against the local stack.
//
//   node tools/krunker-local/launch.mjs --smoke    start the stack, boot the client, check the menu, exit 0/1
//   node tools/krunker-local/launch.mjs            start the stack and the client, keep them up until SIGINT
//   make krunker-node / make krunker-node-smoke   the same, through the Makefile
//
// The stack is this repository's own: kn_brain (sqlite, a scratch database), one server_bin game server and
// v10_bridge, each on a free loopback port. Each process is started by this script and recorded in a pid file;
// stop() signals only the children it spawned (never by process name). The client is the official bundle in
// /home/user/jails/kru/src/Krunker_v10.0.3 (read only), run by node-env.mjs with the page's own scripts.
// Every WebSocket frame in and out is appended to <run>/frames.jsonl (one JSON object per line).
//
// Loopback only: every host this script binds or dials must be 127.0.0.1 or *.krunker-local.test, and it refuses
// to start otherwise. The official hostnames the client uses are routed to the brain inside the page; they are never
// resolved or dialled.
import fs from "node:fs";
import net from "node:net";
import dgram from "node:dgram";
import path from "node:path";
import os from "node:os";
import vm from "node:vm";
import crypto from "node:crypto";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { installPage, parseInto, findByText, isLoopbackHost } from "./node-env.mjs";

// The bundle replaces the global process with its browser stub when it runs: keep the real one.
const proc = process;
const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../..");
const BUILD = path.join(REPO, "cmake-build-debug");
const KRUNKER = proc.env.KN_KRUNKER_SRC || "/home/user/jails/kru/src/Krunker_v10.0.3";
const PAGE_HOST = "krunker-local.test";
const BRIDGE_NAME = "kn-bridge.krunker-local.test";

// Every host the script binds or dials. Refused unless loopback.
export function assertLoopback(name, host) {
  if (!isLoopbackHost(host)) throw new Error(`refusing to start: ${name} host ${host} is not 127.0.0.1 or *.krunker-local.test`);
  return host;
}
const HOSTS = {
  brain: assertLoopback("brain", proc.env.KN_NODE_BRAIN_HOST || "127.0.0.1"),
  bridge: assertLoopback("bridge", proc.env.KN_NODE_BRIDGE_HOST || "127.0.0.1"),
  game: assertLoopback("game server", proc.env.KN_NODE_GAME_HOST || "127.0.0.1"),
  page: assertLoopback("page", PAGE_HOST),
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
    try { await freePort("udp"); return p; } catch (e) { /* UDP taken, try again */ }
  }
  throw new Error("no free port pair");
}

function getJSON(url) {
  return fetch(url).then(async (r) => ({ status: r.status, body: await r.json().catch(() => null) }));
}

async function waitFor(fn, ms, what) {
  const end = now() + ms;
  while (now() < end) {
    try { const v = await fn(); if (v) return v; } catch (e) { /* not up yet */ }
    await sleep(200);
  }
  throw new Error(`timed out waiting for ${what}`);
}

// Child processes: spawned here, recorded in pids/<name>.pid, stopped by their handle only.
class Stack {
  constructor(runDir) {
    this.runDir = runDir;
    this.children = [];
    fs.mkdirSync(path.join(runDir, "pids"), { recursive: true });
  }
  spawn(name, cmd, args, env, logFile) {
    const out = fs.openSync(path.join(this.runDir, logFile), "w");
    const child = spawn(cmd, args, { cwd: REPO, env, stdio: ["ignore", out, out] });
    fs.closeSync(out);
    fs.writeFileSync(path.join(this.runDir, "pids", name + ".pid"), String(child.pid));
    this.children.push({ name, child });
    child.once("exit", () => { try { fs.rmSync(path.join(this.runDir, "pids", name + ".pid")); } catch (e) { /* gone */ } });
    return child;
  }
  // Signals the children this stack started (by handle) and removes their pid files: the exit events may not run
  // before the process exits, so the files go here.
  stop() {
    for (const { name, child } of this.children.reverse()) {
      if (child.exitCode === null && child.signalCode === null) child.kill("SIGTERM");
      try { fs.rmSync(path.join(this.runDir, "pids", name + ".pid")); } catch (e) { /* already removed */ }
    }
    this.children = [];
  }
}

// Starts kn_brain (sqlite scratch database), one server_bin and v10_bridge. Returns their ports.
async function startStack(stack, runDir) {
  const secret = fs.existsSync(path.join(runDir, "secret")) ? fs.readFileSync(path.join(runDir, "secret"), "utf8").trim()
    : crypto.randomBytes(24).toString("base64url");
  fs.writeFileSync(path.join(runDir, "secret"), secret, { mode: 0o600 });
  const db = JSON.parse(spawnSync("python3", ["-I", path.join(REPO, "services/brain/tests/db_scratch.py"), REPO, "create", "sqlite",
    "kn_node_" + proc.pid], { encoding: "utf8" }).stdout || "null");
  if (!db) throw new Error("no scratch database (services/brain/tests/db_scratch.py create sqlite)");
  const env = { ...proc.env, KN_DATABASE_DRIVER: "sqlite", KN_DATABASE_PATH: db.path, KN_BRAIN_INTERNAL_SECRET: secret };
  const brainBin = path.join(BUILD, "services/brain/kn_brain");
  const migrate = spawnSync(brainBin, ["--migrate-only"], { cwd: REPO, env, stdio: "ignore" });
  if (migrate.status !== 0) throw new Error("kn_brain --migrate-only failed");
  const brainPort = await freePort("tcp");
  stack.spawn("brain", brainBin, [`--brain.host=${HOSTS.brain}`, `--brain.port=${brainPort}`, "--brain.region=local",
    "--servers.latest_poll_s=0"], env, "brain.log");
  const brainUrl = `http://${HOSTS.brain}:${brainPort}`;
  await waitFor(() => getJSON(brainUrl + "/mm/game-list").then((r) => r.status === 200), 20000, "kn_brain");

  const gamePort = await freeGamePort();
  const bridgePort = await freePort("tcp");
  const nodeId = "KNN" + crypto.randomBytes(2).toString("hex").toUpperCase();
  const serverCfg = path.join(runDir, "server.json");
  fs.writeFileSync(serverCfg, JSON.stringify({ gameserver: { node_id: nodeId, name: "Krunker Box Office node driver", region: "local",
    listen_port: gamePort, public_host: HOSTS.game, brain_url: `ws://${HOSTS.brain}:${brainPort}/mm/internal/ws`,
    brain_secret: secret, modes: ["ffa"], maps: ["Burg"], max_players: 6, idle_kick_seconds: 0, idle_menu_kick_seconds: 0 } }),
  { mode: 0o600 });
  stack.spawn("server", path.join(BUILD, "server_bin"), ["--headless", "--config", serverCfg, "--cfg", "none"], env, "server.log");
  await waitFor(async () => (await getJSON(brainUrl + "/mm/game-list")).body?.games?.some((g) => g[0].startsWith(nodeId + ":")), 30000, "the game server in the brain's list");

  stack.spawn("bridge", path.join(BUILD, "v10_bridge"), [`--v10-listen=${bridgePort}`, `--bind=${HOSTS.bridge}`,
    `--native=${HOSTS.game}:${gamePort}`], env, "bridge.log");
  await waitFor(() => new Promise((res) => { const s = net.connect(bridgePort, HOSTS.bridge); s.once("connect", () => { s.destroy(); res(true); }); s.once("error", () => res(false)); }), 15000, "v10_bridge");
  return { brainUrl, bridgePort, gamePort, nodeId, secret };
}

// The client's page: index.html's markup and classic scripts, then loader.js's steps (token, game.js with (token, callback)).
async function bootClient(cfg, onFrame, onRequest, onError) {
  const env = installPage({
    pageUrl: `http://${PAGE_HOST}:8094/`,
    brainUrl: cfg.brainUrl,
    bridgeHost: BRIDGE_NAME,
    bridgePort: cfg.bridgePort,
    krunkerSrc: KRUNKER,
    knRepo: REPO,
    fillRoots: ["assets", "ui"],
    onFrame, onRequest, onError,
    onSocket: () => {},
    rewriteSeek: (buf) => {
      const seek = JSON.parse(buf.toString("utf8"));
      seek.host = BRIDGE_NAME;
      seek.port = 443;
      return Buffer.from(JSON.stringify(seek));
    },
  });
  const html = fs.readFileSync(path.join(KRUNKER, "index.html"), "utf8");
  const scripts = parseInto(env.document, html);
  for (const el of scripts) {
    const type = el.getAttribute("type") || "";
    const src = el.getAttribute("src");
    if (type === "module" || type === "application/ld+json") continue;
    if (src && /^(https?:)?\/\//.test(src)) continue; // remote scripts (analytics, ads, vendors) never load
    let code = el.textContent;
    let name = "inline";
    if (src) {
      name = src.replace(/\?.*$/, "");
      const file = path.join(KRUNKER, name.replace(/^\.\//, ""));
      if (!fs.existsSync(file)) continue;
      code = fs.readFileSync(file, "utf8");
    }
    try { vm.runInThisContext(code, { filename: name }); } catch (e) { onError(e); }
  }
  const loader = fs.readFileSync(path.join(HERE, "loader.js"), "utf8");
  const PARAM_TOKEN = loader.match(/PARAM_TOKEN = "([^"]+)"/)[1];
  const PARAM_CALLBACK = loader.match(/PARAM_CALLBACK = "([^"]+)"/)[1];
  const RENAMED = JSON.parse(loader.match(/RENAMED = (\{[^}]*\})/)[1].replace(/(\w+):/g, '"$1":'));
  const t = await fetch(`https://matchmaker.krunker.io/generate-token`);
  if (!t.ok) throw new Error(`generate-token -> HTTP ${t.status}`);
  const token = (await t.text()).trim();
  for (const [name, target] of Object.entries(RENAMED)) globalThis[name] = globalThis[target];
  const src = fs.readFileSync(path.join(KRUNKER, "game.js"), "utf8");
  const fn = new Function(PARAM_TOKEN, PARAM_CALLBACK, src + "\n//# sourceURL=https://krunker.io/js/app.js");
  // The bundle's own start-up. A throw here ends the client's start-up: it is returned, not thrown, so the caller reports it.
  try { fn.apply(globalThis, [token, undefined]); return { env, bootError: null }; }
  catch (e) { onError(e); return { env, bootError: String(e && e.message || e) }; }
}

// The frame log: one JSON object per frame. Binary payloads are base64 (replayable byte for byte).
function frameWriter(file, t0) {
  const fd = fs.openSync(file, "a");
  let n = 0;
  const write = (dir, data, host) => {
    let rec;
    const at = now() - t0;
    if (typeof data === "string") rec = { t: at, dir, host, kind: "text", len: Buffer.byteLength(data), text: data.slice(0, 2000) };
    else if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
      const buf = Buffer.from(data.buffer ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : data);
      rec = { t: at, dir, host, kind: "binary", len: buf.length, b64: buf.toString("base64") };
    } else rec = { t: at, dir, host, kind: typeof data, len: 0 };
    fs.writeSync(fd, JSON.stringify(rec) + "\n");
    n++;
  };
  let closed = false;
  return { write, count: () => n, close: () => { if (!closed) { closed = true; fs.closeSync(fd); } } };
}

// The client as a small API. start() boots everything; the methods drive the page (DOM events) and read the log.
export async function start(opts = {}) {
  const runDir = opts.runDir || path.join(os.homedir(), ".cache/kn-krunker-node/run", String(proc.pid));
  fs.mkdirSync(runDir, { recursive: true });
  const stack = new Stack(runDir);
  const t0 = now();
  const frames = frameWriter(path.join(runDir, "frames.jsonl"), t0);
  const errors = [];
  const requests = [];
  // The page's uncaught errors are page errors (a browser logs them and carries on): record them, keep the stack up.
  const onUncaught = (e) => errors.push("uncaught " + String(e && e.stack || e).split("\n").slice(0, 2).join(" | ").slice(0, 400));
  const onRejected = (e) => errors.push("rejected " + String(e && e.stack || e).split("\n").slice(0, 2).join(" | ").slice(0, 400));
  proc.on("uncaughtException", onUncaught);
  proc.on("unhandledRejection", onRejected);
  let env = null;
  let bootError = null;
  let state = { stack: null, page: null };
  try {
    state.stack = await startStack(stack, runDir);
    const cfg = { ...state.stack, bridgePort: state.stack.bridgePort };
    const boot = await bootClient(cfg,
      (dir, data, host) => frames.write(dir, data, host),
      (r) => requests.push(r),
      (e) => errors.push(String(e && e.stack || e).split("\n").slice(0, 3).join(" | ").slice(0, 400)));
    env = boot.env;
    bootError = boot.bootError;
  } catch (e) {
    proc.off("uncaughtException", onUncaught);
    proc.off("unhandledRejection", onRejected);
    stack.stop();
    frames.close();
    throw e;
  }
  const doc = env.document;
  // Signals stop the stack this process started (its pid files and handles), then exit.
  const onSignal = () => { stopAll(); proc.exit(143); };
  proc.on("SIGINT", onSignal);
  proc.on("SIGTERM", onSignal);
  const stopAll = () => {
    proc.off("SIGINT", onSignal);
    proc.off("SIGTERM", onSignal);
    proc.off("uncaughtException", onUncaught);
    proc.off("unhandledRejection", onRejected);
    stack.stop();
    frames.close();
  };
  return {
    runDir,
    stack: state.stack,
    bootError: () => bootError,
    // The menu's Quick Match button, as the page labels it. Returns whether it was found and clicked.
    clickPlay() {
      const b = findByText(doc, /^\s*quick\s*match\s*$/i);
      if (!b) return false;
      b.click();
      return true;
    },
    // The menu's text as the page shows it (scripts and styles excluded).
    menuText() {
      const walk = (n) => n.childNodes.map((c) => (c.nodeType === 3 ? c.data : c.nodeType === 1 && !["SCRIPT", "STYLE"].includes(c.tagName) ? walk(c) : "")).join(" ");
      return walk(doc.body).replace(/\s+/g, " ").trim();
    },
    menuReached() {
      return /quick\s*match/i.test(this.menuText());
    },
    // Input as keyboard and pointer events on the page (the game's listeners see them like the browser's).
    sendInput(input = {}) {
      const key = (type, code) => doc.dispatchEvent(new KeyboardEvent(type, { code, key: code.replace("Key", "").toLowerCase() }));
      for (const k of input.keys || []) { key("keydown", k); key("keyup", k); }
      if (input.look) doc.dispatchEvent(new MouseEvent("mousemove", { clientX: input.look.x || 0, clientY: input.look.y || 0 }));
      if (input.fire) { doc.dispatchEvent(new MouseEvent("mousedown", { button: 0, buttons: 1 })); doc.dispatchEvent(new MouseEvent("mouseup", { button: 0 })); }
    },
    // HUD detection is not implemented: no in-game element id is verified yet, so this reports that.
    async waitInGame() {
      return { inGame: false, reason: "in-game (HUD) detection is not implemented; no verified HUD element id" };
    },
    readFrames() {
      const file = path.join(runDir, "frames.jsonl");
      return fs.existsSync(file) ? fs.readFileSync(file, "utf8").split("\n").filter(Boolean).map((l) => JSON.parse(l)) : [];
    },
    errors: () => errors.slice(),
    requests: () => requests.slice(),
    stop: stopAll,
  };
}

async function main() {
  const smoke = proc.argv.includes("--smoke");
  const client = await start();
  console.log(`[krunker-node] run dir ${client.runDir}`);
  let ok = true;
  if (smoke) {
    if (client.bootError()) {
      console.log(`[krunker-node] the client's start-up failed: ${client.bootError()}`);
      for (const e of client.errors().slice(0, 5)) console.log(`[krunker-node] error: ${e}`);
      client.stop();
      proc.exit(1);
    }
    const deadline = now() + Number(proc.env.KN_NODE_SMOKE_MS || 60000);
    while (now() < deadline && !client.menuReached()) await sleep(500);
    ok = client.menuReached();
    const frames = client.readFrames().length;
    const log = path.join(client.runDir, "frames.jsonl");
    const logged = fs.existsSync(log);
    console.log(`[krunker-node] menu reached: ${ok}; frames logged: ${frames}; log written: ${logged}`);
    if (!ok) console.log(`[krunker-node] menu text: ${client.menuText().slice(0, 200)}`);
    for (const e of client.errors().slice(0, 5)) console.log(`[krunker-node] error: ${e}`);
    client.stop();
    proc.exit(ok && logged ? 0 : 1);
  }
  console.log("[krunker-node] running; ctrl-c stops the stack");
}

if (proc.argv[1] && path.resolve(proc.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((e) => { console.error(`krunker-node: ${e.message}`); proc.exit(1); });
}
