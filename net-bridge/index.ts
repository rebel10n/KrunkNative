"use strict";

import { createServer, type IncomingMessage, type ServerResponse } from "http";
import fs from "fs";
import path from "path";
import type { AddressInfo, Socket } from "net";
import version from "./version.json";
import * as krunkerProxy from "./krunkerProxy";
import * as krunkerRelay from "./krunkerRelay";
import * as brainProxy from "./brainProxy";
import * as browserProxy from "./proxy";
import * as v10Proxy from "./v10Proxy";
import * as publicRoutes from "./publicRoutes";
import * as staticFiles from "./staticFiles";
import * as webBridge from "./webBridge";

type BridgeMode = "native-to-krunker" | "browser-to-native" | "v10-to-native" | "web" | "bridge-only";

const BRIDGE_MODES: readonly BridgeMode[] = ["native-to-krunker", "browser-to-native", "v10-to-native", "web", "bridge-only"];

// `npm run web:build` at the repo root writes the Emscripten client here.
const DEFAULT_WEB_DIR = path.resolve(__dirname, "..", "dist", "web");

type CliOptions = {
  mode: BridgeMode;
  webDir?: string;
  listenHost?: string;
  listenPort?: number;
  krunkerOrigin?: string;
  krunkerWsUrl?: string;
  region?: string;
  hostname?: string;
  gameId?: string;
  brainUrl?: string;
};

function usage(): string {
  return [
    `net-bridge ${version}`,
    "",
    "Default: listen on the KrunkNative TCP port and bridge to a Krunker v3.1.1 WebSocket server.",
    "",
    "Flags:",
    "  --native-to-krunker       Run native TCP -> Krunker v3.1.1 WS mode (default)",
    "  --browser-to-native       Run the old browser WS -> KrunkNative TCP mode",
    "  --v10-to-native           Run the Krunker v10.0.x client WS (msgpack, v10 labels) -> KrunkNative TCP mode",
    "                            (partial: connect and join; stops at the native INIT, see v10Proxy.ts)",
    "  --web                     Serve the Emscripten web build over HTTP and bridge",
    "                            ws /native <-> native TCP (raw passthrough)",
    "  --bridge-only             Only the ws /native passthrough (+ GET /healthz): no static files,",
    "                            no brain routes, no /ws (the deployed bridge, deploy/ansible kn_bridge)",
    "  --brain URL               kn_brain to proxy /mm /api /social to in web mode (default: KN_BRAIN_URL,",
    "                            config/config.json brain, else http://127.0.0.1:5050)",
    "  --serve-web DIR           Same as --web, serving DIR (default ../dist/web,",
    "                            env KRUNK_NATIVE_WEB_DIR)",
    "  --krunker-origin URL      Matchmaker origin for native-to-krunker mode (default http://127.0.0.1:6767)",
    "  --krunker-ws URL          Skip matchmaker and connect directly to this ws://.../ws URL",
    "  --listen-host HOST        Native TCP listen host in native-to-krunker mode",
    "  --listen-port PORT        Native TCP listen port in native-to-krunker mode (default 21015)",
    "  --region REGION           Matchmaker region (default local)",
    "  --hostname HOSTNAME       Matchmaker hostname query (default 127.0.0.1)",
    "  --game GAME_ID            Request a specific v3.1.1 lobby id",
    "",
    "Environment (web / browser-to-native):",
    "  KRUNK_NATIVE_BRIDGE_HOST / KRUNK_NATIVE_BRIDGE_PORT   HTTP listen address (127.0.0.1:8080)",
    "  KRUNK_NATIVE_HOST / KRUNK_NATIVE_PORT                 native TCP server (127.0.0.1:21015)",
    "  KRUNK_NATIVE_TARGETS                                  /native?target=host:port allowlist (brain matchmaking),",
    "                                                        host:port|host:lo-hi|host:* comma list",
    "                                                        (default KRUNK_NATIVE_HOST:PORT..PORT+63)",
    "  KRUNK_NATIVE_ALLOW_LOOPBACK=1                         also any loopback target (dev; default on without TARGETS)",
    "  KRUNK_NATIVE_MAX_PER_IP / KRUNK_NATIVE_MAX_SESSIONS    /native session caps (0 = off)",
    "  KRUNK_NATIVE_IDLE_TIMEOUT_MS / KRUNK_NATIVE_WS_PING_MS  idle close (0 = off) / ws ping (30000)",
    "  KRUNK_NATIVE_CLIENT_IP_HEADER                         client address header from a loopback proxy",
    "                                                        (e.g. cf-connecting-ip; default X-Forwarded-For)",
    "  KRUNK_NATIVE_WEB_DIR                                  web build dir for --web",
    "  KRUNK_NATIVE_WEB_COI=1                                add COOP/COEP headers (SharedArrayBuffer)",
    "  KN_BRIDGE_KRUNKER_PROXY=socks5h://user:pass@HOST:PORT  send krunker.io traffic (/krunker-relay/http, /krunker-relay/ws)",
    "                                                        through this internal proxy (web / bridge-only; unset = off)",
  ].join("\n");
}

