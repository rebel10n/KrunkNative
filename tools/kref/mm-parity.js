#!/usr/bin/env node
// Matchmaker parity harness: the v3.1.1 reference brain (jail matchmaker/index.ts behind
// an emulation of its nginx vhost, ansible/templates/nginx/brain.conf.j2 + cors.conf)
// against kn_brain, fed identical lobby data by fake game nodes over the internal
// WebSocket (shared/wire.ts JSON frames, which both brains speak).
//
// Every request is one the real v3.1.1 client sends (ClientManager.js, index.js,
// social.js: GET <matchmakerURL>/<route>?<params>); status, headers and JSON bodies
// (key order included) are diffed. Known, documented deviations are reported as
// "DEV" instead of "DIFF" (doc/backend.md section 4.1).
//
//   node tools/kref/mm-parity.js --build /tmp/kn-mmp [--db] [--verbose]
//        [--chromium]   also drive the real v3.1.1 client (headless Chromium + CDP)
//                       against both and diff the matchmaker requests it makes
//
// Needs the jail tree (KRUNKER_REF, default /home/user/jails/kru/src/Krunker_v3.1.1)
// with node_modules, and kn_brain in <build>/services/brain. --db runs kn_brain with
// PostgreSQL in a throwaway schema (federation path: tuples round-trip the DB).
"use strict";
const { spawn, spawnSync } = require("child_process");
const fs = require("fs");
const http = require("http");
const net = require("net");
const path = require("path");
const ROOT = path.resolve(__dirname, "..", "..");
const WebSocket = require(path.join(ROOT, "net-bridge", "node_modules", "ws"));

const REF = process.env.KRUNKER_REF || "/home/user/jails/kru/src/Krunker_v3.1.1";
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 && args[i + 1] && !args[i + 1].startsWith("--") ? args[i + 1] : d; };
const BUILD = opt("--build", path.join(ROOT, "cmake-build-debug"));
const USE_DB = args.includes("--db");
const VERBOSE = args.includes("--verbose");
const CHROMIUM = args.includes("--chromium");
const SECRET = "mmparity-" + process.pid;
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function freePort() {
  return new Promise((res) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); });
}

function start(name, cmd, argv, o, readyRe) {
  return new Promise((res, rej) => {
    const p = spawn(cmd, argv, { ...o, stdio: ["ignore", "pipe", "pipe"], detached: true });
    procs.push(p);
    let log = "";
    const t = setTimeout(() => rej(new Error(name + " did not start:\n" + log)), 60000);
    const on = (d) => {
      log += d;
      if (VERBOSE) process.stderr.write(`[${name}] ${d}`);
      const m = readyRe.exec(log);
      if (m) { clearTimeout(t); res({ p, m }); }
    };
    p.stdout.on("data", on);
    p.stderr.on("data", on);
    p.on("exit", (c) => { clearTimeout(t); rej(new Error(`${name} exited ${c}:\n${log}`)); });
  });
}

function cleanup() {
  for (const p of procs) { try { process.kill(-p.pid, "SIGTERM"); } catch (_) {} }
}
process.on("exit", cleanup);
process.on("SIGINT", () => process.exit(130));

// ---- nginx emulation of the reference matchmaker vhost: "/<x>" -> brain "/mm/<x>",
// cors.conf headers on every response ("always").
const CORS_CONF = [
  ["Access-Control-Allow-Origin", "*"],
  ["Access-Control-Allow-Methods", "GET, POST, OPTIONS, PUT, DELETE, PATCH"],
  ["Access-Control-Allow-Headers", "Authorization, Content-Type, X-Requested-With, Accept, Origin, Cache-Control, Range, If-Modified-Since"],
  ["Access-Control-Expose-Headers", "Content-Length, Content-Range"],
  ["Access-Control-Max-Age", "86400"],
];
// /social/<x> and /api/<x> stand for the social/api vhosts (social.<domain>/<x> -> brain
// /social/<x>), i.e. the brain path itself. kn_brain's deployment (deploy/nginx/
// brain.conf.template) does the same rewrite without cors.conf: kn_brain sends CORS.
function nginxFront(port, upstreamPort, cors) {
  const srv = http.createServer((req, res) => {
    const vhost = /^\/(social|api)\//.test(req.url);
    const up = http.request({ host: "127.0.0.1", port: upstreamPort, method: req.method, path: (vhost ? "" : "/mm") + req.url,
      headers: { ...req.headers, host: req.headers.host, "x-real-ip": "127.0.0.1", "x-forwarded-for": "127.0.0.1", "x-forwarded-proto": "https" } }, (u) => {
      const h = {};
      for (const [k, v] of Object.entries(u.headers)) if (!/^(connection|keep-alive|transfer-encoding)$/i.test(k)) h[k] = v;
      if (cors) for (const [k, v] of CORS_CONF) h[k] = v;
      res.writeHead(u.statusCode, h);
      u.pipe(res);
    });
    up.on("error", () => { res.writeHead(502); res.end(); });
    req.pipe(up);
  });
  return new Promise((r) => srv.listen(port, "127.0.0.1", () => r(srv)));
}

