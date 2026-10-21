"use strict";

// The `--v10-to-native` mode: the official Krunker v10.0.x client (msgpack over WebSocket, v10 labels)
// <-> a KrunkNative game server (knet packets over TCP, the same bytes as the server's raw WebSocket
// framing). The packet mapping is in v10Translate.ts; this file owns the sockets and the order.
//
// Connect sequence (v10.js _73891 joinServer, 222104):
//   client opens ws://<host>/ws?gameId=..&clientKey=..  -> bridge sends io-init, load (INIT_LOAD) and
//   inst-id, the three the v10 client needs before it answers. The bridge opens TCP to the native
//   server and writes the forwarded-address frame first (NET_PACKET_FORWARDED, shared.h).
//   client sends load (LOAD)                           -> HELLO (1)
//   native INIT (2)                                    -> not translated yet: the session stops here
//   client sends en (ENTER_GAME)                       -> ENT (3)
//   client sends ct (CHAT_MESSAGE)                     -> CHAT_SEND (8)
//   native PING (17)                                   -> PONG (18), answered here

import net from "net";
import type { IncomingMessage, Server as HttpServer } from "http";
import { WebSocket, WebSocketServer, type RawData } from "ws";
import * as UTILS from "./utils";
import * as T from "./v10Translate";

export const WS_PATH = "/ws";
const LOBBY_ID = process.env.KRUNK_NATIVE_LOBBY_ID || "KN:local";
const INIT_LOAD_ARG = 20000; // the first INIT_LOAD argument, as proxy.ts sends it
const NET_HEADER_SIZE = 3;
const SOCKET_ID_CHARS = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

// Native packets that stop the translation, with the v10 handler they would feed (the report's table).
export const UNTRANSLATED_NATIVE: Readonly<Record<number, string>> = Object.freeze({
  [T.NATIVE.INIT]: "INIT (2) -> init (INIT_GAME): needs the map (REST /maps/<id> or the map object in arg 9) and the 35-argument list",
});

// Counts of the first stop, per native packet type, for this process (the tests read it).
export const untranslatedSeen = new Map<number, number>();

type V10Session = {
  id: string;
  ws: WebSocket;
  request: IncomingMessage;
  tcp: net.Socket | null;
  connected: boolean;
  pending: Buffer[];
  readBuffer: Buffer;
  helloSent: boolean; // LOAD is answered with one HELLO per session
  stopped: boolean; // true once a native packet had no translation: nothing more goes to the client
  closed: boolean;
  nativeHost: string;
  nativePort: number;
};

const sessions = new Set<V10Session>();

function makeSocketId(length = 12): string {
  let out = "";
  for (let i = 0; i < length; i++) out += SOCKET_ID_CHARS[(Math.random() * SOCKET_ID_CHARS.length) | 0];
  return out;
}

function isWsOpen(ws: WebSocket): boolean {
  return ws.readyState === WebSocket.OPEN;
}

// Server -> client frames. The v10 client ignores the trailer on incoming packets (v10.js 98121 reads
// only label and args), so 0 is sent.
function sendWs(session: V10Session, label: string, ...args: unknown[]): void {
  if (session.closed || !isWsOpen(session.ws)) return;
  session.ws.send(UTILS.encodeNetworkMessage(UTILS.serializeNetworkMessage(label, ...args), 0));
}

function closeSession(session: V10Session, code = 1011, reason = "bridge closed", force = false): void {
  if (session.closed) return;
  session.closed = true;
  sessions.delete(session);
  if (force) session.ws.terminate();
  else if (isWsOpen(session.ws)) session.ws.close(code, reason);
  if (session.tcp) {
    session.tcp.destroy();
    session.tcp = null;
  }
}

function writeNative(session: V10Session, type: number, payload: Buffer = Buffer.alloc(0)): void {
  if (payload.length > 512) throw new Error(`native payload too large for packet ${type}`);
  const frame = Buffer.alloc(NET_HEADER_SIZE + payload.length);
  frame[0] = type & 0xff;
  frame.writeUInt16BE(payload.length, 1);
  payload.copy(frame, NET_HEADER_SIZE);
  if (!session.connected || !session.tcp) session.pending.push(frame);
  else session.tcp.write(frame);
}

function connectNative(session: V10Session): void {
  const tcp = net.createConnection({ host: session.nativeHost, port: session.nativePort }, () => {
    tcp.setNoDelay(true);
    session.connected = true;
    // The forwarded address comes first: the server accepts it only from the bridge's loopback peer.
    tcp.write(UTILS.forwardedFrame(UTILS.peerAddress(session.request)));
    for (const frame of session.pending.splice(0)) tcp.write(frame);
  });
  session.tcp = tcp;

  tcp.on("data", (chunk: Buffer) => {
    try {
      handleNativeData(session, chunk);
    } catch (err) {
      console.error("[v10-proxy] native decode error:", err);
      closeSession(session, 1011, "Native server protocol error");
    }
  });
  tcp.on("close", () => closeSession(session, 1011, "native server closed"));
  tcp.on("error", (err: Error) => {
    console.error("[v10-proxy] native socket error:", err.message);
    closeSession(session, 1011, "native server unavailable");
  });
}

