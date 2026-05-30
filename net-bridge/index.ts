"use strict";

import { createServer } from "http";
import type { AddressInfo, Socket } from "net";
import version from "./version.json";
import * as krunkerProxy from "./krunkerProxy";
import * as browserProxy from "./proxy";
import * as publicRoutes from "./publicRoutes";
import * as staticFiles from "./staticFiles";

type BridgeMode = "native-to-krunker" | "browser-to-native";

type CliOptions = {
  mode: BridgeMode;
  listenHost?: string;
  listenPort?: number;
  krunkerOrigin?: string;
  krunkerWsUrl?: string;
  region?: string;
  hostname?: string;
  gameId?: string;
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
    "  --krunker-origin URL      Matchmaker origin for native-to-krunker mode (default http://127.0.0.1:6767)",
    "  --krunker-ws URL          Skip matchmaker and connect directly to this ws://.../ws URL",
    "  --listen-host HOST        Native TCP listen host in native-to-krunker mode",
    "  --listen-port PORT        Native TCP listen port in native-to-krunker mode (default 21015)",
    "  --region REGION           Matchmaker region (default local)",
    "  --hostname HOSTNAME       Matchmaker hostname query (default 127.0.0.1)",
    "  --game GAME_ID            Request a specific v3.1.1 lobby id",
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
    if (arg.startsWith("--mode=")) {
      const mode = arg.slice("--mode=".length);
      if (mode === "native-to-krunker" || mode === "browser-to-native") options.mode = mode;
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

function startBrowserToNative(): ReturnType<typeof createServer> {
  const http = createServer(async (req, res) => {
    try {
      if (publicRoutes.handle(req, res, { getStats: browserProxy.getStats })) return;
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

  browserProxy.attach(http);

  http.on("listening", () => {
    const address = http.address() as AddressInfo | string | null;
    if (!address || typeof address === "string") {
      console.info(`net-bridge ${version} browser-to-native listening (ws /ws)`);
      return;
    }
    console.info(
      `net-bridge ${version} browser-to-native listening on http://${address.address}:${address.port} (ws /ws)`,
    );
  });

  const host = process.env.KRUNK_NATIVE_BRIDGE_HOST || process.env.MM_HOST || "127.0.0.1";
  const port =
    Number.parseInt(process.env.KRUNK_NATIVE_BRIDGE_PORT || process.env.MM_PORT || "8080", 10) ||
    8080;

  http.listen({ host, port });
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
const server = options.mode === "browser-to-native" ? startBrowserToNative() : startNativeToKrunker(options);
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
  if (options.mode === "browser-to-native") browserProxy.closeAllSessions(force);
  else krunkerProxy.closeAllSessions(force);
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

  closeBridgeSessions(false);
  destroyActiveSockets();
}

const signals: NodeJS.Signals[] = ["SIGTERM", "SIGINT", "SIGHUP"];
for (const signal of signals) {
  process.on(signal, () => shutdown(signal));
}