// ---- fake game nodes (identical frames to both brains)
const VERSION = "3.1.1";
function meta(o) {
  return { i: o.i, v: VERSION, cs: !!o.cs, m: o.cs ? o.map : null, pr: !!o.pr, earnKR: !o.cs, mode: o.mode || 0,
    modeName: o.modeName || "Free for All", map: o.map, host: o.host, port: o.port, timeLeft: 240 };
}
// Game endpoints nothing listens on (the real client opens its game socket there):
// NY = a closed local port, FRA/SV = unresolvable hosts (SV keeps :6767, the port the
// client pings over http instead of https).
const NY_PORT = 9; // discard: closed on any sane host
const FRA_HOST = "fra.mmparity.invalid", SV_HOST = "sv.mmparity.invalid";
const NODES = [
  { nodeId: "NY", region: "us-nj", publicHost: "127.0.0.1", publicPort: NY_PORT, lobbies: [
    ["aaaaa", "us-nj", 3, 8, meta({ i: "Burg", map: "Burg", host: "127.0.0.1", port: NY_PORT })],
    ["bbbbb", "us-nj", 8, 8, meta({ i: "Sandstorm", map: "Sandstorm", host: "127.0.0.1", port: NY_PORT })],
    ["ccccc", "us-nj", 1, 8, meta({ i: "ffa_MyMap", map: "MyMap", cs: true, pr: true, host: "127.0.0.1", port: NY_PORT })],
  ] },
  { nodeId: "FRA", region: "de-fra", publicHost: FRA_HOST, publicPort: 443, lobbies: [
    ["ddddd", "de-fra", 5, 8, meta({ i: "Kanji", map: "Kanji", host: FRA_HOST, port: 443 })],
  ] },
  { nodeId: "SV", region: "us-ca-sv", publicHost: SV_HOST, publicPort: 6767, lobbies: [
    ["eeeee", "us-ca-sv", 0, 8, meta({ i: "Subzero", map: "Subzero", host: SV_HOST, port: 6767 })],
  ] },
];

function connectNode(url, node) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(url, { headers: { authorization: "Bearer " + SECRET } });
    ws.on("open", () => ws.send(JSON.stringify({ type: "hello", nodeId: node.nodeId, region: node.region, name: node.nodeId,
      publicHost: node.publicHost, publicPort: node.publicPort, version: VERSION, maxPlayers: 8, custom: true, maxLobbies: 4 })));
    ws.on("message", (m) => {
      const f = JSON.parse(m.toString());
      if (f.type === "welcome") { ws.send(JSON.stringify({ type: "snapshot", lobbies: node.lobbies })); res(ws); }
      else if (f.type === "ping") ws.send(JSON.stringify({ type: "pong", ts: f.ts }));
    });
    ws.on("error", rej);
  });
}