function readFlagValue(args: string[], index: number, prefix: string): [string | null, number] {
  const arg = args[index];
  if (arg.startsWith(`${prefix}=`)) return [arg.slice(prefix.length + 1), index];
  if (arg === prefix && index + 1 < args.length) return [args[index + 1], index + 1];
  return [null, index];
}

function parsePort(value: string | null): number | undefined {
  if (!value) return undefined;
  const port = Number.parseInt(value, 10);
  return port > 0 && port <= 65535 ? port : undefined;
}

function parseArgs(argv: string[]): CliOptions {
  const options: CliOptions = { mode: "native-to-krunker" };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];

    if (arg === "--help" || arg === "-h") {
      console.info(usage());
      process.exit(0);
    }
    if (arg === "--native-to-krunker") {
      options.mode = "native-to-krunker";
      continue;
    }
    if (arg === "--browser-to-native") {
      options.mode = "browser-to-native";
      continue;
    }
    if (arg === "--v10-to-native") {
      options.mode = "v10-to-native";
      continue;
    }
    if (arg === "--web") {
      options.mode = "web";
      continue;
    }
    if (arg === "--bridge-only") {
      options.mode = "bridge-only";
      continue;
    }
    if (arg === "--serve-web" || arg.startsWith("--serve-web=")) {
      const [value, nextIndex] = readFlagValue(argv, i, "--serve-web");
      if (!value) throw new Error("--serve-web requires a directory");
      options.mode = "web";
      options.webDir = value;
      i = nextIndex;
      continue;
    }
    if (arg === "--brain" || arg.startsWith("--brain=")) {
      const [value, nextIndex] = readFlagValue(argv, i, "--brain");
      if (!value) throw new Error("--brain requires a URL");
      options.brainUrl = value;
      i = nextIndex;
      continue;
    }
    if (arg.startsWith("--mode=")) {
      const mode = arg.slice("--mode=".length) as BridgeMode;
      if (BRIDGE_MODES.includes(mode)) options.mode = mode;
      else throw new Error(`unknown bridge mode: ${mode}`);
      continue;
    }

    const flagReaders: Array<[string, keyof CliOptions, (value: string | null) => unknown]> = [
      ["--krunker-origin", "krunkerOrigin", (value) => value || undefined],
      ["--krunker-ws", "krunkerWsUrl", (value) => value || undefined],
      ["--listen-host", "listenHost", (value) => value || undefined],
      ["--listen-port", "listenPort", parsePort],
      ["--region", "region", (value) => value || undefined],
      ["--hostname", "hostname", (value) => value || undefined],
      ["--game", "gameId", (value) => value || undefined],
    ];

    let matched = false;
    for (const [flag, key, parse] of flagReaders) {
      if (arg === flag || arg.startsWith(`${flag}=`)) {
        const [value, nextIndex] = readFlagValue(argv, i, flag);
        const parsed = parse(value);
        if (parsed !== undefined) (options as any)[key] = parsed;
        i = nextIndex;
        matched = true;
        break;
      }
    }
    if (!matched) throw new Error(`unknown argument: ${arg}`);
  }
  return options;
}

