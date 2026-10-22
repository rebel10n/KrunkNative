#!/usr/bin/env node
// The playability check from Node (make playable-node). The same check as playable.sh, with the stack started and stopped
// from here: the brain, the game server, the v10 bridge and the harness page run as children of this process on free
// loopback ports, each with a pid file ($RUN/<name>.pid: "pid command-name"). The driver is playable() of krunker-local.mjs
// (the official client in headless Chromium over CDP, the second player included), imported, so it is the same driving the
// harness runs. The report goes to $RUN/playable-report.json and playable-check.mjs asserts it, unchanged.
//   node tools/krunker-local/playable-node.mjs
// Env, as playable.sh: KN_PLAYABLE_HOME (absolute; default ~/.cache/kn-playable-node, its own run dir so it never stops a
// playable.sh stack), KN_PLAYABLE_ROUND_S (90), KN_PLAYABLE_MAX_EXC (0), KN_PLAYABLE_PLAYERS (2), KN_BUILD (default
// <repo>/cmake-build-debug). Exit 0 only when every assertion passes. Only processes this run started are signalled, by pid
// from its pid files, and only while /proc/<pid>/cmdline still names the command. Loopback only; headless only.
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";
import { spawn } from "node:child_process";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../..");
const HOME_DIR = process.env.KN_PLAYABLE_HOME || path.join(os.homedir(), ".cache/kn-playable-node");
if (!path.isAbsolute(HOME_DIR)) {
  console.error("playable-node: KN_PLAYABLE_HOME must be an absolute path");
  process.exit(1);
}
const RUN = path.join(HOME_DIR, "run");
const BUILD = process.env.KN_BUILD || path.join(REPO, "cmake-build-debug");
const ROUND_S = Number(process.env.KN_PLAYABLE_ROUND_S || 90);
const MAX_EXC = Number(process.env.KN_PLAYABLE_MAX_EXC || 0);
const PLAYERS = Number(process.env.KN_PLAYABLE_PLAYERS || 2);
const RUN_LIMIT_MS = 1500 * 1000; // the bash wrapper's `timeout 1500`
const HARNESS = path.join(HERE, "krunker-local.mjs");
const CHECK = path.join(HERE, "playable-check.mjs");
const REPORT = path.join(RUN, "playable-report.json");
const PRIVATE = { mode: 0o600 };

