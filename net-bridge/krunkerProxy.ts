"use strict";

import http from "http";
import https from "https";
import net from "net";
import { WebSocket, type RawData } from "ws";
import antihackKey from "./antihackkey.json";
import version from "./version.json";
import * as UTILS from "./utils";

const DEFAULT_NATIVE_PORT = 21015;
const DEFAULT_KRUNKER_ORIGIN = "http://127.0.0.1:6767";
const DEFAULT_REGION = "local";
const DEFAULT_HOSTNAME = "127.0.0.1";
const DEFAULT_TICK_RATE = 30;
const NET_MAX_PAYLOAD = 512;
const NET_HEADER_SIZE = 3;
const ANTIHACK_KEY = Number(antihackKey) || 0;

const NET_PACKET = Object.freeze({
  HELLO: 1,
  INIT: 2,
  ENT: 3,
  SPAWN: 4,
  INPUT: 5,
  STATE: 6,
  CYCLE: 7,
} as const);

type NetPacketType = (typeof NET_PACKET)[keyof typeof NET_PACKET];

const NET_INPUT = Object.freeze({
  SHOOT: 1 << 0,
  SCOPE: 1 << 1,
  JUMP: 1 << 2,
  CROUCH: 1 << 3,
  RELOAD: 1 << 4,
} as const);

type Vec2 = [number, number];
type Vec3 = [number, number, number];

type MatchmakerReply = {
  gameId?: string;
  lobbyId?: string;
  host?: string;
  port?: number;
  clientKey?: string;
  pgi?: string;
  bpt?: string;
};

type NativeInputPacket = {
  seq: number;
  moveDir: number;
  delta: number;
  pitch: number;
  yaw: number;
  flags: number;
  swap: number;
};

type NativeSpawn = {
  uid: number;
  isYou: boolean;
  active: boolean;
  classIndex: number;
  team: number;
  health: number;
  maxHealth: number;
  position: Vec3;
  direction: Vec2;
  name: string;
};

type NativeState = {
  uid: number;
  ackSeq: number;
  active: boolean;
  loadoutIndex: number;
  position: Vec3;
  velocity: Vec3;
  direction: Vec2;
  health: number;
  aimVal: number;
  crouchVal: number;
};

export type NativeToKrunkerOptions = {
  listenHost?: string;
  listenPort?: number;
  krunkerOrigin?: string;
  krunkerWsUrl?: string | null;
  region?: string;
  hostname?: string;
  gameId?: string | null;
  dataQuery?: Record<string, unknown> | null;
};

type NativeToKrunkerSession = {
  id: number;
  tcp: net.Socket;
  readBuffer: Buffer;
  upstream: WebSocket | null;
  upstreamReady: boolean;
  upstreamConnecting: boolean;
  pendingUpstreamPackets: Array<[string, unknown[]]>;
  upstreamAhNum: number;
  socketId: string | null;
  ownSid: number | null;
  tickRate: number;
  mapIndex: number;
  modeIndex: number;
  healthByUid: Map<number, number>;
  maxHealthByUid: Map<number, number>;
  classByUid: Map<number, number>;
  teamByUid: Map<number, number>;
  nameByUid: Map<number, string>;
  lastStateByUid: Map<number, NativeState>;
  closed: boolean;
};

export type NativeToKrunkerStats = {
  sessions: number;
  upstreamConnected: number;
};

const sessions = new Set<NativeToKrunkerSession>();
let nextSessionId = 1;

function toInt(value: unknown, fallback = 0): number {
  const n = Number(value);
  return Number.isFinite(n) ? n | 0 : fallback;
}

function toNumber(value: unknown, fallback = 0): number {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function normalizeOrigin(raw: string): URL {
  const withScheme = /^https?:\/\//i.test(raw) ? raw : `http://${raw}`;
  return new URL(withScheme);
}

function protocolFor(origin: URL): "ws:" | "wss:" {
  return origin.protocol === "https:" ? "wss:" : "ws:";
}

function requestJson(url: URL): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const transport = url.protocol === "https:" ? https : http;
    const req = transport.get(url, (res) => {
      const chunks: Buffer[] = [];
      res.on("data", (chunk) => chunks.push(Buffer.from(chunk)));
      res.on("end", () => {
        const body = Buffer.concat(chunks).toString("utf8");
        if ((res.statusCode || 0) < 200 || (res.statusCode || 0) >= 300) {
          reject(new Error(`matchmaker ${res.statusCode}: ${body.slice(0, 200)}`));
          return;
        }
        try {
          resolve(JSON.parse(body));
        } catch (err) {
          reject(err);
        }
      });
    });
    req.on("error", reject);
    req.setTimeout(5000, () => {
      req.destroy(new Error(`matchmaker timeout: ${url.href}`));
    });
  });
}

