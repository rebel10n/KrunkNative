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
//   native MAP_INFO (25)                               -> MAP_REQUEST (26); MAP_DATA (27) chunks are
//                                                         reassembled into the map JSON
//   native GAME_CONFIG (183), NETWORK_DATA (182)       -> init arguments 6 and 12
//   native INIT (2)                                    -> init (INIT_GAME), held until the map is whole;
//                                                         packets after it are queued meanwhile
//   native GLOBAL_VALUE (121), PLAYER_SOUND (125), TIMER (15) -> gv, asnd / rsnd, t
//   native MODE_STATE (40), OBJECT_STATE (100)         -> ts, lv, lock, gmsg; do, ro, gte, bnk, mo, mf
//   native MODE_STATE flags (in play)                  -> ufl (uid, x, y, z, carrier) and ufi (team lines)
//   native SKINS (35), SPAWN (4)                        -> kept, then ADD_PLAYERS "0" (+ start for us)
//   native LEADERBOARD (14), LEADER_STATS (240)        -> "7" rows (free for all); the repeat is consumed
//   native AI_ADD (90)                                  -> consumed: the v7 record (162) is the one for v7 peers
//   native AI_ADD_V7 (162)                              -> not translated yet: the session stops here
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

// Native packets that stop the translation, by name, counted for this process (the tests read it).
export const untranslatedSeen = new Map<string, number>();

// Native packets sent before the INIT that the v10 init does not take (billboards: a separate v10
// "bill" packet, not translated, ignored).
const SKIPPED_BEFORE_INIT: ReadonlySet<number> = new Set([T.NATIVE_MORE.BILLBOARD, T.NATIVE_MORE.BILLBOARD_IMAGE]);
const NATIVE_NAMES: Readonly<Record<number, string>> = Object.freeze({
  [T.NATIVE.HELLO]: "HELLO",
  [T.NATIVE.INIT]: "INIT",
  [T.NATIVE.ENT]: "ENT",
  [T.NATIVE_MORE.MAP_INFO]: "MAP_INFO",
  [T.NATIVE_MORE.MAP_REQUEST]: "MAP_REQUEST",
  [T.NATIVE_MORE.MAP_DATA]: "MAP_DATA",
  [T.NATIVE_MORE.NETWORK_DATA]: "NETWORK_DATA",
  [T.NATIVE_MORE.GAME_CONFIG]: "GAME_CONFIG",
  [T.NATIVE_MORE.GLOBAL_VALUE]: "GLOBAL_VALUE",
  [T.NATIVE_MORE.PLAYER_SOUND]: "PLAYER_SOUND",
  [T.NATIVE_MORE.TIMER]: "TIMER",
  [T.NATIVE_MORE.MODE_STATE]: "MODE_STATE",
  [T.NATIVE_MORE.OBJECT_STATE]: "OBJECT_STATE",
  [T.NATIVE_MORE.MODE_SYNC_V7]: "MODE_SYNC_V7",
  [T.NATIVE_MORE.LEADERBOARD]: "LEADERBOARD",
  [T.NATIVE_MORE.LEADER_STATS]: "LEADER_STATS",
  [T.NATIVE_MORE.AI_ADD_V7]: "AI_ADD_V7",
  [T.NATIVE_MORE.AI_SYNC_V7]: "AI_SYNC_V7",
});

