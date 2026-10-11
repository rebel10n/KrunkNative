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

function targetAllowed(host: string, port: number): boolean {
  for (const rule of TARGET_RULES) {
    if (rule.host === host && port >= rule.lo && port <= rule.hi) return true;
  }
  return false;
}

// The default target (no `?target=`) has to pass the allowlist too: a deployment that
// only allows remote game servers must not bridge to whatever listens on its own loopback.
function resolveTarget(req: IncomingMessage): { host: string; port: number } | null {
  let target: string | null = null;
  try {
    target = new URL(req.url || "/", "http://bridge").searchParams.get("target");
  } catch (_err) {
    return null;
  }
  if (!target) return targetAllowed(NATIVE_HOST.toLowerCase(), NATIVE_PORT) ? { host: NATIVE_HOST, port: NATIVE_PORT } : null;
  const colon = target.lastIndexOf(":");
  if (colon <= 0) return null;
  const host = target.slice(0, colon).replace(/^\[|\]$/g, "").toLowerCase();
  const portText = target.slice(colon + 1);
  if (!/^[0-9]{1,5}$/.test(portText)) return null;
  const port = Number.parseInt(portText, 10);
  if (port <= 0 || port > 65535) return null;
  return targetAllowed(host, port) ? { host, port } : null;
}

function envInt(name: string, fallback: number): number {
  const value = Number.parseInt(process.env[name] || "", 10);
  return Number.isFinite(value) && value >= 0 ? value : fallback;
}

// Abuse limits (0 = off; deploy/ansible role kn_bridge sets them):
//   KRUNK_NATIVE_MAX_PER_IP       concurrent sessions per client address
//   KRUNK_NATIVE_MAX_SESSIONS     concurrent sessions in total
//   KRUNK_NATIVE_IDLE_TIMEOUT_MS  close a session after this long without a byte either way
//   KRUNK_NATIVE_WS_PING_MS       WebSocket ping interval (keeps proxies such as Cloudflare,
//                                 100 s idle limit, from cutting the socket; a peer that did
//                                 not answer the previous ping is dropped)
//   KRUNK_NATIVE_CONNECT_TIMEOUT_MS  TCP connect timeout to the game server
//   KRUNK_NATIVE_CLIENT_IP_HEADER    header holding the client address when the TCP peer is
//                                 loopback (e.g. cf-connecting-ip behind cloudflared); default
//                                 the rightmost X-Forwarded-For (utils.ts peerAddress)
const MAX_PER_IP = envInt("KRUNK_NATIVE_MAX_PER_IP", 0);
const MAX_SESSIONS = envInt("KRUNK_NATIVE_MAX_SESSIONS", 0);
const IDLE_TIMEOUT_MS = envInt("KRUNK_NATIVE_IDLE_TIMEOUT_MS", 0);
const WS_PING_MS = envInt("KRUNK_NATIVE_WS_PING_MS", 30000);
const CONNECT_TIMEOUT_MS = envInt("KRUNK_NATIVE_CONNECT_TIMEOUT_MS", 10000);
const CLIENT_IP_HEADER = (process.env.KRUNK_NATIVE_CLIENT_IP_HEADER || "").trim().toLowerCase();
// One native frame is at most 3 + 65535 bytes; the browser never needs more per message.
const WS_MAX_PAYLOAD = 1 << 17;
// Browser bytes waiting for the TCP connect / for a slow game server.
const TCP_PENDING_MAX = 256 << 10;
const TCP_WRITE_MAX = 1 << 20;

function isLoopbackAddress(addr: string): boolean {
  return addr === "::1" || addr.startsWith("127.") || addr.startsWith("::ffff:127.");
}

function isLoopbackTarget(host: string): boolean {
  return host === "localhost" || isLoopbackAddress(host);
}

function clientAddress(req: IncomingMessage): string {
  if (CLIENT_IP_HEADER && isLoopbackAddress(req.socket.remoteAddress || "")) {
    const value = req.headers[CLIENT_IP_HEADER];
    const text = (Array.isArray(value) ? value[0] : value || "").trim();
    if (text) return text.slice(0, 64);
  }
  return peerAddress(req);
}

const perIp = new Map<string, number>();

/**
 * Upgrade-time admission for `/native` (index.ts): an HTTP status to refuse with, or
 * null. Refusing before the WebSocket handshake costs nothing and leaves no socket open.
 */