function buildSeekUrl(options: Required<Pick<NativeToKrunkerOptions, "krunkerOrigin" | "region" | "hostname">> & Pick<NativeToKrunkerOptions, "gameId" | "dataQuery">): URL {
  const origin = normalizeOrigin(options.krunkerOrigin);
  const url = new URL("/mm/seek-game", origin);
  url.searchParams.set("hostname", options.hostname);
  url.searchParams.set("region", options.region);
  url.searchParams.set("autoChangeGame", "false");
  if (options.gameId) url.searchParams.set("game", options.gameId);
  if (options.dataQuery) url.searchParams.set("dataQuery", JSON.stringify(options.dataQuery));
  return url;
}

function wsUrlFromReply(reply: MatchmakerReply, fallbackOrigin: string): string {
  const origin = normalizeOrigin(fallbackOrigin);
  const host = reply.host || origin.hostname || DEFAULT_HOSTNAME;
  const port = Number(reply.port) || Number(origin.port) || 6767;
  const wsUrl = new URL("/ws", `${protocolFor(origin)}//${host}:${port}`);
  const gameId = reply.gameId || reply.lobbyId;
  if (gameId) wsUrl.searchParams.set("gameId", gameId);
  if (reply.clientKey) wsUrl.searchParams.set("clientKey", reply.clientKey);
  if (reply.pgi) wsUrl.searchParams.set("pgi", reply.pgi);
  if (reply.bpt) wsUrl.searchParams.set("bpt", reply.bpt);
  return wsUrl.href;
}

async function resolveUpstreamWsUrl(options: Required<Pick<NativeToKrunkerOptions, "krunkerOrigin" | "region" | "hostname">> & Pick<NativeToKrunkerOptions, "gameId" | "dataQuery" | "krunkerWsUrl">): Promise<string> {
  if (options.krunkerWsUrl) return options.krunkerWsUrl;
  const seekUrl = buildSeekUrl(options);
  console.info(`[krunker-proxy] seeking ${seekUrl.href}`);
  const reply = (await requestJson(seekUrl)) as MatchmakerReply;
  return wsUrlFromReply(reply, options.krunkerOrigin);
}

function writeNativeFrame(tcp: net.Socket, type: NetPacketType, payload: Buffer = Buffer.alloc(0)): void {
  if (payload.length > NET_MAX_PAYLOAD) throw new Error(`native payload too large for packet ${type}`);
  const frame = Buffer.alloc(NET_HEADER_SIZE + payload.length);
  frame[0] = type & 0xff;
  frame.writeUInt16BE(payload.length, 1);
  payload.copy(frame, NET_HEADER_SIZE);
  tcp.write(frame);
}

function writeNativeInit(session: NativeToKrunkerSession, mapIndex: number, modeIndex: number): void {
  session.mapIndex = Math.max(0, mapIndex);
  session.modeIndex = Math.max(0, modeIndex);

  const payload = Buffer.alloc(17);
  payload[0] = 1;
  payload[1] = 0;
  payload[2] = 0;
  payload.writeInt32LE(session.mapIndex, 3);
  payload.writeInt32LE(session.modeIndex, 7);
  payload.writeUInt16LE(session.tickRate || DEFAULT_TICK_RATE, 11);
  payload.writeInt32LE(session.ownSid ?? 0, 13);
  writeNativeFrame(session.tcp, NET_PACKET.INIT, payload);
}