function bridgeListenAddress(): { host: string; port: number } {
  const host = process.env.KRUNK_NATIVE_BRIDGE_HOST || process.env.MM_HOST || "127.0.0.1";
  const port =
    Number.parseInt(process.env.KRUNK_NATIVE_BRIDGE_PORT || process.env.MM_PORT || "8080", 10) ||
    8080;
  return { host, port };
}

function describeAddress(http: ReturnType<typeof createServer>): string {
  const address = http.address() as AddressInfo | string | null;
  if (!address || typeof address === "string") return "";
  const host = address.family === "IPv6" ? `[${address.address}]` : address.address;
  return `http://${host}:${address.port}`;
}

/**
 * Hand a `/native` upgrade to the passthrough, or refuse it: a WebSocket handshake is
 * completed and closed at once with the refusal's close code and reason (the page shows
 * them; webBridge.admit), anything else gets the HTTP status.
 */
function upgradeNative(
  wss: ReturnType<typeof webBridge.createServer>,
  request: IncomingMessage,
  socket: Socket,
  head: Buffer,
): void {
  const refused = webBridge.admit(request);
  if (refused) {
    console.warn(
      `[web-bridge] ${request.socket.remoteAddress || "?"} upgrade refused ${refused.close}: ${refused.reason} (${request.url})`,
    );
    if (!/websocket/i.test(String(request.headers.upgrade || "")) || !request.headers["sec-websocket-key"]) {
      socket.end(
        `HTTP/1.1 ${refused.status} ${refused.reason}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`,
      );
      return;
    }
    wss.handleUpgrade(request, socket, head, (ws) => {
      ws.on("error", () => ws.terminate());
      ws.close(refused.close, refused.reason);
    });
    return;
  }
  wss.handleUpgrade(request, socket, head, (ws) => {
    wss.emit("connection", ws, request);
  });
}

/**
 * `--bridge-only`: the deployed bridge (play.krunk.io behind the Cloudflare tunnel). Only
 * the `/native` passthrough and a health check; the page, the assets and the brain are
 * served elsewhere.
 */
function startBridgeOnly(): ReturnType<typeof createServer> {
  const relay = relayOrExit();
  const http = createServer((req, res) => {
    const pathname = (req.url || "/").split("?")[0];
    if (req.method === "GET" && pathname === "/healthz") {
      res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store" });
      res.end(JSON.stringify({ ok: true, version, ...webBridge.getStats() }));
      return;
    }
    if (relay.handlesHttp(pathname)) {
      relay.handleHttp(req, res);
      return;
    }
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ error: "not_found" }));
  });
  const nativeWss = webBridge.createServer();
  http.on("upgrade", (request, socket: Socket, head) => {
    let pathname = "";
    try {
      pathname = new URL(request.url || "/", "http://localhost").pathname;
    } catch (_err) {
      pathname = "";
    }
    if (relay.handlesUpgrade(pathname)) {
      relay.handleUpgrade(request, socket, head);
      return;
    }
    if (pathname !== webBridge.WEB_WS_PATH) {
      socket.destroy();
      return;
    }
    upgradeNative(nativeWss, request, socket, head);
  });
  http.on("close", () => nativeWss.close());
  http.on("listening", () => {
    console.info(
      `net-bridge ${version} bridge-only listening on ${describeAddress(http) || "(unknown address)"} ` +
        `(ws ${webBridge.WEB_WS_PATH}, GET /healthz)`,
    );
  });
  http.listen(bridgeListenAddress());
  return http;
}

// KN_BRIDGE_KRUNKER_PROXY is read once at startup; a value that is not a proxy URL stops the bridge
// (a silent direct connection would be the wrong way round). The message holds no credentials.
function relayOrExit(): krunkerRelay.KrunkerRelay {
  try {
    return krunkerRelay.relayFromEnv();
  } catch (err) {
    console.error(`net-bridge: ${(err as Error).message}`);
    process.exit(2);
  }
}

/**
 * `--web`: static web build + raw `/native` WebSocket passthrough to the
 * native TCP server. The legacy `/ws` translator stays attached because it
 * costs nothing and lets the same bridge serve both clients.
 */