const log = (m) => console.error(`playable-node: ${m}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// a loopback port is busy when something accepts on it
function listening(port) {
  return new Promise((resolve) => {
    const s = net.connect({ host: "127.0.0.1", port });
    const done = (v) => { s.destroy(); resolve(v); };
    s.once("connect", () => done(true));
    s.once("error", () => done(false));
    s.setTimeout(500, () => done(false));
  });
}

// the ports this run uses: free on the host when picked, never reused within the run
const used = new Set();
async function pick(from) {
  let p = from;
  while (used.has(p) || (await listening(p))) p++;
  used.add(p);
  return p;
}

const pidFile = (name) => path.join(RUN, `${name}.pid`);
function recordPid(name, pid, bin) {
  fs.writeFileSync(pidFile(name), `${pid} ${bin}\n`, PRIVATE);
}

// the process is ours to signal only while its command line still names the command (pids are reused)
function isNamed(pid, bin) {
  try {
    return fs.readFileSync(`/proc/${pid}/cmdline`).toString("utf8").split("\0").join(" ").includes(bin);
  } catch {
    return false;
  }
}

// stop what this run started (pid files in RUN only; leftovers of an earlier run in the same home included)
async function stopOurs() {
  if (!fs.existsSync(RUN)) return;
  for (const f of fs.readdirSync(RUN).filter((x) => x.endsWith(".pid"))) {
    const file = path.join(RUN, f);
    const [pidText, bin] = fs.readFileSync(file, "utf8").trim().split(/\s+/);
    const pid = Number(pidText);
    if (Number.isInteger(pid) && pid > 1 && bin && isNamed(pid, bin)) {
      try { process.kill(pid, "SIGTERM"); } catch { /* already gone */ }
      for (let i = 0; i < 10 && isNamed(pid, bin); i++) await sleep(500);
      if (isNamed(pid, bin)) { try { process.kill(pid, "SIGKILL"); } catch { /* already gone */ } }
    }
    fs.rmSync(file, { force: true });
  }
}

// start NAME BIN PORT CMD ARGS ENV: a background child with its output in $RUN/NAME.out; up when PORT listens
async function start(name, bin, port, cmd, args, env) {
  const outFile = path.join(RUN, `${name}.out`);
  const out = fs.openSync(outFile, "w", 0o600);
  let child;
  try {
    child = spawn(cmd, args, { cwd: REPO, env, stdio: ["ignore", out, out] });
  } finally {
    fs.closeSync(out);
  }
  child.on("error", () => {}); // a failed spawn shows as no pid, below
  if (!child.pid) throw new Error(`${name} did not start`);
  recordPid(name, child.pid, bin);
  for (let i = 0; i < 120; i++) {
    if (child.exitCode !== null || child.signalCode !== null) throw new Error(`${name} exited (see ${outFile})`);
    if (await listening(port)) { log(`${name} up on ${port}`); return child; }
    await sleep(500);
  }
  throw new Error(`${name} did not listen on ${port} (see ${outFile})`);
}

// a node command to the end: its exit code (its output to logFile, or to this process's when none)
function runNode(args, { env = process.env, logFile = null } = {}) {
  const fd = logFile ? fs.openSync(logFile, "w", 0o600) : "inherit";
  return new Promise((resolve, reject) => {
    const c = spawn(process.execPath, args, { cwd: REPO, env, stdio: ["ignore", fd, fd] });
    const close = () => { if (typeof fd === "number") fs.closeSync(fd); };
    c.once("error", (e) => { close(); reject(e); });
    c.once("exit", (code) => { close(); resolve(code === null ? 1 : code); });
  });
}

function brainSecret() {
  const f = path.join(RUN, "brain.secret");
  if (!fs.existsSync(f) || fs.statSync(f).size === 0) {
    fs.writeFileSync(f, `${crypto.randomBytes(16).toString("hex")}\n`, PRIVATE);
  }
  return fs.readFileSync(f, "utf8").trim();
}

// the harness config for this run (git-ignored): this checkout's client, this run's dirs and ports
function writeHarnessConfig(ports) {
  const c = JSON.parse(fs.readFileSync(path.join(HERE, "config.example.json"), "utf8"));
  c.wwwDir = `${HOME_DIR}/krunker/www`;
  c.runDir = `${HOME_DIR}/krunker/run`;
  c.port = ports.http;
  c.tlsPort = ports.tls;
  c.localUrl = `http://krunker-local.test:${ports.http}/`;
  c.brainUrl = `http://127.0.0.1:${ports.brain}`;
  c.bridge.port = ports.bridge;
  c.bridge.log = `${RUN}/bridge.out`;
  c.loadWaitMs = 20000;
  c.afterAcceptMs = 6000;
  c.joinWaitMs = 150000;
  fs.writeFileSync(path.join(HERE, "config.json"), `${JSON.stringify(c, null, 2)}\n`);
}