function writeNativeSpawn(session: NativeToKrunkerSession, spawn: NativeSpawn): void {
  const payload = Buffer.alloc(74);
  payload.writeInt32LE(spawn.uid, 0);
  payload[4] = spawn.isYou ? 1 : 0;
  payload[5] = spawn.active ? 1 : 0;
  payload.writeInt32LE(spawn.classIndex, 6);
  payload.writeInt32LE(spawn.team, 10);
  payload.writeFloatLE(spawn.health, 14);
  payload.writeInt32LE(spawn.maxHealth, 18);
  payload.writeFloatLE(spawn.position[0], 22);
  payload.writeFloatLE(spawn.position[1], 26);
  payload.writeFloatLE(spawn.position[2], 30);
  payload.writeFloatLE(spawn.direction[0], 34);
  payload.writeFloatLE(spawn.direction[1], 38);
  Buffer.from(spawn.name || `Guest_${spawn.uid}`, "utf8").copy(payload, 42, 0, 31);
  writeNativeFrame(session.tcp, NET_PACKET.SPAWN, payload);

  session.healthByUid.set(spawn.uid, spawn.health);
  session.maxHealthByUid.set(spawn.uid, spawn.maxHealth);
  session.classByUid.set(spawn.uid, spawn.classIndex);
  session.teamByUid.set(spawn.uid, spawn.team);
  session.nameByUid.set(spawn.uid, spawn.name);
}

function writeNativeState(session: NativeToKrunkerSession, state: NativeState): void {
  const payload = Buffer.alloc(57);
  payload.writeInt32LE(state.uid, 0);
  payload.writeInt32LE(state.ackSeq, 4);
  payload[8] = state.active ? 1 : 0;
  payload.writeInt32LE(state.loadoutIndex, 9);
  payload.writeFloatLE(state.position[0], 13);
  payload.writeFloatLE(state.position[1], 17);
  payload.writeFloatLE(state.position[2], 21);
  payload.writeFloatLE(state.velocity[0], 25);
  payload.writeFloatLE(state.velocity[1], 29);
  payload.writeFloatLE(state.velocity[2], 33);
  payload.writeFloatLE(state.direction[0], 37);
  payload.writeFloatLE(state.direction[1], 41);
  payload.writeFloatLE(state.health, 45);
  payload.writeFloatLE(state.aimVal, 49);
  payload.writeFloatLE(state.crouchVal, 53);
  writeNativeFrame(session.tcp, NET_PACKET.STATE, payload);
  session.lastStateByUid.set(state.uid, state);
}

function sendUpstream(session: NativeToKrunkerSession, label: string, ...args: unknown[]): void {
  if (!session.upstreamReady || !session.upstream || session.upstream.readyState !== WebSocket.OPEN) {
    session.pendingUpstreamPackets.push([label, args]);
    return;
  }

  if (label !== "cptR") session.upstreamAhNum = UTILS.rotateAhNum(session.upstreamAhNum, ANTIHACK_KEY);
  const payload = UTILS.serializeNetworkMessage(label, ...args);
  session.upstream.send(UTILS.encodeNetworkMessage(payload, session.upstreamAhNum));
}

function flushUpstreamPackets(session: NativeToKrunkerSession): void {
  for (const [label, args] of session.pendingUpstreamPackets.splice(0)) {
    sendUpstream(session, label, ...args);
  }
}

function parseNativeInput(payload: Buffer): NativeInputPacket | null {
  if (payload.length < 22) return null;
  return {
    seq: payload.readInt32LE(0),
    moveDir: payload.readInt32LE(4),
    delta: payload.readFloatLE(8),
    pitch: payload.readFloatLE(12),
    yaw: payload.readFloatLE(16),
    flags: payload[20],
    swap: payload[21],
  };
}

function nativeMoveDirToKrunker(moveDir: number): number {
  if (moveDir < 0) return -1;
  return ((moveDir % 8) + 9) % 8;
}

function sendInputToUpstream(session: NativeToKrunkerSession, input: NativeInputPacket): void {
  sendUpstream(
    session,
    "i",
    input.seq,
    Math.max(0, input.delta) * 1000,
    input.yaw * 1000,
    input.pitch * 1000,
    nativeMoveDirToKrunker(input.moveDir),
    input.flags & NET_INPUT.SHOOT ? 1 : 0,
    input.flags & NET_INPUT.SCOPE ? 1 : 0,
    input.flags & NET_INPUT.JUMP ? 1 : 0,
    input.flags & NET_INPUT.CROUCH ? 1 : 0,
    input.flags & NET_INPUT.RELOAD ? 1 : 0,
    0,
    input.swap,
    0,
  );
}