function startWeb(options: CliOptions): ReturnType<typeof createServer> {
  const relay = relayOrExit();
  const webDir = path.resolve(
    options.webDir || process.env.KRUNK_NATIVE_WEB_DIR || DEFAULT_WEB_DIR,
  );
  if (!fs.existsSync(path.join(webDir, "index.html"))) {
    console.warn(
      `[net-bridge] web dir ${webDir} has no index.html (run \`npm run web:build\` at the repo root); ` +
        "only the /native and /ws bridges will work",
    );
  }

  const extraHeaders: Record<string, string> = {};
  if (process.env.KRUNK_NATIVE_WEB_COI === "1") {
    // Required for SharedArrayBuffer (Emscripten -pthread) on modern browsers.
    extraHeaders["Cross-Origin-Opener-Policy"] = "same-origin";
    extraHeaders["Cross-Origin-Embedder-Policy"] = "require-corp";
  }

  const serveOptions = {
    root: webDir,
    longCachePrefixes: ["/assets/"],
    extraHeaders,
  };

  // the brain's routes are proxied to kn_brain (same origin, like nginx); without a brain the
  // bridge's own fallback routes answer (brainProxy.ts)
  const brain = brainProxy.resolveBrain(options.brainUrl);
  const localRoutes = async (req: IncomingMessage, res: ServerResponse) => {
    if (
      publicRoutes.handle(req, res, {
        getStats: webBridge.getStats,
      })
    ) return;
    if (await staticFiles.serve(req, res, serveOptions)) return;
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ error: "not_found", path: req.url }));
  };

  const http = createServer(async (req, res) => {
    try {
      const pathname = (req.url || "/").split("?")[0];
      if (relay.handlesHttp(pathname)) {
        relay.handleHttp(req, res);
        return;
      }
      if (brainProxy.isBrainPath(pathname)) {
        brainProxy.proxyHttp(brain, req, res, () => {
          localRoutes(req, res).catch((err) => {
            console.error("[net-bridge] fallback route threw:", err);
          });
        });
        return;
      }
      if (
        publicRoutes.handle(req, res, {
          getStats: webBridge.getStats,
        })
      ) return;
      if (await staticFiles.serve(req, res, serveOptions)) return;

      res.writeHead(404, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "not_found", path: req.url }));
    } catch (err) {
      console.error("[net-bridge] request handler threw:", err);
      if (!res.headersSent) {
        res.writeHead(500, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: "internal" }));
      }
    }
  });

  const nativeWss = webBridge.createServer();
  const legacyWss = browserProxy.attach(http, { handleUpgrade: false });

  http.on("upgrade", (request, socket, head) => {
    let url: URL;
    try {
      url = new URL(request.url || "/", "http://localhost");
    } catch (_err) {
      socket.destroy();
      return;
    }

    if (relay.handlesUpgrade(url.pathname)) {
      relay.handleUpgrade(request, socket as Socket, head);
      return;
    }
    if (brainProxy.isBrainWsPath(url.pathname)) {
      brainProxy.proxyUpgrade(brain, request, socket, head); // the social socket (kn_brain)
      return;
    }
    if (url.pathname === webBridge.WEB_WS_PATH) {
      upgradeNative(nativeWss, request, socket as Socket, head);
      return;
    }
    const wss = url.pathname === browserProxy.WS_PATH ? legacyWss : null;
    if (!wss) {
      socket.destroy();
      return;
    }
    wss.handleUpgrade(request, socket, head, (ws) => {
      wss.emit("connection", ws, request);
    });
  });
  http.on("close", () => nativeWss.close());

  http.on("listening", () => {
    const where = describeAddress(http) || "(unknown address)";
    console.info(
      `net-bridge ${version} web listening on ${where} ` +
        `(static ${webDir}, ws ${webBridge.WEB_WS_PATH} -> tcp ${webBridge.NATIVE_HOST}:${webBridge.NATIVE_PORT}, ws ${browserProxy.WS_PATH}, ` +
        `brain routes /mm /api /social -> ${brain.url})`,
    );
  });

  http.listen(bridgeListenAddress());
  return http;
}