// ---- requests
function get(port, p, o = {}) {
  return new Promise((res, rej) => {
    const r = http.request({ host: "127.0.0.1", port, path: p, method: o.method || "GET",
      headers: { "accept-language": "en-US,en;q=0.6;", ...(o.headers || {}) } }, (s) => {
      let b = ""; s.on("data", (d) => b += d); s.on("end", () => res({ status: s.statusCode, headers: s.headers, body: b }));
    });
    r.on("error", rej);
    r.end();
  });
}
const IGNORED_HEADERS = /^(date|server|connection|keep-alive|transfer-encoding|content-length|vary)$/i;
function headerList(h) {
  return Object.entries(h).filter(([k]) => !IGNORED_HEADERS.test(k)).map(([k, v]) => `${k.toLowerCase()}: ${v}`).sort();
}
// JSON with key order; volatile values masked
function shape(text, mask) {
  let v;
  try { v = JSON.parse(text); } catch (_) { return "RAW " + text; }
  const walk = (x, key) => {
    if (mask.includes(key)) return "<" + key + ">";
    if (Array.isArray(x)) return x.map((y) => walk(y));
    if (x && typeof x === "object") { const o = {}; for (const k of Object.keys(x)) o[k] = walk(x[k], k); return o; }
    return x;
  };
  return JSON.stringify(walk(v));
}

const q = (o) => "?" + new URLSearchParams(o).toString();
const HOSTNAME = { hostname: "127.0.0.1" };
const DQ = JSON.stringify({ v: VERSION });
// [name, path (client form, no /mm), {mask, dev: reason if a deviation is expected}]
const EMPTY_CASES = [
  ["empty ping-list", "/ping-list" + q(HOSTNAME), {}],
  ["empty game-list", "/game-list" + q(HOSTNAME), {}],
  ["empty game-info", "/game-info" + q({ game: "NY:aaaaa" }), { dev: "error name: kn NoAvailableServers (client name), ref no_lobbies" }],
  ["empty seek", "/seek-game" + q({ ...HOSTNAME, region: "us-nj", autoChangeGame: false, dataQuery: DQ }),
    { dev: "error name: kn NoAvailableServers (client name), ref no_lobbies" }],
  ["empty player-count", "/social/player-count", {}],
];
const SEEK = { mask: ["clientId"], knExtra: ["region", "webUrl"] };
const CASES = [
  ["ping-list", "/ping-list" + q(HOSTNAME), {}],
  ["game-list", "/game-list" + q(HOSTNAME), {}],
  ["game-info existing", "/game-info" + q({ game: "NY:aaaaa" }), {}],
  ["game-info custom", "/game-info" + q({ game: "NY:ccccc" }), {}],
  ["game-info full", "/game-info" + q({ game: "NY:bbbbb" }), {}],
  ["game-info unknown", "/game-info" + q({ game: "NY:zzzzz" }), {}],
  ["game-info none", "/game-info", {}],
  ["seek default region", "/seek-game" + q({ ...HOSTNAME, region: "de-fra", autoChangeGame: false, dataQuery: DQ }), SEEK],
  ["seek requested room", "/seek-game" + q({ ...HOSTNAME, region: "us-nj", autoChangeGame: false, game: "NY:aaaaa", dataQuery: DQ }), SEEK],
  ["seek requested custom", "/seek-game" + q({ ...HOSTNAME, region: "us-nj", autoChangeGame: true, game: "NY:ccccc" }), SEEK],
  ["seek requested full, autoChange", "/seek-game" + q({ ...HOSTNAME, region: "us-nj", autoChangeGame: true, game: "NY:bbbbb", dataQuery: DQ }),
    { ...SEEK, dev: "kn quick join stays in the requested region (us-nj -> NY:aaaaa); ref ignores region (FRA:ddddd, most populated)" }],
  ["seek region us-nj", "/seek-game" + q({ ...HOSTNAME, region: "us-nj", autoChangeGame: false, dataQuery: DQ }),
    { ...SEEK, dev: "kn quick join stays in the requested region; ref ignores region" }],
  ["OPTIONS preflight seek", "/seek-game?region=us-nj", { method: "OPTIONS", headers: { origin: "http://x.test", "access-control-request-method": "GET",
    "access-control-request-headers": "authorization" }, dev: "kn answers a preflight 204 without issuing a ticket; ref runs the seek" }],
  ["OPTIONS preflight game-list", "/game-list", { method: "OPTIONS", headers: { origin: "http://x.test", "access-control-request-method": "GET" } }],
  ["HEAD game-list", "/game-list", { method: "HEAD" }],
  ["seek with Origin", "/seek-game" + q({ ...HOSTNAME, region: "de-fra", autoChangeGame: false }), { ...SEEK, headers: { origin: "http://x.test" } }],
  ["seek requested full, no autoChange", "/seek-game" + q({ ...HOSTNAME, region: "us-nj", autoChangeGame: false, game: "NY:bbbbb", dataQuery: DQ }),
    { ...SEEK, dev: "kn: 409 GameFull (the client's matchmaker.full2); ref: overflow into another lobby" }],
  ["seek requested unknown, no autoChange", "/seek-game" + q({ ...HOSTNAME, region: "us-nj", autoChangeGame: false, game: "XX:nope", dataQuery: DQ }),
    { ...SEEK, dev: "kn: 404 InvalidGameId (the client's matchmaker.invalid); ref: overflow" }],
  ["seek FORCE_ENTER full", "/seek-game" + q({ ...HOSTNAME, region: "us-nj", autoChangeGame: false, game: "NY:bbbbb" }), { ...SEEK, headers: { cookie: "a=b; FORCE_ENTER=1" } }],
  ["social player-count", "/social/player-count", {}],
  ["api player-count", "/api/player-count", {}],
  ["unknown route", "/nope", {}],
  ["POST game-list", "/game-list", { method: "POST" }],
  ["queue poll (no ref route)", "/queue/poll" + q({ clientKey: "x" }), { dev: "kn implements the v3.1.1 ranked queue routes (section 6.5); the reference has none" }],
];