function parseSpawn(payload: unknown, session: NativeToKrunkerSession): NativeSpawn | null {
  if (!Array.isArray(payload)) return null;
  const uid = toInt(payload[1]);
  const socketId = payload[0] == null ? null : String(payload[0]);
  const isYou = !!(session.socketId && socketId === session.socketId);
  if (isYou) session.ownSid = uid;

  return {
    uid,
    isYou,
    active: true,
    classIndex: toInt(payload[6]),
    team: toInt(payload[9]),
    health: toNumber(payload[7], 100),
    maxHealth: toInt(payload[8], 100),
    position: [toNumber(payload[2]), toNumber(payload[3], 10), toNumber(payload[4])],
    direction: [0, toNumber(payload[18], -Math.PI / 2)],
    name: String(payload[5] || payload[31] || `Guest_${uid}`),
  };
}

function stateFromL(session: NativeToKrunkerSession, reconciliation: unknown[]): NativeState | null {
  const uid = session.ownSid;
  if (uid == null) return null;
  const previous = session.lastStateByUid.get(uid);
  return {
    uid,
    ackSeq: toInt(reconciliation[0]),
    active: true,
    loadoutIndex: toInt(reconciliation[16], previous?.loadoutIndex || 0),
    position: [toNumber(reconciliation[2]), toNumber(reconciliation[3], 10), toNumber(reconciliation[4])],
    velocity: [toNumber(reconciliation[6]), toNumber(reconciliation[5]), toNumber(reconciliation[7])],
    direction: [previous?.direction[0] || 0, toNumber(reconciliation[8], previous?.direction[1] || 0)],
    health: session.healthByUid.get(uid) ?? previous?.health ?? 100,
    aimVal: toNumber(reconciliation[14], previous?.aimVal ?? 1),
    crouchVal: toNumber(reconciliation[15], previous?.crouchVal ?? 0),
  };
}

function stateFromK(session: NativeToKrunkerSession, data: unknown[], offset: number): NativeState | null {
  const uid = toInt(data[offset]);
  if (uid === session.ownSid) return null;
  const previous = session.lastStateByUid.get(uid);
  return {
    uid,
    ackSeq: previous?.ackSeq ?? 0,
    active: true,
    loadoutIndex: toInt(data[offset + 8], previous?.loadoutIndex || 0),
    position: [toNumber(data[offset + 1]), toNumber(data[offset + 2], 10), toNumber(data[offset + 3])],
    velocity: previous?.velocity || [0, 0, 0],
    direction: [toNumber(data[offset + 5]), toNumber(data[offset + 4])],
    health: session.healthByUid.get(uid) ?? previous?.health ?? 100,
    aimVal: toNumber(data[offset + 9], previous?.aimVal ?? 1),
    crouchVal: toNumber(data[offset + 7], previous?.crouchVal ?? 0),
  };
}

function sendInactiveSpawn(session: NativeToKrunkerSession, uid: number): void {
  writeNativeSpawn(session, {
    uid,
    isYou: uid === session.ownSid,
    active: false,
    classIndex: session.classByUid.get(uid) ?? 0,
    team: session.teamByUid.get(uid) ?? 0,
    health: session.healthByUid.get(uid) ?? 0,
    maxHealth: session.maxHealthByUid.get(uid) ?? 100,
    position: session.lastStateByUid.get(uid)?.position || [0, 10, 0],
    direction: session.lastStateByUid.get(uid)?.direction || [0, -Math.PI / 2],
    name: session.nameByUid.get(uid) || `Guest_${uid}`,
  });
  session.lastStateByUid.delete(uid);
}

function applyHealth(session: NativeToKrunkerSession, health: number, sidValue: unknown): void {
  const uid = sidValue == null ? session.ownSid : toInt(sidValue, -1);
  if (uid == null || uid < 0) return;
  session.healthByUid.set(uid, health);
  const previous = session.lastStateByUid.get(uid);
  if (previous) writeNativeState(session, { ...previous, health });
}