function startBrowserToNative(v10 = false): ReturnType<typeof createServer> {
  const http = createServer(async (req, res) => {
    try {
      // the v3.1.1 JS client plays through this bridge with no brain: its single-lobby matchmaker
      if (publicRoutes.handle(req, res, { getStats: browserProxy.getStats, legacyMatchmaker: true })) return;
      if (await staticFiles.serve(req, res)) return;

      res.writeHead(404, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "not_found", path: req.url }));
    } catch (err) {
      console.error("[net-bridge] request handler threw:", err);
      if (!res.headersSent) {
        res.writeHead(500, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: "internal" }));
      }
    }
  });

  if (v10) v10Proxy.attach(http);
  else browserProxy.attach(http);

  http.on("listening", () => {
    const where = describeAddress(http);
    console.info(
      `net-bridge ${version} ${v10 ? "v10-to-native" : "browser-to-native"} listening${where ? ` on ${where}` : ""} (ws ${v10 ? v10Proxy.WS_PATH : browserProxy.WS_PATH})`,
    );
  });

  http.listen(bridgeListenAddress());
  return http;
}

function startNativeToKrunker(options: CliOptions): ReturnType<typeof krunkerProxy.listen> {
  return krunkerProxy.listen({
    listenHost: options.listenHost,
    listenPort: options.listenPort,
    krunkerOrigin: options.krunkerOrigin,
    krunkerWsUrl: options.krunkerWsUrl,
    region: options.region,
    hostname: options.hostname,
    gameId: options.gameId,
  });
}

const options = parseArgs(process.argv.slice(2));
const server =
  options.mode === "web"
    ? startWeb(options)
    : options.mode === "bridge-only"
      ? startBridgeOnly()
      : options.mode === "browser-to-native"
      ? startBrowserToNative()
      : options.mode === "v10-to-native"
      ? startBrowserToNative(true)
      : startNativeToKrunker(options);
const activeSockets = new Set<Socket>();

server.on("connection", (socket: Socket) => {
  activeSockets.add(socket);
  socket.on("close", () => activeSockets.delete(socket));
});

server.on("error", (err) => {
  console.error("server error:", err);
  process.exit(1);
});

function closeBridgeSessions(force = false): void {
  if (options.mode === "web") {
    webBridge.closeAllSessions(force);
    browserProxy.closeAllSessions(force);
  } else if (options.mode === "bridge-only") {
    webBridge.closeAllSessions(force);
  } else if (options.mode === "browser-to-native") {
    browserProxy.closeAllSessions(force);
  } else if (options.mode === "v10-to-native") {
    v10Proxy.closeAllSessions(force);
  } else {
    krunkerProxy.closeAllSessions(force);
  }
}

function destroyActiveSockets(): void {
  for (const socket of Array.from(activeSockets)) socket.destroy();
}

function exitCodeFor(signal: NodeJS.Signals): number {
  return signal === "SIGINT" ? 130 : 0;
}

let shuttingDown = false;
let exiting = false;
let shutdownTimer: NodeJS.Timeout | null = null;

function finishExit(code: number): void {
  if (exiting) return;
  exiting = true;
  if (shutdownTimer) clearTimeout(shutdownTimer);
  process.exit(code);
}

function shutdown(signal: NodeJS.Signals): void {
  const code = exitCodeFor(signal);
  if (shuttingDown) {
    console.info(`${signal} received again, forcing exit`);
    closeBridgeSessions(true);
    destroyActiveSockets();
    finishExit(code);
    return;
  }

  shuttingDown = true;
  console.info(`${signal} received, exiting`);

  shutdownTimer = setTimeout(() => {
    console.warn("shutdown timed out, forcing exit");
    closeBridgeSessions(true);
    destroyActiveSockets();
    finishExit(code);
  }, 1500);
  shutdownTimer.unref?.();

  try {
    server.close(() => finishExit(code));
  } catch (err) {
    console.error("server close error:", err);
    finishExit(code);
    return;
  }

  // the bridge's players get their ERROR frame and close first (webBridge.ts closeAllSessions);
  // destroying their sockets at once would discard both
  closeBridgeSessions(false);
  setTimeout(destroyActiveSockets, webBridge.STOP_FRAME_WAIT_MS + 200).unref?.();
}

const signals: NodeJS.Signals[] = ["SIGTERM", "SIGINT", "SIGHUP"];
for (const signal of signals) {
  process.on(signal, () => shutdown(signal));
}