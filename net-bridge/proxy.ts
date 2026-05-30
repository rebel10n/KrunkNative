"use strict";

import net from "net";
import type { IncomingMessage, Server as HttpServer } from "http";
import { WebSocket, WebSocketServer, type RawData } from "ws";
import antihackKey from "./antihackkey.json";
import * as UTILS from "./utils";

const WS_PATH = process.env.KRUNK_NATIVE_WS_PATH || "/ws";
const NATIVE_HOST = process.env.KRUNK_NATIVE_HOST || "127.0.0.1";
const NATIVE_PORT =
  Number.parseInt(process.env.KRUNK_NATIVE_PORT || "21015", 10) || 21015;
const LOBBY_ID = process.env.KRUNK_NATIVE_LOBBY_ID || "KN:local";
const DEFAULT_TICK_RATE = 30;
const NET_MAX_PAYLOAD = 512;
const NET_HEADER_SIZE = 3;
const ANTIHACK_KEY = Number(antihackKey) || 0;

export const NET_PACKET = Object.freeze({
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

const CHARS = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

type Vec2 = [number, number];
type Vec3 = [number, number, number];

type NativeInitPacket = {
  major: number;
  minor: number;
  patch: number;
  mapIndex: number;
  modeIndex: number;
  tickRate: number;
  clientId: number;
};

type NativeSpawnPacket = {
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

type NativeStatePacket = {
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

type BridgeSession = {
  id: string;
  ws: WebSocket;
  request: IncomingMessage;
  tcp: net.Socket | null;
  nativeConnected: boolean;
  nativeClientId: number | null;
  tickRate: number;
  mapIndex: number;
  modeIndex: number;
  pendingNativeFrames: Buffer[];
  readBuffer: Buffer;
  inputFieldState: unknown[] | null;
  lastInputSeq: number;
  clientAhNum: number;
  clientAhSynced: boolean;
  lastPingSentAt: number;
  pingMs: number;
  uidToBrowserId: Map<number, string>;
  activeUids: Set<number>;
  lastHealthByUid: Map<number, number>;
  closed: boolean;
};

export type BridgeStats = {
  sessions: number;
  nativeConnected: number;
};

const sessions = new Set<BridgeSession>();

function makeId(length = 5): string {
  let out = "";
  for (let i = 0; i < length; i++) {
    out += CHARS[(Math.random() * CHARS.length) | 0];
  }
  return out;
}

function isWsOpen(ws: WebSocket | null | undefined): boolean {
  return !!ws && ws.readyState === WebSocket.OPEN;
}

function sendWs(session: BridgeSession, label: string, ...args: unknown[]): void {
  if (!isWsOpen(session.ws)) return;
  const payload = UTILS.serializeNetworkMessage(label, ...args);
  session.ws.send(UTILS.encodeNetworkMessage(payload, 0));
}

function sendPing(session: BridgeSession): void {
  session.lastPingSentAt = Date.now();
  sendWs(session, "pi", null);
}

function closeWs(session: BridgeSession, code = 1011, reason = "bridge closed"): void {
  if (!isWsOpen(session.ws)) return;
  try {
    session.ws.close(code, reason);
  } catch (_err) {
    session.ws.terminate();
  }
}

function writeNativePacket(
  session: BridgeSession,
  type: NetPacketType,
  payload: Buffer | Uint8Array = Buffer.alloc(0),
): void {
  const payloadBuffer = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  if (payloadBuffer.length > NET_MAX_PAYLOAD) {
    throw new Error(`native payload too large for packet ${type}`);
  }

  const frame = Buffer.alloc(NET_HEADER_SIZE + payloadBuffer.length);
  frame[0] = type & 0xff;
  frame.writeUInt16BE(payloadBuffer.length, 1);
  payloadBuffer.copy(frame, NET_HEADER_SIZE);

  if (!session.nativeConnected || !session.tcp) {
    session.pendingNativeFrames.push(frame);
    return;
  }

  session.tcp.write(frame);
}

function writeNativeHello(session: BridgeSession): void {
  writeNativePacket(session, NET_PACKET.HELLO, Buffer.from([1, 0, 0]));
}

function writeNativeEnt(session: BridgeSession, entData: unknown): void {
  const payload = Buffer.alloc(4);
  const classIndex = Array.isArray(entData) ? Number(entData[0]) || 0 : 0;
  payload.writeInt32LE(classIndex, 0);
  writeNativePacket(session, NET_PACKET.ENT, payload);
}

function writeNativeCycle(session: BridgeSession): void {
  writeNativePacket(session, NET_PACKET.CYCLE);
}

function flushPendingNativeFrames(session: BridgeSession): void {
  if (!session.nativeConnected || !session.tcp || !session.pendingNativeFrames.length) return;
  for (const frame of session.pendingNativeFrames.splice(0)) {
    session.tcp.write(frame);
  }
}

function clampByte(value: unknown): number {
  const n = Number(value) || 0;
  if (n <= 0) return 0;
  if (n >= 255) return 255;
  return n | 0;
}

function krunkerMoveDirToNative(moveDir: number): number {
  if (moveDir < 0) return -1;
  return moveDir + 7;
}

function writeNativeInput(session: BridgeSession, frame: unknown[]): void {
  if (!Array.isArray(frame)) return;

  const payload = Buffer.alloc(22);
  const seq = Number(frame[0]) || 0;
  const deltaMs = Number(frame[1]) || 0;
  const yaw = (Number(frame[2]) || 0) / 1000.0;
  const pitch = (Number(frame[3]) || 0) / 1000.0;
  const moveDir = Number.isFinite(Number(frame[4])) ? krunkerMoveDirToNative(Number(frame[4]) | 0) : -1;

  let flags = 0;
  if (frame[5]) flags |= NET_INPUT.SHOOT;
  if (frame[6]) flags |= NET_INPUT.SCOPE;
  if (frame[7]) flags |= NET_INPUT.JUMP;
  if (frame[8]) flags |= NET_INPUT.CROUCH;
  if (frame[9]) flags |= NET_INPUT.RELOAD;

  const switchAction = Number(frame[11]) || 0;
  const scroll = Number(frame[10]) || 0;
  let swap = switchAction;
  if (!swap && scroll) swap = scroll > 0 ? 1 : 2;

  payload.writeInt32LE(seq, 0);
  payload.writeInt32LE(moveDir, 4);
  payload.writeFloatLE(Math.max(0, deltaMs) / 1000.0, 8);
  payload.writeFloatLE(pitch, 12);
  payload.writeFloatLE(yaw, 16);
  payload[20] = flags;
  payload[21] = clampByte(swap);

  writeNativePacket(session, NET_PACKET.INPUT, payload);
}

function ensureInputState(session: BridgeSession): unknown[] {
  if (!session.inputFieldState) {
    session.inputFieldState = new Array(13).fill(0);
  }
  return session.inputFieldState;
}

function decodeDeltas(encodedDeltas: unknown, width: number): number[] {
  if (Array.isArray(encodedDeltas)) return encodedDeltas.map(Number);
  if (typeof encodedDeltas === "string" && width > 0) {
    const deltas: number[] = [];
    for (let i = 0; i + width <= encodedDeltas.length; i += width) {
      deltas.push(Number.parseInt(encodedDeltas.slice(i, i + width), 10));
    }
    return deltas;
  }
  if (encodedDeltas != null) return [Number(encodedDeltas) || 0];
  return [];
}

function decodeAndWriteQ(session: BridgeSession, args: unknown[]): void {
  const firstSN = Number(args?.[0]) || 0;
  const aimDeltas = Array.isArray(args?.[1]) ? args[1] : null;
  const diffs =
    args?.[2] && typeof args[2] === "object" && !Array.isArray(args[2])
      ? (args[2] as Record<string, unknown>)
      : null;
  const deltaStringWidth = Number(args?.[4]) || 0;
  const deltas = decodeDeltas(args?.[3], deltaStringWidth).map((value) => Number(value) || 0);
  if (!deltas.length) return;

  const state = ensureInputState(session);
  let yaw = Number(state[2]) || 0;
  let pitch = Number(state[3]) || 0;
  if (aimDeltas && aimDeltas.length >= 2) {
    yaw = Number(aimDeltas[0]) || 0;
    pitch = Number(aimDeltas[1]) || 0;
  }

  for (let i = 0; i < deltas.length; i++) {
    if (aimDeltas && i > 0) {
      const deltaIndex = 2 + (i - 1) * 2;
      if (deltaIndex + 1 < aimDeltas.length) {
        yaw += Number(aimDeltas[deltaIndex]) || 0;
        pitch += Number(aimDeltas[deltaIndex + 1]) || 0;
      }
    }

    for (let field = 4; field <= 12; field++) {
      const key = `${i}-${field}`;
      if (diffs && diffs[key] !== undefined) state[field] = diffs[key];
    }

    const frame = new Array(13).fill(0);
    frame[0] = firstSN + i;
    frame[1] = deltas[i];
    frame[2] = yaw;
    frame[3] = pitch;
    for (let field = 4; field <= 12; field++) frame[field] = state[field];

    writeNativeInput(session, frame);
    session.lastInputSeq = Number(frame[0]) || session.lastInputSeq;
  }

  state[2] = yaw;
  state[3] = pitch;
}

function decodeAndWriteI(session: BridgeSession, args: unknown[]): void {
  const frame = args.slice(0, 13);
  while (frame.length < 13) frame.push(0);
  writeNativeInput(session, frame);
  session.lastInputSeq = Number(frame[0]) || session.lastInputSeq;
}

function readCString(buffer: Buffer, offset: number, length: number): string {
  const end = buffer.indexOf(0, offset);
  const cappedEnd = end >= offset && end < offset + length
    ? end
    : Math.min(offset + length, buffer.length);
  return buffer.toString("utf8", offset, cappedEnd);
}

function parseInitPacket(payload: Buffer): NativeInitPacket | null {
  if (payload.length < 17) return null;
  return {
    major: payload[0],
    minor: payload[1],
    patch: payload[2],
    mapIndex: payload.readInt32LE(3),
    modeIndex: payload.readInt32LE(7),
    tickRate: payload.readUInt16LE(11),
    clientId: payload.readInt32LE(13),
  };
}

function parseSpawnPacket(payload: Buffer): NativeSpawnPacket | null {
  if (payload.length < 74) return null;
  return {
    uid: payload.readInt32LE(0),
    isYou: !!payload[4],
    active: !!payload[5],
    classIndex: payload.readInt32LE(6),
    team: payload.readInt32LE(10),
    health: payload.readFloatLE(14),
    maxHealth: payload.readInt32LE(18),
    position: [
      payload.readFloatLE(22),
      payload.readFloatLE(26),
      payload.readFloatLE(30),
    ],
    direction: [payload.readFloatLE(34), payload.readFloatLE(38)],
    name: readCString(payload, 42, 32),
  };
}

function parseStatePacket(payload: Buffer): NativeStatePacket | null {
  if (payload.length < 57) return null;
  return {
    uid: payload.readInt32LE(0),
    ackSeq: payload.readInt32LE(4),
    active: !!payload[8],
    loadoutIndex: payload.readInt32LE(9),
    position: [
      payload.readFloatLE(13),
      payload.readFloatLE(17),
      payload.readFloatLE(21),
    ],
    velocity: [
      payload.readFloatLE(25),
      payload.readFloatLE(29),
      payload.readFloatLE(33),
    ],
    direction: [payload.readFloatLE(37), payload.readFloatLE(41)],
    health: payload.readFloatLE(45),
    aimVal: payload.readFloatLE(49),
    crouchVal: payload.readFloatLE(53),
  };
}

function browserIdForUid(session: BridgeSession, uid: number, isYou = false): string {
  if (isYou || uid === session.nativeClientId) return session.id;
  let id = session.uidToBrowserId.get(uid);
  if (!id) {
    id = `n${uid}`;
    session.uidToBrowserId.set(uid, id);
  }
  return id;
}

function buildSyncData(): Record<string, unknown> {
  return {
    dest: 0,
    flg: 0,
    spwns: 0,
    gor: 1,
    pkups: 0,
    gates: 0,
    banks: 0,
    bill: {},
    zone: 0,
    lck: 0,
  };
}

function buildSpawnPayload(session: BridgeSession, spawn: NativeSpawnPacket): unknown[] {
  const name = spawn.name || `Guest_${spawn.uid + 1}`;
  return [
    browserIdForUid(session, spawn.uid, spawn.isYou),
    spawn.uid,
    spawn.position[0],
    spawn.position[1],
    spawn.position[2],
    name,
    spawn.classIndex,
    spawn.health,
    spawn.maxHealth,
    spawn.team,
    100,
    null,
    [-1, -1],
    -1,
    -1,
    null,
    false,
    null,
    spawn.direction[1],
    -1,
    0,
    0,
    0,
    null,
    -1,
    0,
    0,
    0,
    0,
    -1,
    0,
    name,
    false,
    -1,
    0,
    0,
    0,
    0,
    null,
    null,
  ];
}

function buildLPacket(session: BridgeSession, state: NativeStatePacket): unknown[] {
  return [
    state.ackSeq,
    session.pingMs || 25,
    state.position[0],
    state.position[1],
    state.position[2],
    state.velocity[1],
    state.velocity[0],
    state.velocity[2],
    state.direction[1],
    true,
    false,
    false,
    0,
    false,
    state.aimVal,
    state.crouchVal,
    state.loadoutIndex,
    0,
    true,
    false,
    0,
    0,
    0,
  ];
}

function buildKPayload(state: NativeStatePacket): unknown[] {
  return [
    state.uid,
    state.position[0],
    state.position[1],
    state.position[2],
    state.direction[1],
    state.direction[0],
    1,
    state.crouchVal,
    state.loadoutIndex,
    state.aimVal,
  ];
}

function sendHealthIfChanged(session: BridgeSession, uid: number, health: number): void {
  const roundedHealth = Math.round(health);
  const previous = session.lastHealthByUid.get(uid);
  if (previous === roundedHealth) return;
  session.lastHealthByUid.set(uid, roundedHealth);
  sendWs(session, "h", roundedHealth, uid === session.nativeClientId ? null : uid);
}

function sendInit(session: BridgeSession, init: NativeInitPacket): void {
  session.nativeClientId = init.clientId;
  session.tickRate = init.tickRate || DEFAULT_TICK_RATE;
  session.mapIndex = Math.max(0, init.mapIndex);
  session.modeIndex = Math.max(0, init.modeIndex);

  sendWs(
    session,
    "init",
    session.mapIndex,
    session.modeIndex,
    null,
    null,
    null,
    { maps: [session.mapIndex] },
    null,
    0,
    null,
    buildSyncData(),
    true,
  );
  sendWs(session, "start", 0, 1, false, false, false, true);
}

function handleNativePacket(session: BridgeSession, type: number, payload: Buffer): void {
  switch (type) {
    case NET_PACKET.INIT: {
      const init = parseInitPacket(payload);
      if (init) sendInit(session, init);
      break;
    }
    case NET_PACKET.SPAWN: {
      const spawn = parseSpawnPacket(payload);
      if (!spawn) break;
      const sid = spawn.uid;
      if (!spawn.active) {
        session.activeUids.delete(sid);
        session.lastHealthByUid.delete(sid);
        sendWs(session, "2", sid);
        break;
      }

      session.activeUids.add(sid);
      session.lastHealthByUid.set(sid, spawn.health);
      sendWs(session, "0", buildSpawnPayload(session, spawn), 1);
      if (spawn.isYou || sid === session.nativeClientId) {
        sendWs(session, "start", 0, 1, false, false, false, true);
        sendWs(session, "7", [sid, 0]);
      }
      break;
    }
    case NET_PACKET.STATE: {
      const state = parseStatePacket(payload);
      if (!state) break;
      if (!state.active) {
        session.activeUids.delete(state.uid);
        session.lastHealthByUid.delete(state.uid);
        sendWs(session, "2", state.uid);
        break;
      }

      session.activeUids.add(state.uid);
      sendHealthIfChanged(session, state.uid, state.health);

      if (state.uid === session.nativeClientId) {
        sendWs(session, "l", buildLPacket(session, state), true);
      } else {
        sendWs(session, "k", buildKPayload(state), session.tickRate || DEFAULT_TICK_RATE);
      }
      break;
    }
    default:
      break;
  }
}

function handleNativeData(session: BridgeSession, chunk: Buffer): void {
  session.readBuffer = Buffer.concat([session.readBuffer, chunk]);

  while (session.readBuffer.length >= NET_HEADER_SIZE) {
    const type = session.readBuffer[0];
    const length = session.readBuffer.readUInt16BE(1);
    if (length > NET_MAX_PAYLOAD) {
      throw new Error(`invalid native payload length ${length}`);
    }

    const frameLength = NET_HEADER_SIZE + length;
    if (session.readBuffer.length < frameLength) break;

    const payload = session.readBuffer.subarray(NET_HEADER_SIZE, frameLength);
    session.readBuffer = session.readBuffer.subarray(frameLength);
    handleNativePacket(session, type, payload);
  }
}

function handleBrowserPacket(session: BridgeSession, label: string, args: unknown[]): void {
  switch (label) {
    case "ent":
      writeNativeEnt(session, args[0]);
      break;
    case "q":
      decodeAndWriteQ(session, args);
      break;
    case "i":
      decodeAndWriteI(session, args);
      break;
    case "load":
      sendWs(session, "ready", 0, 1);
      break;
    case "po":
      session.pingMs = Math.max(0, Math.min(999, Date.now() - session.lastPingSentAt));
      sendWs(session, "pir", 1);
      setTimeout(() => sendPing(session), 5000);
      break;
    case "cycle":
    case "nextMap":
      writeNativeCycle(session);
      break;
    case "v":
    case "csv":
    case "sb":
    case "a":
    case "ct":
    case "maVote":
    case "k":
    case "int":
      break;
    default:
      break;
  }
}

function createSession(ws: WebSocket, request: IncomingMessage): BridgeSession {
  return {
    id: makeId(),
    ws,
    request,
    tcp: null,
    nativeConnected: false,
    nativeClientId: null,
    tickRate: DEFAULT_TICK_RATE,
    mapIndex: 0,
    modeIndex: 0,
    pendingNativeFrames: [],
    readBuffer: Buffer.alloc(0),
    inputFieldState: null,
    lastInputSeq: -1,
    clientAhNum: 0,
    clientAhSynced: false,
    lastPingSentAt: Date.now(),
    pingMs: 25,
    uidToBrowserId: new Map(),
    activeUids: new Set(),
    lastHealthByUid: new Map(),
    closed: false,
  };
}

function closeSession(session: BridgeSession, force = false): void {
  if (session.closed) return;
  session.closed = true;
  sessions.delete(session);
  if (force) {
    try {
      session.ws.terminate();
    } catch (_err) {}
  } else {
    closeWs(session);
  }
  if (session.tcp) {
    session.tcp.destroy();
    session.tcp = null;
  }
}

function connectNative(session: BridgeSession): void {
  const tcp = net.createConnection(
    {
      host: NATIVE_HOST,
      port: NATIVE_PORT,
    },
    () => {
      tcp.setNoDelay(true);
      session.nativeConnected = true;
      sendPing(session);
      sendWs(session, "load", 20000, session.id);
      sendWs(session, "io-init", session.id);
      sendWs(session, "inst-id", LOBBY_ID);
      writeNativeHello(session);
      flushPendingNativeFrames(session);
    },
  );

  session.tcp = tcp;

  tcp.on("data", (chunk: Buffer) => {
    try {
      handleNativeData(session, chunk);
    } catch (err) {
      console.error("[proxy] native decode error:", err);
      sendWs(session, "error", "Native server protocol error");
      closeWs(session);
      closeSession(session);
    }
  });

  tcp.on("close", () => {
    session.nativeConnected = false;
    closeWs(session, 1011, "native server closed");
  });

  tcp.on("error", (err: Error) => {
    console.error("[proxy] native socket error:", err.message);
    sendWs(session, "error", `Native server unavailable: ${err.message}`);
    closeWs(session);
  });
}

function handleConnection(ws: WebSocket, request: IncomingMessage): void {
  const session = createSession(ws, request);
  sessions.add(session);
  connectNative(session);

  ws.on("message", (message: RawData) => {
    let decoded: UTILS.DecodedPacket;
    try {
      decoded = UTILS.decodePacket(message);
    } catch (err) {
      console.error("[proxy] client msgpack decode error:", err);
      sendWs(session, "error", "Invalid client packet");
      return;
    }

    const [ahNum, label, args] = decoded;
    if (label !== "cptR") {
      session.clientAhNum = UTILS.rotateAhNum(session.clientAhNum, ANTIHACK_KEY);
      if (ahNum !== session.clientAhNum) {
        if (!session.clientAhSynced) {
          session.clientAhNum = ahNum;
          session.clientAhSynced = true;
        } else {
          console.warn("[proxy] client antihack padding mismatch", {
            label,
            got: ahNum,
            expected: session.clientAhNum,
          });
          session.clientAhNum = ahNum;
        }
      } else if (!session.clientAhSynced) {
        session.clientAhSynced = true;
      }
    }
    handleBrowserPacket(session, label, args);
  });

  ws.on("close", () => closeSession(session));
  ws.on("error", (err: Error) => {
    console.error("[proxy] websocket error:", err.message);
    closeSession(session);
  });
}

export function attach(http: HttpServer): WebSocketServer {
  const wss = new WebSocketServer({ noServer: true });

  wss.on("connection", handleConnection);

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

    wss.handleUpgrade(request, socket, head, (ws) => {
      wss.emit("connection", ws, request);
    });
  });

  http.on("close", () => wss.close());
  console.info(`[proxy] forwarding ${WS_PATH} to native ${NATIVE_HOST}:${NATIVE_PORT}`);

  return wss;
}

export function closeAllSessions(force = false): void {
  for (const session of Array.from(sessions)) closeSession(session, force);
}

export function getStats(): BridgeStats {
  let nativeConnected = 0;
  for (const session of sessions) {
    if (session.nativeConnected) nativeConnected++;
  }
  return {
    sessions: sessions.size,
    nativeConnected,
  };
}