const extras = new Set();
function dropTrailing(text, keys) {
  let v;
  try { v = JSON.parse(text); } catch (_) { return text; }
  if (!v || typeof v !== "object" || Array.isArray(v)) return text;
  const ks = Object.keys(v);
  while (ks.length && keys.includes(ks[ks.length - 1])) { extras.add(ks[ks.length - 1]); delete v[ks.pop()]; }
  return JSON.stringify(v);
}

async function compareHttp(cases, refFront, kn) {
  let diffs = 0, devs = 0;
  for (const [name, p, o] of cases) {
    const ro = { method: o.method, headers: o.headers };
    const ref = await get(refFront, p, ro);
    const k = await get(kn, p, ro);
    const mask = o.mask || [];
    const a = { status: ref.status, headers: headerList(ref.headers), body: shape(ref.body, mask) };
    const b = { status: k.status, headers: headerList(k.headers), body: shape(o.knExtra ? dropTrailing(k.body, o.knExtra) : k.body, mask) };
    const same = JSON.stringify(a) === JSON.stringify(b);
    if (same) { console.log(`  same  ${name}`); if (VERBOSE) console.log("        " + a.body); continue; }
    const tag = o.dev ? "DEV " : "DIFF";
    if (o.dev) devs++; else diffs++;
    console.log(`  ${tag}  ${name}  ${p}${o.dev ? "  (" + o.dev + ")" : ""}`);
    if (a.status !== b.status) console.log(`        status ref ${a.status}  kn ${b.status}`);
    const ah = new Set(a.headers), bh = new Set(b.headers);
    for (const h of a.headers) if (!bh.has(h)) console.log(`        header ref only: ${h}`);
    for (const h of b.headers) if (!ah.has(h)) console.log(`        header kn only:  ${h}`);
    if (a.body !== b.body) console.log(`        body ref ${a.body}\n        body kn  ${b.body}`);
  }
  return { diffs, devs };
}