function handleNativeData(session: V10Session, chunk: Buffer): void {
  session.readBuffer = Buffer.concat([session.readBuffer, chunk]);
  while (session.readBuffer.length >= NET_HEADER_SIZE) {
    const size = session.readBuffer.readUInt16BE(1);
    if (session.readBuffer.length < NET_HEADER_SIZE + size) return;
    const type = session.readBuffer[0];
    const payload = Buffer.from(session.readBuffer.subarray(NET_HEADER_SIZE, NET_HEADER_SIZE + size));
    session.readBuffer = session.readBuffer.subarray(NET_HEADER_SIZE + size);
    handleNativePacket(session, type, payload);
  }
}

function handleNativePacket(session: V10Session, type: number, payload: Buffer): void {
  if (type === T.NATIVE.PING) {
    writeNative(session, T.NATIVE.PONG, payload);
    return;
  }
  if (session.stopped) return;
  if (UNTRANSLATED_NATIVE[type]) {
    session.stopped = true;
    untranslatedSeen.set(type, (untranslatedSeen.get(type) || 0) + 1);
    console.warn(`[v10-proxy] stop: native packet ${type} has no v10 translation (${UNTRANSLATED_NATIVE[type]})`);
  }
}

function handleClientPacket(session: V10Session, label: string, args: unknown[]): void {
  switch (label) {
    case T.V10_OUT.LOAD:
      // The client is ready: the native server answers with INIT once it has HELLO.
      if (session.helloSent) break;
      session.helloSent = true;
      writeNative(session, T.NATIVE.HELLO, T.HELLO_PAYLOAD);
      break;
    case T.V10_OUT.ENTER_GAME: {
      const ent = T.encodeEnter(args[0]);
      if (ent) writeNative(session, T.NATIVE.ENT, ent);
      break;
    }
    case T.V10_OUT.CHAT_MESSAGE: {
      const chat = T.encodeChat(args[0]);
      if (chat) writeNative(session, T.NATIVE.CHAT_SEND, chat);
      break;
    }
    default:
      // Not translated yet (movement input, pings, the rest of the v10 labels): dropped, not forwarded.
      break;
  }
}

function createSession(ws: WebSocket, request: IncomingMessage, options: AttachOptions): V10Session {
  return {
    id: makeSocketId(5),
    ws,
    request,
    tcp: null,
    connected: false,
    pending: [],
    readBuffer: Buffer.alloc(0),
    helloSent: false,
    stopped: false,
    closed: false,
    nativeHost: options.nativeHost || "127.0.0.1",
    nativePort: options.nativePort || 21015,
  };
}

function handleConnection(ws: WebSocket, request: IncomingMessage, options: AttachOptions): void {
  const session = createSession(ws, request, options);
  sessions.add(session);

  // What the v10 client needs before it answers: the socket id, the load request and the instance id.
  const socketId = makeSocketId(12);
  sendWs(session, T.V10_IN.IO_INIT, socketId);
  sendWs(session, T.V10_IN.INIT_LOAD, INIT_LOAD_ARG, socketId);
  sendWs(session, T.V10_IN.GAME_INSTANCE_ID, LOBBY_ID);
  connectNative(session);

  ws.on("message", (message: RawData) => {
    let decoded: UTILS.DecodedPacket;
    try {
      decoded = UTILS.decodePacket(message);
    } catch (err) {
      console.error("[v10-proxy] client msgpack decode error:", err);
      closeSession(session, 1002, "Invalid client packet");
      return;
    }
    const [, label, args] = decoded;
    handleClientPacket(session, label, args);
  });
  ws.on("close", () => closeSession(session, 1000, "client closed"));
  ws.on("error", (err: Error) => {
    console.error("[v10-proxy] websocket error:", err.message);
    closeSession(session, 1011, "websocket error");
  });
}

type AttachOptions = {
  /** Native server (default KRUNK_NATIVE_HOST:KRUNK_NATIVE_PORT, 127.0.0.1:21015). */
  nativeHost?: string;
  nativePort?: number;
  /** false: the caller routes `upgrade` itself (see proxy.ts). */
  handleUpgrade?: boolean;
};

export function attach(http: HttpServer, opts: AttachOptions = {}): WebSocketServer {
  const options: AttachOptions = {
    nativeHost: opts.nativeHost || process.env.KRUNK_NATIVE_HOST || "127.0.0.1",
    nativePort: opts.nativePort || Number.parseInt(process.env.KRUNK_NATIVE_PORT || "21015", 10) || 21015,
  };
  const wss = new WebSocketServer({ noServer: true });
  wss.on("connection", (ws: WebSocket, request: IncomingMessage) => handleConnection(ws, request, options));

  if (opts.handleUpgrade !== false) {
    http.on("upgrade", (request, socket, head) => {
      let url: URL;
      try {
        url = new URL(request.url || "/", "http://localhost");
      } catch (_err) {
        socket.destroy();
        return;
      }
      if (url.pathname !== WS_PATH) {
        socket.destroy();
        return;
      }
      wss.handleUpgrade(request, socket, head, (ws) => wss.emit("connection", ws, request));
    });
  }
  http.on("close", () => wss.close());
  console.info(`[v10-proxy] v10 client ${WS_PATH} -> native ${options.nativeHost}:${options.nativePort}`);
  return wss;
}

export function closeAllSessions(force = false): void {
  for (const session of Array.from(sessions)) closeSession(session, 1001, "bridge stopping", force);
}
