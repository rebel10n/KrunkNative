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

import fs from "fs";
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

/**
 * Local/dev bridges also take any loopback target (`localhost`, 127.x, ::1; any port), so a
 * developer's own server_bin is reachable from the web console `connect localhost:<port>` and
 * the Direct Connect field: `make bridge` (no KRUNK_NATIVE_TARGETS) or
 * KRUNK_NATIVE_ALLOW_LOOPBACK=1 (deploy/local/backend.sh). A deployed bridge sets
 * KRUNK_NATIVE_TARGETS and is never an open proxy to its host's loopback ports.
 */
const ALLOW_LOOPBACK = process.env.KRUNK_NATIVE_ALLOW_LOOPBACK !== undefined
  ? /^(1|true|yes|on)$/i.test(process.env.KRUNK_NATIVE_ALLOW_LOOPBACK)
  : !process.env.KRUNK_NATIVE_TARGETS;

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
  if (targetAllowed(host, port)) return { host, port };
  // "localhost" is the IPv4 loopback the game servers listen on (they bind 0.0.0.0 only)
  if (host === "localhost" && targetAllowed("127.0.0.1", port)) return { host: "127.0.0.1", port };
  if (ALLOW_LOOPBACK && isLoopbackTarget(host)) return { host: host === "localhost" ? "127.0.0.1" : host, port };
  return null;
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
 * Upgrade-time admission for `/native` (index.ts): why to refuse, or null. `status` is the
 * HTTP answer for a request that is not a WebSocket upgrade; a real upgrade is accepted and
 * closed at once with `close` and the reason, because a browser hides an HTTP refusal of the
 * handshake (the page only sees close 1006 with no reason) and the client could neither show
 * why nor tell a final refusal (1008: the target is not allowed, retrying cannot help) from a
 * full bridge (1013: try again later). No bridged TCP connection is opened either way.
 */
export type Refusal = { status: number; close: number; reason: string };
export const CLOSE_POLICY = 1008;
export const CLOSE_TRY_AGAIN_LATER = 1013;

export function admit(req: IncomingMessage): Refusal | null {
  if (!resolveTarget(req)) return { status: 403, close: CLOSE_POLICY, reason: "target not allowed" };
  if (MAX_SESSIONS && sessions.size >= MAX_SESSIONS) return { status: 503, close: CLOSE_TRY_AGAIN_LATER, reason: "bridge full" };
  if (MAX_PER_IP && (perIp.get(clientAddress(req)) || 0) >= MAX_PER_IP) {
    return { status: 429, close: CLOSE_TRY_AGAIN_LATER, reason: "too many connections from this address" };
  }
  return null;
}

// Stopping (SIGTERM): every player gets a game-level ERROR first, like a game server's own stop
// (server/src/main.c server_send_shutdown_message): one native frame NET_PACKET_ERROR (30) with a
// NetErrorPacket {message[96], code[8]} (shared/include/shared.h), injected at a frame boundary of
// the server's stream, then the WebSocket closes with 1012 (service restart). The client shows it
// and rejoins with a fresh ticket (client/src/ui/servers_ui.c, 004004 / 004006). The text comes
// from the first line of KRUNK_NATIVE_STOP_REASON_FILE (a deploy writes "update" there around the
// restart; ignored when older than 15 min, like server_bin's shutdown_reason_file).
const STOP_REASON_FILE = process.env.KRUNK_NATIVE_STOP_REASON_FILE || "";
const STOP_REASON_MAX_AGE_MS = 15 * 60 * 1000;
const NET_PACKET_ERROR = 30;
const ERROR_MESSAGE_SIZE = 96;
const ERROR_CODE_SIZE = 8;
// A frame still arriving when the stop comes gets this long to complete before the plain close.
export const STOP_FRAME_WAIT_MS = 300;

export function stopMessage(file: string = STOP_REASON_FILE): { message: string; code: string } {
  let reason = "";
  try {
    if (file && Date.now() - fs.statSync(file).mtimeMs <= STOP_REASON_MAX_AGE_MS) {
      reason = fs.readFileSync(file, "utf8").split("\n")[0].trim().toLowerCase();
    }
  } catch (_err) {
    reason = "";
  }
  return reason === "update" ? { message: "GAME UPDATING", code: "004004" } : { message: "Server restarting", code: "004006" };
}