async function main() {
  for (const b of [path.join(BUILD, "services/brain/kn_brain"), path.join(BUILD, "server_bin"), path.join(BUILD, "v10_bridge")]) {
    try { fs.accessSync(b, fs.constants.X_OK); } catch { throw new Error(`missing ${b} (run make build first)`); }
  }
  fs.mkdirSync(RUN, { recursive: true, mode: 0o700 });
  await stopOurs();

  const base = 20000 + Math.floor(Math.random() * 25000);
  const brain = await pick(base);
  const server = await pick(brain + 1);
  const bridge = await pick(server + 1);
  const http = await pick(bridge + 1);
  const tls = await pick(http + 1);

  // a fresh run: a new brain database and an empty server config; the brain secret is generated once, never printed
  for (const f of ["brain.db", "brain.db-shm", "brain.db-wal", "brain.db-journal"]) fs.rmSync(path.join(RUN, f), { force: true });
  fs.writeFileSync(path.join(RUN, "empty.json"), "{}\n", PRIVATE);
  const secret = brainSecret();
  writeHarnessConfig({ http, tls, brain, bridge });
  const env = { ...process.env, KN_BRAIN_INTERNAL_SECRET: secret };

  await start("brain", "kn_brain", brain, path.join(BUILD, "services/brain/kn_brain"), [
    "--brain.host=127.0.0.1", `--brain.port=${brain}`,
    "--database.driver=sqlite", `--database.path=${path.join(RUN, "brain.db")}`, "--migrate",
    "--brain.region=local", "--brain.id=playable", "--brain.public_url=https://matchmaker.beta.krunker.io",
    "--accounts.signup_rewards.enabled=false", "--finder.poll_interval_s=1",
  ], env);
  await start("server", "server_bin", server, path.join(BUILD, "server_bin"), [
    "--cfg", "none", "--config", path.join(RUN, "empty.json"), "--headless", "--no-cli",
    "--port", String(server), "--mode", "ffa", "--round-seconds", String(ROUND_S),
    "--region", "us-nj", "--node-id", "PLAYABLE", "--name", "Krunker Box Office playable",
    "--public-host", "127.0.0.1", "--public-port", String(server),
    "--brain", `ws://127.0.0.1:${brain}/mm/internal/ws`, "--log", path.join(RUN, "server.log"),
    path.join(REPO, "assets/maps/official/00_burg.json"),
  ], { ...env, KN_GAMESERVER_BRAIN_SECRET: secret });
  await start("bridge", "v10_bridge", bridge, path.join(BUILD, "v10_bridge"), [
    `--v10-listen=${bridge}`, "--bind=127.0.0.1", `--native=127.0.0.1:${server}`,
  ], { ...env, KRUNK_NATIVE_V10_TRACE: "1" });

  const prep = await runNode([HARNESS, "prepare"], { env, logFile: path.join(RUN, "prepare.log") });
  if (prep !== 0) throw new Error(`prepare failed (see ${path.join(RUN, "prepare.log")})`);
  await start("harness", "krunker-local.mjs", http, process.execPath, [HARNESS, "serve"], env);

  // the driver: playable() of krunker-local.mjs, the same CDP run as `krunker-local.mjs playable`
  process.env.KN_ROUND_SECONDS = String(ROUND_S);
  process.env.KN_PLAYABLE_PLAYERS = String(PLAYERS);
  const { playable } = await import(pathToFileURL(HARNESS).href);
  let chromes = 0;
  const onChild = (c) => {
    if (!c.pid) return;
    const name = `chromium-${++chromes}`;
    recordPid(name, c.pid, "chrome");
    c.once("exit", () => fs.rmSync(pidFile(name), { force: true }));
  };
  let timer;
  const limit = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`the run took longer than ${RUN_LIMIT_MS / 1000} s`)), RUN_LIMIT_MS);
  });
  let report;
  try {
    report = await Promise.race([playable({ onChild }), limit]);
  } finally {
    clearTimeout(timer);
  }
  fs.writeFileSync(REPORT, `${JSON.stringify(report, null, 2)}\n`, PRIVATE);
  log(`report: ${REPORT}`);
  return runNode([CHECK, REPORT, String(MAX_EXC)]);
}

async function cli() {
  let code = 1;
  const onSignal = (sig) => {
    log(`${sig}: stopping this run's processes`);
    stopOurs().finally(() => process.exit(1));
  };
  process.once("SIGINT", () => onSignal("SIGINT"));
  process.once("SIGTERM", () => onSignal("SIGTERM"));
  try {
    code = await main();
  } catch (e) {
    console.error(`playable-node: ${e.message}`);
  } finally {
    await stopOurs();
  }
  process.exit(code);
}

cli();