function handleUpstreamPacket(session: NativeToKrunkerSession, label: string, args: unknown[]): void {
  switch (label) {
    case "io-init":
      session.socketId = args[0] == null ? null : String(args[0]);
      break;
    case "pi":
      sendUpstream(session, "po");
      break;
    case "pir":
    case "load":
    case "ready":
    case "start":
    case "inst-id":
      break;
    case "init":
      writeNativeInit(session, toInt(args[0]), toInt(args[1]));
      sendUpstream(session, "load");
      break;
    case "0": {
      const spawn = parseSpawn(args[0], session);
      if (spawn) writeNativeSpawn(session, spawn);
      break;
    }
    case "2":
      sendInactiveSpawn(session, toInt(args[0], -1));
      break;
    case "l": {
      const reconciliation = Array.isArray(args[0]) ? args[0] : null;
      if (!reconciliation) break;
      const state = stateFromL(session, reconciliation);
      if (state) writeNativeState(session, state);
      sendUpstream(session, "v");
      break;
    }
    case "k": {
      const flat = Array.isArray(args[0]) ? args[0] : [];
      for (let i = 0; i + 9 < flat.length; i += 10) {
        const state = stateFromK(session, flat, i);
        if (state) writeNativeState(session, state);
      }
      break;
    }
    case "h":
      applyHealth(session, toNumber(args[0], 0), args[1]);
      break;
    case "error":
      console.error("[krunker-proxy] upstream error:", args[0]);
      break;
    default:
      break;
  }
}

function closeSession(session: NativeToKrunkerSession, force = false): void {
  if (session.closed) return;
  session.closed = true;
  sessions.delete(session);
  if (session.upstream) {
    try {
      if (force) session.upstream.terminate();
      else session.upstream.close();
    } catch (_err) {
      session.upstream.terminate();
    }
    session.upstream = null;
  }
  session.tcp.destroy();
}

async function connectUpstream(session: NativeToKrunkerSession, options: Required<Pick<NativeToKrunkerOptions, "krunkerOrigin" | "region" | "hostname">> & Pick<NativeToKrunkerOptions, "gameId" | "dataQuery" | "krunkerWsUrl">): Promise<void> {
  if (session.upstreamConnecting || session.upstreamReady || session.closed) return;
  session.upstreamConnecting = true;

  try {
    const wsUrl = await resolveUpstreamWsUrl(options);
    if (session.closed) return;
    console.info(`[krunker-proxy] session ${session.id} -> ${wsUrl}`);

    const upstream = new WebSocket(wsUrl);
    session.upstream = upstream;

    upstream.on("open", () => {
      session.upstreamReady = true;
      session.upstreamConnecting = false;
      flushUpstreamPackets(session);
    });

    upstream.on("message", (message: RawData) => {
      try {
        const [, label, args] = UTILS.decodePacket(message);
        handleUpstreamPacket(session, label, args);
      } catch (err) {
        console.error("[krunker-proxy] upstream decode error:", err);
        closeSession(session);
      }
    });

    upstream.on("close", () => {
      session.upstreamReady = false;
      if (!session.closed) closeSession(session);
    });

    upstream.on("error", (err: Error) => {
      console.error("[krunker-proxy] upstream socket error:", err.message);
      if (!session.closed) closeSession(session);
    });
  } catch (err) {
    session.upstreamConnecting = false;
    console.error("[krunker-proxy] failed to connect upstream:", err);
    closeSession(session);
  }
}

function handleNativePacket(session: NativeToKrunkerSession, type: number, payload: Buffer, options: Required<Pick<NativeToKrunkerOptions, "krunkerOrigin" | "region" | "hostname">> & Pick<NativeToKrunkerOptions, "gameId" | "dataQuery" | "krunkerWsUrl">): void {
  switch (type) {
    case NET_PACKET.HELLO:
      void connectUpstream(session, options);
      break;
    case NET_PACKET.ENT: {
      if (payload.length !== 4) break;
      const classIndex = payload.readInt32LE(0);
      sendUpstream(session, "ent", [classIndex, 0, [-1, -1], -1, -1, null, 0, 0, 0, -1, 0, 0, 0, null, 0, false, 0, -1, 0, 0, 0, 0]);
      break;
    }
    case NET_PACKET.INPUT: {
      const input = parseNativeInput(payload);
      if (input) sendInputToUpstream(session, input);
      break;
    }
    case NET_PACKET.CYCLE:
      if (!payload.length) sendUpstream(session, "cycle");
      break;
    default:
      break;
  }
}