// [30][len16 BE][NetErrorPacket]: both strings NUL-terminated inside their fixed fields.
export function errorFrame(message: string, code: string): Buffer {
  const frame = Buffer.alloc(3 + ERROR_MESSAGE_SIZE + ERROR_CODE_SIZE);
  frame[0] = NET_PACKET_ERROR;
  frame.writeUInt16BE(ERROR_MESSAGE_SIZE + ERROR_CODE_SIZE, 1);
  Buffer.from(message, "utf8").subarray(0, ERROR_MESSAGE_SIZE - 1).copy(frame, 3);
  Buffer.from(code, "utf8").subarray(0, ERROR_CODE_SIZE - 1).copy(frame, 3 + ERROR_MESSAGE_SIZE);
  return frame;
}

// Where the server -> browser byte stream is in its native frames (1 byte type, 2 bytes length,
// payload), so a frame of our own never lands inside one of the server's.
export type FrameCursor = { left: number; header: number[] };

// Advance over chunk. until: return the offset of the first frame boundary at or after the start
// of chunk (-1 when chunk ends inside a frame); otherwise chunk.length.
export function scanFrames(cursor: FrameCursor, chunk: Buffer, until: boolean): number {
  let i = 0;
  for (;;) {
    if (until && cursor.left === 0 && cursor.header.length === 0) return i;
    if (i >= chunk.length) return until ? -1 : i;
    if (cursor.left > 0) {
      const n = Math.min(cursor.left, chunk.length - i);
      cursor.left -= n;
      i += n;
      continue;
    }
    cursor.header.push(chunk[i++]);
    if (cursor.header.length === 3) {
      cursor.left = (cursor.header[1] << 8) | cursor.header[2];
      cursor.header.length = 0;
    }
  }
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
  frames: FrameCursor;
  stop: { frame: Buffer; message: string } | null; // stopping: the ERROR frame waits for a frame boundary
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
    frames: { left: 0, header: [] },
    stop: null,
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
    if (session.stop) {
      // stopping: finish the server's frame in progress, then our ERROR and the close
      const end = scanFrames(session.frames, chunk, true);
      if (end !== 0) ws.send(end < 0 ? chunk : chunk.subarray(0, end), { binary: true });
      if (end >= 0) finishStop(session);
      return;
    }
    scanFrames(session.frames, chunk, false);
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
      `targets ${TARGET_RULES.map((r) => `${r.host}:${r.lo === r.hi ? r.lo : `${r.lo}-${r.hi}`}`).join(",") || "(none)"}` +
      `${ALLOW_LOOPBACK ? " + any loopback (dev)" : ""}; ` +
      `limits per-ip ${MAX_PER_IP || "off"}, total ${MAX_SESSIONS || "off"}, idle ${IDLE_TIMEOUT_MS || "off"} ms, ping ${WS_PING_MS || "off"} ms`,
  );
  return wss;
}

function finishStop(session: PassthroughSession): void {
  const stop = session.stop;
  if (!stop || !releaseSession(session)) return;
  console.info(`[web-bridge] #${session.id} ${session.remote} closed (bridge stopping, told "${stop.message}")`);
  session.pending.length = 0;
  session.tcp.destroy();
  if (session.ws.readyState === WebSocket.OPEN) session.ws.send(stop.frame, { binary: true });
  closeWs(session.ws, 1012, stop.message);
}

// Graceful stop of one session: the ERROR frame now if the server's stream is at a frame boundary,
// else right after the frame in progress (STOP_FRAME_WAIT_MS at most, then a plain close).
function stopSession(session: PassthroughSession, message: string, code: string): void {
  if (!sessions.has(session) || session.stop) return;
  session.stop = { frame: errorFrame(message, code), message };
  if (session.ws.readyState !== WebSocket.OPEN || !session.tcpConnected) {
    endSession(session, "bridge stopping");
    return;
  }
  if (session.frames.left === 0 && session.frames.header.length === 0) {
    finishStop(session);
    return;
  }
  setTimeout(() => endSession(session, "bridge stopping (mid-frame)"), STOP_FRAME_WAIT_MS).unref?.();
}

export function closeAllSessions(force = false): void {
  const { message, code } = stopMessage();
  if (!force && sessions.size) console.info(`[web-bridge] stopping: telling ${sessions.size} session(s) "${message}" (${code})`);
  for (const session of Array.from(sessions)) {
    if (force) {
      releaseSession(session);
      session.tcp.destroy();
      session.ws.terminate();
    } else {
      stopSession(session, message, code);
    }
  }
}

export function getStats(): WebBridgeStats {
  let nativeConnected = 0;
  for (const session of sessions) if (session.tcpConnected) nativeConnected++;
  return { sessions: sessions.size, nativeConnected };
}