type V10Session = {
  id: string;
  ws: WebSocket;
  request: IncomingMessage;
  tcp: net.Socket | null;
  connected: boolean;
  pending: Buffer[];
  readBuffer: Buffer;
  helloSent: boolean; // LOAD is answered with one HELLO per session
  mapInfo: T.NativeMapInfo | null; // the last MAP_INFO
  mapAssembly: T.ChunkAssembly | null; // MAP_DATA chunks in progress
  mapText: string | null; // the map JSON, whole
  configAssembly: T.ChunkAssembly | null; // GAME_CONFIG chunks in progress
  config: Record<string, unknown>; // init argument 6
  configText: string | null; // the GAME_CONFIG text the init carries: a repeat of it is no change
  networkReceived: boolean; // a NETWORK_DATA arrived for the init
  modeIndex: number; // the INIT's mode (0 Free for All)
  leaderRows: number[] | null; // the rows of the last LEADERBOARD as sent ("7"), for the LEADER_STATS that repeats them
  socketId: string; // io-init's id: our own player's id in the add record
  skinsByUid: Map<number, T.NativeSkins>; // SKINS, kept for the SPAWN that follows
  gameTimer: number; // the last TIMER seconds (the start message's gameTimer)
  challMode: number; // the join's challenge mode (ENTER_GAME argument 7)
  rankedReceived: boolean; // the ranked-match flag arrived for the init (sent after the INIT, like the config)
  ranked: boolean; // init argument 16, isRanked
  networkData: Record<string, number>; // init argument 12
  pendingInit: T.NativeInit | null; // the INIT, held until the map is whole
  queue: Array<{ type: number; payload: Buffer }>; // packets that arrive while the INIT is held
  initSent: boolean; // init went to the client: every later native packet not yet translated stops the session
  globalValuesSent: boolean; // a gv went to the client: a later reset of the set cannot be replayed
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

function nativeName(type: number, payload: Buffer): string {
  if (type === T.NATIVE_MORE.EXT && payload.length >= 2) {
    const sub = payload.readUInt16LE(0);
    return sub === T.EXT_GAME_STATE ? "EXT 0x0901 GAME_STATE" : `EXT 0x${sub.toString(16).padStart(4, "0")}`;
  }
  return NATIVE_NAMES[type] || `native ${type}`;
}

// The first native packet the translation cannot carry: nothing more goes to the client.
function stopAt(session: V10Session, name: string, reason: string): void {
  if (session.stopped) return;
  session.stopped = true;
  session.queue = [];
  untranslatedSeen.set(name, (untranslatedSeen.get(name) || 0) + 1);
  console.warn(`[v10-proxy] stop: native ${name} has no v10 translation yet (${reason})`);
}

// init (INIT_GAME): once the map JSON is whole. Then the packets held behind it run in order.
// The init carries the map, the config and the networking: the server sends the config and the networking
// after the INIT (its batched sender), so the init waits for all three.
function tryEmitInit(session: V10Session): void {
  if (!session.pendingInit || session.mapText === null || session.configText === null || !session.networkReceived || !session.rankedReceived) return;
  emitInit(session);
}

function emitInit(session: V10Session): void {
  const init = session.pendingInit;
  if (!init) return;
  session.pendingInit = null;
  sendWs(session, T.V10_IN.INIT_GAME, ...T.buildInitArgs(init, session.mapText, session.config, session.networkData, session.ranked));
  session.initSent = true;
  session.modeIndex = init.modeIndex;
  for (const held of session.queue.splice(0)) handleNativePacket(session, held.type, held.payload);
}

function handleNativePacket(session: V10Session, type: number, payload: Buffer): void {
  if (type === T.NATIVE.PING) {
    writeNative(session, T.NATIVE.PONG, payload);
    return;
  }
  if (session.stopped) return;
  // the init's own parts (the map, the config, the networking) are taken while the init is held
  const ranked = type === T.NATIVE_MORE.EXT && payload.length >= 3 && payload.readUInt16LE(0) === T.EXT_RANKED_MATCH;
  const initPart = type === T.NATIVE_MORE.MAP_DATA || type === T.NATIVE_MORE.GAME_CONFIG || type === T.NATIVE_MORE.NETWORK_DATA || ranked;
  if (session.pendingInit && !initPart) {
    session.queue.push({ type, payload });
    return;
  }
  if (session.initSent && type === T.NATIVE_MORE.GLOBAL_VALUE) {
    const value = T.parseGlobalValue(payload);
    if (!value) return stopAt(session, "GLOBAL_VALUE", "malformed");
    // A reset clears the client's set. The client holds nothing from this bridge before its first gv,
    // so a reset then is a no-op; after a gv, v10 has no message to clear the set: stop.
    if (value.reset && session.globalValuesSent) return stopAt(session, "GLOBAL_VALUE", "reset after values: v10 gv cannot clear");
    if (value.key.length === 0) return;
    session.globalValuesSent = true;
    sendWs(session, T.V10_IN.UPDATE_GLOBAL_VALUE, value.key, value.value);
    return;
  }
  if (session.initSent && (type === T.NATIVE_MORE.MODE_STATE || type === T.NATIVE_MORE.OBJECT_STATE)) {
    const mapped = type === T.NATIVE_MORE.MODE_STATE ? T.mapModeState(payload) : T.mapObjectState(payload);
    if (mapped.stop) return stopAt(session, nativeName(type, payload), mapped.stop);
    for (const [label, args] of mapped.messages) sendWs(session, label, ...args);
    return;
  }
  if (ranked) {
    const value = payload[2] !== 0;
    if (session.initSent && value !== session.ranked) return stopAt(session, "EXT 0x8050 RANKED", "the ranked flag changed after the init: v10 takes it in the init only");
    session.ranked = value;
    session.rankedReceived = true;
    tryEmitInit(session);
    return;
  }
  // the match phase (0 INITIALIZING .. 5 ENDED, the same values as v10's enum): gsc [gameState] (v10.js 218899)
  if (session.initSent && type === T.NATIVE_MORE.EXT && payload.length >= 3 && payload.readUInt16LE(0) === T.EXT_GAME_STATE) {
    sendWs(session, T.V10_IN.GAME_STATE_CHANGED, payload[2]);
    return;
  }
  if (session.initSent && type === T.NATIVE_MORE.EXT && payload.length >= 2 && payload.readUInt16LE(0) === T.EXT_GAME_ANNOUNCEMENT) {
    const mapped = T.mapGameAnnouncement(payload.subarray(2));
    if (mapped.stop) return stopAt(session, "EXT 0x0302 ANNOUNCEMENT", mapped.stop);
    for (const [label, args] of mapped.messages) sendWs(session, label, ...args);
    return;
  }
  if (session.initSent && type === T.NATIVE_MORE.PING_RESULT) return; // v10's "pir" handler does nothing
  if (session.initSent && type === T.NATIVE_MORE.SOCIAL_V7) return; // a v7 capability notice: the bridge sends chat as CHAT_SEND
  if (session.initSent && type === T.NATIVE_MORE.LEADERBOARD) {
    const mapped = T.mapLeaderboard(payload, session.modeIndex);
    if (mapped.stop) return stopAt(session, "LEADERBOARD", mapped.stop);
    for (const [label, args] of mapped.messages) sendWs(session, label, ...args);
    session.leaderRows = mapped.messages.length ? (mapped.messages[0][1] as [number[]])[0] : null;
    return;
  }
  if (session.initSent && type === T.NATIVE_MORE.LEADER_STATS) {
    const stop = T.checkLeaderStats(payload, session.leaderRows);
    if (stop) return stopAt(session, "LEADER_STATS", stop);
    return; // the rows it repeats were sent as "7"
  }
  // AI_ADD (90) is the v3 encoding of a bot. A protocol 1.3+ peer gets AI_ADD_V7 (162) as well, with the full
  // record (server/src/ai.c 2124-2131 sends both; game_ai_packet_for_peer keeps one), so the v3 one is consumed:
  // translating both would add every bot twice.
  if (session.initSent && type === T.NATIVE_MORE.AI_ADD) return;
  if (session.initSent && type === T.NATIVE_MORE.AI_SYNC) return; // the v3 sync: the v7 one (161) is the one for a v7 peer
  // SKINS come right before the SPAWN they belong to: kept for it, nothing sent
  if (session.initSent && type === T.NATIVE_MORE.SKINS) {
    const skins = T.parseSkins(payload);
    if (!skins) return stopAt(session, "SKINS", "malformed");
    session.skinsByUid.set(skins.uid, skins);
    return;
  }
  // SPAWN: a living player's add record (ADD_PLAYERS "0", one player) and, for our own spawn, the start
  if (session.initSent && type === T.NATIVE_MORE.SPAWN) {
    const spawn = T.parseSpawn(payload);
    if (!spawn) return stopAt(session, "SPAWN", "malformed");
    if (!spawn.active) return; // a player who is dead now: the v10 add is for players in play
    sendWs(session, T.V10_IN.ADD_PLAYERS, T.buildPlayerAdd(spawn, session.skinsByUid.get(spawn.uid), session.socketId), 1);
    if (spawn.isYou) sendWs(session, T.V10_IN.START_GAME, ...T.buildStartGame(spawn, session.gameTimer, session.challMode));
    return;
  }
  if (session.initSent && type === T.NATIVE_MORE.TIMER) {
    const timer = T.parseTimer(payload);
    if (!timer) return stopAt(session, "TIMER", "malformed");
    session.gameTimer = timer.seconds;
    sendWs(session, T.V10_IN.UPDATE_TIMER, timer.seconds, timer.end ? 1 : 0);
    return;
  }
  if (session.initSent && type === T.NATIVE_MORE.PLAYER_SOUND) {
    const sound = T.parsePlayerSound(payload);
    if (!sound) return stopAt(session, "PLAYER_SOUND", "malformed");
    // asnd: v10 addPlayerSound(sid, name, {loop, vol, rate}); rsnd: removePlayerSound(sid, name, force)
    // (v10.js 190131, 190145; the server's own broadcasts have these arguments).
    if (!sound.add) sendWs(session, T.V10_IN.REMOVE_PLAYER_SOUND, sound.uid, sound.name, sound.force);
    else sendWs(session, T.V10_IN.ADD_PLAYER_SOUND, sound.uid, sound.name, { loop: sound.loop, vol: sound.volume, rate: sound.rate });
    return;
  }
  // after the init, the server repeats the config for a peer whose HELLO came after its INIT
  // (server/src/net.c HELLO path): the same text is no change. v10 has no message for a changed config.
  if (session.initSent && type === T.NATIVE_MORE.GAME_CONFIG) {
    const chunk = T.parseGameConfigChunk(payload);
    if (!chunk || chunk.total === 0) return stopAt(session, "GAME_CONFIG", "malformed");
    if (chunk.offset === 0) session.configAssembly = new T.ChunkAssembly(chunk.total);
    if (!session.configAssembly || !session.configAssembly.add(chunk.offset, chunk.data)) {
      return stopAt(session, "GAME_CONFIG", "out of order");
    }
    if (!session.configAssembly.complete) return;
    const text = session.configAssembly.text();
    session.configAssembly = null;
    if (text === session.configText) return;
    return stopAt(session, "GAME_CONFIG", "the config changed after the init: v10 has no message for it");
  }
  if (session.initSent && type === T.NATIVE_MORE.NETWORK_DATA) {
    const network = T.parseNetworkData(payload);
    if (!network) return stopAt(session, "NETWORK_DATA", "malformed");
    const same = Object.keys(network).every((k) => session.networkData[k] === network[k]);
    if (same) return;
    return stopAt(session, "NETWORK_DATA", "the networking changed after the init: v10 has no message for it");
  }
  if (session.initSent) {
    stopAt(session, nativeName(type, payload), "after the init");
    return;
  }

  switch (type) {
    case T.NATIVE_MORE.MAP_INFO: {
      const info = T.parseMapInfo(payload);
      if (!info || info.size === 0) return stopAt(session, "MAP_INFO", "malformed or empty map");
      session.mapInfo = info;
      session.mapText = null;
      session.mapAssembly = new T.ChunkAssembly(info.size);
      // The bridge never has the map: it asks by hash, as the native client does (client/src/net/game_events.c).
      writeNative(session, T.NATIVE_MORE.MAP_REQUEST, T.encodeMapRequest(info.hash));
      return;
    }
    case T.NATIVE_MORE.MAP_DATA: {
      const chunk = T.parseMapData(payload);
      if (!chunk || !session.mapInfo || !session.mapAssembly || chunk.hash !== session.mapInfo.hash) {
        return stopAt(session, "MAP_DATA", "not the map asked for");
      }
      if (!session.mapAssembly.add(chunk.offset, chunk.data)) return stopAt(session, "MAP_DATA", "out of order");
      if (!session.mapAssembly.complete) return;
      const text = session.mapAssembly.text();
      session.mapAssembly = null;
      try {
        JSON.parse(text);
      } catch (_err) {
        return stopAt(session, "MAP_DATA", "the map is not JSON");
      }
      session.mapText = text;
      tryEmitInit(session);
      return;
    }
    case T.NATIVE_MORE.GAME_CONFIG: {
      const chunk = T.parseGameConfigChunk(payload);
      if (!chunk || chunk.total === 0) return stopAt(session, "GAME_CONFIG", "malformed");
      if (chunk.offset === 0) session.configAssembly = new T.ChunkAssembly(chunk.total);
      if (!session.configAssembly || !session.configAssembly.add(chunk.offset, chunk.data)) {
        return stopAt(session, "GAME_CONFIG", "out of order");
      }
      if (!session.configAssembly.complete) return;
      const text = session.configAssembly.text();
      session.configAssembly = null;
      try {
        session.config = T.buildConfig(text);
      } catch (_err) {
        return stopAt(session, "GAME_CONFIG", "the config is not JSON");
      }
      session.configText = text;
      tryEmitInit(session);
      return;
    }
    case T.NATIVE_MORE.NETWORK_DATA: {
      const network = T.parseNetworkData(payload);
      if (!network) return stopAt(session, "NETWORK_DATA", "malformed");
      session.networkData = network;
      session.networkReceived = true;
      tryEmitInit(session);
      return;
    }
    case T.NATIVE.INIT: {
      const init = T.parseInit(payload);
      if (!init) return stopAt(session, "INIT", "malformed");
      if (!session.mapInfo) return stopAt(session, "INIT", "no MAP_INFO before it");
      if (session.mapText === null && session.mapAssembly === null) return stopAt(session, "INIT", "the map was not requested");
      session.pendingInit = init;
      tryEmitInit(session);
      return;
    }
    default:
      if (SKIPPED_BEFORE_INIT.has(type)) return;
      stopAt(session, nativeName(type, payload), "before the init");
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
      const join = args[0];
      session.challMode = Array.isArray(join) && join[T.EN.CHALLENGE] ? 1 : 0;
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
    mapInfo: null,
    mapAssembly: null,
    mapText: null,
    configAssembly: null,
    config: {},
    configText: null,
    networkReceived: false,
    modeIndex: 0,
    leaderRows: null,
    socketId: makeSocketId(12),
    skinsByUid: new Map(),
    gameTimer: 0,
    challMode: 0,
    rankedReceived: false,
    ranked: false,
    networkData: {},
    pendingInit: null,
    queue: [],
    initSent: false,
    globalValuesSent: false,
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
  const socketId = session.socketId;
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