function handleNativeData(session: NativeToKrunkerSession, chunk: Buffer, options: Required<Pick<NativeToKrunkerOptions, "krunkerOrigin" | "region" | "hostname">> & Pick<NativeToKrunkerOptions, "gameId" | "dataQuery" | "krunkerWsUrl">): void {
  session.readBuffer = Buffer.concat([session.readBuffer, chunk]);

  while (session.readBuffer.length >= NET_HEADER_SIZE) {
    const type = session.readBuffer[0];
    const length = session.readBuffer.readUInt16BE(1);
    if (length > NET_MAX_PAYLOAD) throw new Error(`invalid native payload length ${length}`);

    const frameLength = NET_HEADER_SIZE + length;
    if (session.readBuffer.length < frameLength) break;

    const payload = session.readBuffer.subarray(NET_HEADER_SIZE, frameLength);
    session.readBuffer = session.readBuffer.subarray(frameLength);
    handleNativePacket(session, type, payload, options);
  }
}

function createSession(tcp: net.Socket): NativeToKrunkerSession {
  return {
    id: nextSessionId++,
    tcp,
    readBuffer: Buffer.alloc(0),
    upstream: null,
    upstreamReady: false,
    upstreamConnecting: false,
    pendingUpstreamPackets: [],
    upstreamAhNum: 0,
    socketId: null,
    ownSid: null,
    tickRate: DEFAULT_TICK_RATE,
    mapIndex: 0,
    modeIndex: 0,
    healthByUid: new Map(),
    maxHealthByUid: new Map(),
    classByUid: new Map(),
    teamByUid: new Map(),
    nameByUid: new Map(),
    lastStateByUid: new Map(),
    closed: false,
  };
}

export function listen(options: NativeToKrunkerOptions = {}): net.Server {
  const listenHost = options.listenHost || process.env.KRUNK_NATIVE_LISTEN_HOST || process.env.KRUNK_NATIVE_HOST || "127.0.0.1";
  const listenPort = options.listenPort || Number.parseInt(process.env.KRUNK_NATIVE_LISTEN_PORT || process.env.KRUNK_NATIVE_PORT || String(DEFAULT_NATIVE_PORT), 10) || DEFAULT_NATIVE_PORT;
  const resolvedOptions = {
    krunkerOrigin: options.krunkerOrigin || process.env.KRUNKER_SERVER_ORIGIN || process.env.KRUNK_NATIVE_KRUNKER_ORIGIN || DEFAULT_KRUNKER_ORIGIN,
    krunkerWsUrl: options.krunkerWsUrl || process.env.KRUNKER_WS_URL || process.env.KRUNK_NATIVE_KRUNKER_WS || null,
    region: options.region || process.env.KRUNKER_REGION || process.env.KRUNK_NATIVE_REGION || DEFAULT_REGION,
    hostname: options.hostname || process.env.KRUNKER_HOSTNAME || process.env.KRUNK_NATIVE_MATCHMAKER_HOSTNAME || DEFAULT_HOSTNAME,
    gameId: options.gameId || process.env.KRUNKER_GAME_ID || null,
    dataQuery: options.dataQuery || { v: version },
  };

  const server = net.createServer((tcp) => {
    tcp.setNoDelay(true);
    const session = createSession(tcp);
    sessions.add(session);
    console.info(`[krunker-proxy] native client connected session=${session.id}`);

    tcp.on("data", (chunk: Buffer) => {
      try {
        handleNativeData(session, chunk, resolvedOptions);
      } catch (err) {
        console.error("[krunker-proxy] native decode error:", err);
        closeSession(session);
      }
    });
    tcp.on("close", () => closeSession(session));
    tcp.on("error", (err: Error) => {
      console.error("[krunker-proxy] native socket error:", err.message);
      closeSession(session);
    });
  });

  server.on("listening", () => {
    console.info(
      `[krunker-proxy] listening for KrunkNative TCP on ${listenHost}:${listenPort}; upstream ${resolvedOptions.krunkerWsUrl || resolvedOptions.krunkerOrigin}`,
    );
  });
  server.on("close", () => {
    for (const session of Array.from(sessions)) closeSession(session);
  });
  server.listen({ host: listenHost, port: listenPort });
  return server;
}

export function closeAllSessions(force = false): void {
  for (const session of Array.from(sessions)) closeSession(session, force);
}

export function getStats(): NativeToKrunkerStats {
  let upstreamConnected = 0;
  for (const session of sessions) {
    if (session.upstreamReady) upstreamConnected++;
  }
  return {
    sessions: sessions.size,
    upstreamConnected,
  };
}
