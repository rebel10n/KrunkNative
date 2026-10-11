"use strict";

/**
 * Raw WebSocket <-> TCP passthrough for the Emscripten/WebGL2 client.
 *
 * The web build speaks the same framed protocol as the native client
 * (1 byte type, 2 bytes big-endian length, payload; see shared/src/net.c).
 * Browsers cannot open TCP sockets, so the page connects to
 * `ws(s)://<page host>/native` and this module pipes the bytes of every binary
 * WebSocket frame straight into the native server's TCP socket, and every TCP
 * `data` chunk back as a binary frame. No translation happens here; framing is
 * the client's and server's job.
 *
 * This is separate from proxy.ts, which translates the Krunker v3.1.1 browser
 * protocol on `/ws`.
 */

import net from "net";
import type { IncomingMessage } from "http";
import { WebSocket, WebSocketServer, type RawData } from "ws";
import { forwardedFrame, peerAddress } from "./utils";

export const WEB_WS_PATH = process.env.KRUNK_NATIVE_WEB_WS_PATH || "/native";
export const NATIVE_HOST = process.env.KRUNK_NATIVE_HOST || "127.0.0.1";
export const NATIVE_PORT =
  Number.parseInt(process.env.KRUNK_NATIVE_PORT || "21015", 10) || 21015;

/**
 * Brain matchmaking (KrunkNative backend): the page asks for a specific game server
 * with `/native?target=host:port` (the native TCP endpoint the brain's /mm/seek-game
 * returned). Only allowlisted targets are bridged, so the bridge is not an open TCP
 * proxy: KRUNK_NATIVE_TARGETS is a comma list of `host:port`, `host:lo-hi` or
 * `host:*`; the default is NATIVE_HOST with ports NATIVE_PORT .. NATIVE_PORT+63 (the
 * local game servers `make backend-up` starts). No `target` = NATIVE_HOST:NATIVE_PORT.
 */
type TargetRule = { host: string; lo: number; hi: number };

function parseTargetRules(spec: string): TargetRule[] {
  const rules: TargetRule[] = [];
  for (const raw of spec.split(",")) {
    const item = raw.trim();
    const colon = item.lastIndexOf(":");
    if (colon <= 0) continue;
    const host = item.slice(0, colon).toLowerCase();
    const ports = item.slice(colon + 1);
    if (ports === "*") {
      rules.push({ host, lo: 1, hi: 65535 });
      continue;
    }
    const [a, b] = ports.split("-");
    const lo = Number.parseInt(a, 10);
    const hi = b === undefined ? lo : Number.parseInt(b, 10);
    if (Number.isFinite(lo) && Number.isFinite(hi) && lo > 0 && hi >= lo) rules.push({ host, lo, hi });
  }
  return rules;
}

const TARGET_RULES = parseTargetRules(
  process.env.KRUNK_NATIVE_TARGETS || `${NATIVE_HOST}:${NATIVE_PORT}-${NATIVE_PORT + 63}`,
);

function resolveTarget(req: IncomingMessage): { host: string; port: number } | null {
  let target: string | null = null;
  try {
    target = new URL(req.url || "/", "http://bridge").searchParams.get("target");
  } catch (_err) {
    target = null;
  }
  if (!target) return { host: NATIVE_HOST, port: NATIVE_PORT };
  const colon = target.lastIndexOf(":");
  if (colon <= 0) return null;
  const host = target.slice(0, colon).replace(/^\[|\]$/g, "").toLowerCase();
  const port = Number.parseInt(target.slice(colon + 1), 10);
  if (!Number.isFinite(port) || port <= 0 || port > 65535) return null;
  for (const rule of TARGET_RULES) {
    if (rule.host === host && port >= rule.lo && port <= rule.hi) return { host, port };
  }
  return null;
}

// Stop reading from TCP while the browser is this far behind, resume below it.
const WS_HIGH_WATER = 1 << 20;
const WS_LOW_WATER = 256 << 10;

export type WebBridgeStats = {
  sessions: number;
  nativeConnected: number;
};

type PassthroughSession = {
  id: number;
  ws: WebSocket;
  tcp: net.Socket;
  tcpConnected: boolean;
  pending: Buffer[];
  remote: string;
};

const sessions = new Set<PassthroughSession>();
let nextSessionId = 1;