async function main() {
  const brainBin = path.join(BUILD, "services", "brain", "kn_brain");
  if (!fs.existsSync(brainBin)) throw new Error("no kn_brain in " + BUILD);
  const refPort = await freePort(), frontPort = await freePort();
  const ref = start("ref", "npm", ["run", "matchmaker"], { cwd: REF, env: { ...process.env, MM_HOST: "127.0.0.1", MM_PORT: String(refPort),
    MM_INTERNAL_SECRET: SECRET, MM_SERVE_SITE: "0" } }, /listening on http/);
  const env = { ...process.env, KN_LOG_LEVEL: VERBOSE ? "debug" : "warn" };
  const knArgs = ["--brain.port=0", "--brain.internal_secret=" + SECRET, "--brain.id=mmparity",
    // equivalent ping targets: the reference pings the game nodes' public host:port
    "--brain.regions=" + JSON.stringify([{ id: "us-nj", name: "New York", ping_host: "127.0.0.1:" + NY_PORT },
      { id: "de-fra", name: "Frankfurt", ping_host: FRA_HOST }, { id: "us-ca-sv", name: "Silicon Valley", ping_host: SV_HOST + ":6767" }])];
  let schema = null;
  if (USE_DB) {
    schema = "kn_test_mmp_" + process.pid;
    const dbName = JSON.parse(fs.readFileSync(path.join(ROOT, "config", "config.json"), "utf8")).database.name;
    const r = spawnSync("python3", ["-I", path.join(ROOT, "services/brain/tests/pg_schema.py"), ROOT, dbName, "create", schema], { stdio: "inherit" });
    if (r.status) throw new Error("schema create failed");
    process.on("exit", () => spawnSync("python3", ["-I", path.join(ROOT, "services/brain/tests/pg_schema.py"), ROOT, dbName, "drop", schema]));
    env.KN_DATABASE_SEARCH_PATH = schema;
    knArgs.push("--migrate");
  } else knArgs.push("--no-db");
  // KN_LOG_LEVEL=warn hides the listening line: force info for it
  env.KN_LOG_LEVEL = "info";
  const kn = await start("kn", brainBin, knArgs, { cwd: ROOT, env }, /listening on http:\/\/[0-9.]+:(\d+) \(ws/);
  const knPort = Number(kn.m[1]);
  // --db: a second brain on the same schema sees the nodes only through PostgreSQL
  let kn2Port = 0;
  if (USE_DB) {
    const k2 = await start("kn2", brainBin, knArgs.filter((a) => a !== "--migrate").map((a) => a.startsWith("--brain.id=") ? "--brain.id=mmparity2" : a),
      { cwd: ROOT, env }, /listening on http:\/\/[0-9.]+:(\d+) \(ws/);
    kn2Port = Number(k2.m[1]);
  }
  await ref;
  await nginxFront(frontPort, refPort, true);
  const knFrontPort = await freePort();
  await nginxFront(knFrontPort, knPort, false);
  console.log(`reference brain :${refPort} (nginx front :${frontPort}), kn_brain :${knPort} (nginx front :${knFrontPort})` +
    (USE_DB ? ` (PostgreSQL ${schema})` : " (--no-db)"));
  console.log("no game servers:");
  const e = await compareHttp(EMPTY_CASES, frontPort, knFrontPort);
  // the same requests on kn_brain's prefix-less aliases (no nginx; local development)

  const links = [];
  for (const n of NODES) {
    links.push(await connectNode(`ws://127.0.0.1:${refPort}/mm/internal/ws`, n));
    links.push(await connectNode(`ws://127.0.0.1:${knPort}/mm/internal/ws`, n));
  }
  await sleep(USE_DB ? 1500 : 300);
  console.log("3 game servers, 5 lobbies:");
  const r = await compareHttp(CASES, frontPort, knFrontPort);
  if (kn2Port) {
    const kn2Front = await freePort();
    await nginxFront(kn2Front, kn2Port, false);
    console.log("second kn_brain (federation: lobbies read back from PostgreSQL):");
    const f = await compareHttp(CASES.filter((c) => /^\/(game-list|game-info|ping-list|social|api)/.test(c[1])).map((c) => ["fed " + c[0], c[1], c[2]]), frontPort, kn2Front);
    r.diffs += f.diffs;
  }
  console.log("kn_brain prefix-less aliases vs its nginx front:");
  const a2 = await compareHttp(CASES.filter((c) => !/^\/(social|api)\/|^\/nope/.test(c[1]) && !c[2].knExtra).map((c) => ["alias " + c[0], c[1], { ...c[2], dev: undefined }]), knFrontPort, knPort);
  r.diffs += e.diffs + a2.diffs; r.devs += e.devs;
  if (extras.size) console.log(`  DEV   seek-game: kn appends ${SEEK.knExtra.filter((k) => extras.has(k)).join(", ")} after the v3.1.1 keys (KrunkNative client: cross-region label, web endpoint)`);
  if (CHROMIUM) await require("./mm-parity-client.js").run({ refFront: frontPort, kn: knPort, verbose: VERBOSE });
  console.log(`${r.diffs} unexpected difference(s), ${r.devs} documented deviation(s)`);
  for (const l of links) l.close();
  process.exitCode = r.diffs ? 1 : 0;
}

main().catch((e) => { console.error(e.stack || e); process.exitCode = 2; }).finally(() => setTimeout(() => process.exit(), 200));