export function admit(req: IncomingMessage): { status: number; reason: string } | null {
  if (!resolveTarget(req)) return { status: 403, reason: "target not allowed" };
  if (MAX_SESSIONS && sessions.size >= MAX_SESSIONS) return { status: 503, reason: "bridge full" };
  if (MAX_PER_IP && (perIp.get(clientAddress(req)) || 0) >= MAX_PER_IP) {
    return { status: 429, reason: "too many connections from this address" };
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
  pendingBytes: number;
  remote: string;
  client: string;
  lastActivity: number;
  awaitingPong: boolean;
  timer: NodeJS.Timeout | null;
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

function releaseSession(session: PassthroughSession): boolean {
  if (!sessions.delete(session)) return false;
  if (session.timer) clearInterval(session.timer);
  session.timer = null;
  const left = (perIp.get(session.client) || 1) - 1;
  if (left > 0) perIp.set(session.client, left);
  else perIp.delete(session.client);
  return true;
}

function endSession(session: PassthroughSession, why: string): void {
  if (!releaseSession(session)) return;
  console.info(`[web-bridge] #${session.id} ${session.remote} closed (${why})`);
  session.pending.length = 0;
  session.tcp.destroy();
  closeWs(session.ws, 1000, why);
}

function handleConnection(ws: WebSocket, req: IncomingMessage): void {
  const client = clientAddress(req);
  const remote = `${client} (${req.socket.remoteAddress || "?"}:${req.socket.remotePort || "?"})`;
  const target = resolveTarget(req);
  if (!target) {
    console.warn(`[web-bridge] ${remote} refused: target not allowed (${req.url})`);
    closeWs(ws, 1008, "target not allowed");
    return;
  }
  // admit() ran at the upgrade; this catches sessions that raced past it.
  if ((MAX_SESSIONS && sessions.size >= MAX_SESSIONS) || (MAX_PER_IP && (perIp.get(client) || 0) >= MAX_PER_IP)) {
    console.warn(`[web-bridge] ${remote} refused: connection limit`);
    closeWs(ws, 1013, "connection limit");
    return;
  }
  const tcp = net.createConnection({ host: target.host, port: target.port });
  const session: PassthroughSession = {
    id: nextSessionId++,
    ws,
    tcp,
    tcpConnected: false,
    pending: [],
    pendingBytes: 0,
    remote,
    client,
    lastActivity: Date.now(),
    awaitingPong: false,
    timer: null,
  };
  sessions.add(session);
  perIp.set(client, (perIp.get(client) || 0) + 1);
  console.info(
    `[web-bridge] #${session.id} ${remote} -> tcp ${target.host}:${target.port}`,
  );

  tcp.setNoDelay(true);
  tcp.setKeepAlive(true, 30000);
  if (CONNECT_TIMEOUT_MS) tcp.setTimeout(CONNECT_TIMEOUT_MS);
  tcp.on("timeout", () => {
    if (!session.tcpConnected) endSession(session, "native tcp connect timeout");
  });

  if (WS_PING_MS || IDLE_TIMEOUT_MS) {
    const tick = Math.min(WS_PING_MS || IDLE_TIMEOUT_MS, IDLE_TIMEOUT_MS || WS_PING_MS);
    session.timer = setInterval(() => {
      if (IDLE_TIMEOUT_MS && Date.now() - session.lastActivity > IDLE_TIMEOUT_MS) {
        endSession(session, "idle timeout");
        return;
      }
      if (!WS_PING_MS || ws.readyState !== WebSocket.OPEN) return;
      if (session.awaitingPong) {
        console.warn(`[web-bridge] #${session.id} no pong, dropping`);
        if (releaseSession(session)) tcp.destroy();
        ws.terminate();
        return;
      }
      session.awaitingPong = true;
      ws.ping();
    }, tick);
    session.timer.unref?.();
  }
  ws.on("pong", () => {
    session.awaitingPong = false;
  });

  tcp.on("connect", () => {
    session.tcpConnected = true;
    tcp.setTimeout(0);
    // The browser's real address, before any browser byte. Only a game server on this
    // machine honours it (server/src/net.c net_handle_forwarded ignores remote peers).
    if (isLoopbackTarget(target.host)) tcp.write(forwardedFrame(clientAddress(req)));
    for (const chunk of session.pending) tcp.write(chunk);
    session.pending.length = 0;
    session.pendingBytes = 0;
  });

  tcp.on("data", (chunk: Buffer) => {
    session.lastActivity = Date.now();
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
    if (!releaseSession(session)) return;
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
    session.lastActivity = Date.now();
    if (!session.tcpConnected) {
      session.pendingBytes += bytes.length;
      if (session.pendingBytes > TCP_PENDING_MAX) {
        endSession(session, "too much data before connect");
        return;
      }
      session.pending.push(bytes);
      return;
    }
    if (tcp.destroyed) return;
    if (tcp.writableLength > TCP_WRITE_MAX) {
      endSession(session, "native tcp backlog");
      return;
    }
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
  const wss = new WebSocketServer({ noServer: true, maxPayload: WS_MAX_PAYLOAD, perMessageDeflate: false });
  wss.on("connection", handleConnection);
  console.info(
    `[web-bridge] passthrough ${WEB_WS_PATH} -> native ${NATIVE_HOST}:${NATIVE_PORT}; ` +
      `targets ${TARGET_RULES.map((r) => `${r.host}:${r.lo === r.hi ? r.lo : `${r.lo}-${r.hi}`}`).join(",") || "(none)"}; ` +
      `limits per-ip ${MAX_PER_IP || "off"}, total ${MAX_SESSIONS || "off"}, idle ${IDLE_TIMEOUT_MS || "off"} ms, ping ${WS_PING_MS || "off"} ms`,
  );
  return wss;
}

export function closeAllSessions(force = false): void {
  for (const session of Array.from(sessions)) {
    if (force) {
      releaseSession(session);
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