function toBuffer(data: RawData): Buffer {
  if (Buffer.isBuffer(data)) return data;
  if (Array.isArray(data)) return Buffer.concat(data);
  return Buffer.from(data);
}

function closeWs(ws: WebSocket, code: number, reason: string): void {
  if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
    try {
      ws.close(code, reason);
    } catch (_err) {
      ws.terminate();
    }
  }
}

function endSession(session: PassthroughSession, why: string): void {
  if (!sessions.delete(session)) return;
  console.info(`[web-bridge] #${session.id} ${session.remote} closed (${why})`);
  session.pending.length = 0;
  session.tcp.destroy();
  closeWs(session.ws, 1000, why);
}

function handleConnection(ws: WebSocket, req: IncomingMessage): void {
  const remote = `${req.socket.remoteAddress || "?"}:${req.socket.remotePort || "?"}`;
  const target = resolveTarget(req);
  if (!target) {
    console.warn(`[web-bridge] ${remote} refused: target not allowed (${req.url})`);
    closeWs(ws, 1008, "target not allowed");
    return;
  }
  const tcp = net.createConnection({ host: target.host, port: target.port });
  const session: PassthroughSession = {
    id: nextSessionId++,
    ws,
    tcp,
    tcpConnected: false,
    pending: [],
    remote,
  };
  sessions.add(session);
  console.info(
    `[web-bridge] #${session.id} ${remote} -> tcp ${target.host}:${target.port}`,
  );

  tcp.setNoDelay(true);

  tcp.on("connect", () => {
    session.tcpConnected = true;
    tcp.write(forwardedFrame(peerAddress(req))); // before any browser byte: who this really is
    for (const chunk of session.pending) tcp.write(chunk);
    session.pending.length = 0;
  });

  tcp.on("data", (chunk: Buffer) => {
    if (ws.readyState !== WebSocket.OPEN) return;
    ws.send(chunk, { binary: true }, (err) => {
      if (err) {
        endSession(session, `ws send error: ${err.message}`);
        return;
      }
      if (tcp.isPaused() && ws.bufferedAmount < WS_LOW_WATER) tcp.resume();
    });
    if (ws.bufferedAmount > WS_HIGH_WATER && !tcp.isPaused()) tcp.pause();
  });

  tcp.on("error", (err: NodeJS.ErrnoException) => {
    console.error(`[web-bridge] #${session.id} tcp error: ${err.code || err.message}`);
    if (!sessions.has(session)) return;
    sessions.delete(session);
    session.pending.length = 0;
    tcp.destroy();
    closeWs(ws, 1011, `native tcp error: ${err.code || "error"}`);
  });

  tcp.on("close", () => endSession(session, "native tcp closed"));

  ws.on("message", (data: RawData, isBinary: boolean) => {
    // Text frames carry nothing in this protocol; drop them.
    if (!isBinary) return;
    const bytes = toBuffer(data);
    if (bytes.length === 0) return;
    if (!session.tcpConnected) {
      session.pending.push(bytes);
      return;
    }
    if (tcp.destroyed) return;
    tcp.write(bytes);
  });

  ws.on("close", (code: number, reason: Buffer) => {
    const text = reason && reason.length ? reason.toString() : "";
    endSession(session, `ws closed ${code}${text ? ` ${text}` : ""}`);
  });

  ws.on("error", (err: Error) => {
    console.error(`[web-bridge] #${session.id} ws error: ${err.message}`);
    endSession(session, "ws error");
  });
}

/**
 * Build a `noServer` WebSocketServer for the passthrough. The caller routes
 * HTTP upgrade requests to it (see index.ts), which lets `/native` and the
 * legacy `/ws` translator share one HTTP listener.
 */
export function createServer(): WebSocketServer {
  const wss = new WebSocketServer({ noServer: true });
  wss.on("connection", handleConnection);
  console.info(
    `[web-bridge] passthrough ${WEB_WS_PATH} -> native ${NATIVE_HOST}:${NATIVE_PORT}`,
  );
  return wss;
}

export function closeAllSessions(force = false): void {
  for (const session of Array.from(sessions)) {
    if (force) {
      sessions.delete(session);
      session.tcp.destroy();
      session.ws.terminate();
    } else {
      endSession(session, "bridge shutting down");
    }
  }
}

export function getStats(): WebBridgeStats {
  let nativeConnected = 0;
  for (const session of sessions) if (session.tcpConnected) nativeConnected++;
  return { sessions: sessions.size, nativeConnected };
}
