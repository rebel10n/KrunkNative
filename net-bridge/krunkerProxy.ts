"use strict";

import http from "http";
import https from "https";
import net from "net";
import { WebSocket, type RawData } from "ws";
import antihackKey from "./antihackkey.json";
import version from "./version.json";
import * as UTILS from "./utils";
import * as EV from "./nativeEvents";

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
  SKINS: 35, // NetSkinsPacket (EV.NET_PACKET_SKINS): Krunker spawn skins / meleeIndex, before SPAWN
  RECON: 110, // NetReconPacket: Krunker "l"
  INPUT_BATCH: 111, // NetInputBatchPacket: Krunker "q" (count byte + 24-byte NetInputPackets)
  CHAT_SEND: EV.NET_EVENT.CHAT_SEND,
  CHAT: EV.NET_EVENT.CHAT,
  KILL: EV.NET_EVENT.KILL,
  HIT: EV.NET_EVENT.HIT,
  HEALTH: EV.NET_EVENT.HEALTH,
  SCORE: EV.NET_EVENT.SCORE,
  LEADERBOARD: EV.NET_EVENT.LEADERBOARD,
  TIMER: EV.NET_EVENT.TIMER,
  END: EV.NET_EVENT.END,
  PING: EV.NET_EVENT.PING,
  PONG: EV.NET_EVENT.PONG,
  PING_RESULT: EV.NET_EVENT.PING_RESULT,
  LEAVE: EV.NET_EVENT.LEAVE,
  SHOT: EV.NET_FX.SHOT,
  PROJECTILE: EV.NET_FX.PROJECTILE,
  PROJECTILE_END: EV.NET_FX.PROJECTILE_END,
  EXPLOSION: EV.NET_FX.EXPLOSION,
  SPECTATE: EV.NET_SPECTATE.SPECTATE,
  VOICE_SEND: 31, // C->S NetSocialChunk pieces of a "vc" data URL (shared/include/voice.h)
  VOICE: 32, // S->C NetSocialChunk pieces of [alias length u8][alias][data URL], chunk id = sid
} as const);

// shared/include/social.h NetSocialChunk: u32 id, u16 index, u16 count, data.
const VOICE_CHUNK_HEADER = 8;
const VOICE_CHUNK_DATA = NET_MAX_PAYLOAD - VOICE_CHUNK_HEADER;
const VOICE_MAX_DATA = 256 * 1024; // io_vc cap

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
  slowdown: number; // input[12] (1 - speedLmt.round(1)); 0 for 22-byte v1 packets
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
  secondaryIndex: number;
  cosmetics: EV.Cosmetics;
  skins: EV.WeaponSkins;
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
  killsByUid: Map<number, number>;
  deathsByUid: Map<number, number>;
  ownScore: number;
  pingMs: number;
  voiceParts: Buffer[]; // NET_PACKET_VOICE_SEND chunks of the clip being reassembled
  voiceNext: number;
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
  payload[1] = 2; // protocol 1.2: this bridge reads ENT V5 / V6 (spray, weapon skins) and V7 (lagComp)
  payload[2] = 0;
  payload.writeInt32LE(session.mapIndex, 3);
  payload.writeInt32LE(session.modeIndex, 7);
  payload.writeUInt16LE(session.tickRate || DEFAULT_TICK_RATE, 11);
  payload.writeInt32LE(session.ownSid ?? 0, 13);
  writeNativeFrame(session.tcp, NET_PACKET.INIT, payload);
}

function writeNativeSpawn(session: NativeToKrunkerSession, spawn: NativeSpawn): void {
  const skins = Buffer.alloc(EV.SKINS_PACKET_SIZE); // the weapon skins go first: the client builds meshes on SPAWN
  skins.writeInt32LE(spawn.uid, 0);
  EV.writeSkins(skins, 4, spawn.skins);
  writeNativeFrame(session.tcp, NET_PACKET.SKINS, skins);

  const payload = Buffer.alloc(EV.SPAWN_COSMETICS_SIZE);
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
  payload.writeInt32LE(spawn.secondaryIndex, 74);
  EV.writeCosmetics(payload, EV.SPAWN_V1_SIZE, spawn.cosmetics);
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

// Krunker "l" -> NetReconPacket (58 bytes, shared/include/shared.h): the native client reconciles.
function writeNativeRecon(session: NativeToKrunkerSession, l: unknown[]): void {
  const payload = Buffer.alloc(58);
  payload.writeInt32LE(Math.min(toInt(l[0]), 0x7fffffff), 0);
  payload.writeFloatLE(toNumber(l[2]), 4);
  payload.writeFloatLE(toNumber(l[3]), 8);
  payload.writeFloatLE(toNumber(l[4]), 12);
  payload.writeFloatLE(toNumber(l[6]), 16); // velX
  payload.writeFloatLE(toNumber(l[5]), 20); // velY
  payload.writeFloatLE(toNumber(l[7]), 24); // velZ
  payload.writeFloatLE(toNumber(l[8]), 28); // yaw
  payload.writeFloatLE(1 - toNumber(l[14], 1), 32); // Krunker aimVal 0 = aimed, KrunkNative 1
  payload.writeFloatLE(toNumber(l[15]), 36);
  payload.writeInt32LE(l[16] == null ? -1 : toInt(l[16]), 40);
  payload.writeFloatLE(toNumber(l[17]) / 1000, 44); // slideTimer ms -> s
  payload.writeFloatLE(toNumber(l[21]), 48);
  payload.writeFloatLE(toNumber(l[22]), 52);
  payload[56] = toInt(l[12]) & 7;
  payload[57] = (l[9] ? 0x01 : 0) | (l[10] ? 0x02 : 0) | (l[11] ? 0x04 : 0) | (l[13] ? 0x08 : 0) |
    (l[18] !== false ? 0x10 : 0) | (l[19] ? 0x20 : 0);
  writeNativeFrame(session.tcp, NET_PACKET.RECON, payload);
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
    slowdown: payload.length >= 23 ? Math.min(payload[22], 10) / 10 : 0,
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
    input.slowdown,
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
    secondaryIndex: toInt(payload[15], -1),
    cosmetics: EV.cosmeticsFromSpawn(payload),
    skins: EV.skinsFromKrunker(payload[12], payload[19]),
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
    case "start":
      // "start" [timeString, team, spectating, ...]: a spectating start -> native SPECTATE
      if (args[2]) writeNativeFrame(session.tcp, NET_PACKET.SPECTATE, Buffer.from([1]));
      break;
    case "pir":
    case "load":
    case "ready":
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
    case "2": {
      const uid = toInt(args[0], -1);
      if (uid < 0) break;
      writeNativeFrame(session.tcp, NET_PACKET.LEAVE, EV.encodeInt32(uid));
      session.lastStateByUid.delete(uid);
      session.nameByUid.delete(uid);
      break;
    }
    case "l": {
      const reconciliation = Array.isArray(args[0]) ? args[0] : null;
      if (!reconciliation) break;
      const state = stateFromL(session, reconciliation);
      if (state) session.lastStateByUid.set(state.uid, state);
      if (args[1]) writeNativeRecon(session, reconciliation);
      const ping = toInt(reconciliation[1], -1);
      if (ping >= 0 && ping !== session.pingMs) {
        session.pingMs = ping;
        writeNativeFrame(session.tcp, NET_PACKET.PING_RESULT, EV.encodeInt32(ping));
      }
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
      writeNativeHealth(session, args);
      break;
    default:
      handleUpstreamEvent(session, label, args);
      break;
    case "error":
      console.error("[krunker-proxy] upstream error:", args[0]);
      break;
  }
}

function writeNativeHealth(session: NativeToKrunkerSession, args: unknown[]): void {
  const uid = args[1] == null ? session.ownSid : toInt(args[1], -1);
  if (uid == null || uid < 0) return;
  const hasSource = args[1] == null && args[2] != null && args[3] != null;
  writeNativeFrame(
    session.tcp,
    NET_PACKET.HEALTH,
    EV.encodeHealth({ uid, health: toNumber(args[0], 0), hasSource, sourceX: toNumber(args[2]), sourceZ: toNumber(args[3]) }),
  );
}

function boardFromFlat(session: NativeToKrunkerSession, flat: unknown[], stride: number, scoreOffset: number): EV.LeaderboardEntry[] {
  const rows: EV.LeaderboardEntry[] = [];
  for (let i = 0; i + stride - 1 < flat.length && rows.length < EV.LEADERBOARD_MAX; i += stride) {
    const uid = toInt(flat[i], -1);
    if (uid < 0) continue;
    rows.push({
      uid,
      score: toInt(flat[i + scoreOffset]),
      kills: stride > 3 ? toInt(flat[i + 3]) : session.killsByUid.get(uid) ?? 0,
      deaths: stride > 4 ? toInt(flat[i + 4]) : session.deathsByUid.get(uid) ?? 0,
    });
  }
  return rows;
}

// NET_PACKET_VOICE_SEND chunk -> the whole data URL once its last chunk arrived.
function takeVoiceChunk(session: NativeToKrunkerSession, payload: Buffer): string | null {
  if (payload.length < VOICE_CHUNK_HEADER) return null;
  const index = payload.readUInt16LE(4);
  const count = payload.readUInt16LE(6);
  if (!count || index >= count || index !== (index === 0 ? 0 : session.voiceNext)) {
    session.voiceParts = [];
    session.voiceNext = 0;
    return null;
  }
  if (index === 0) session.voiceParts = [];
  session.voiceParts.push(Buffer.from(payload.subarray(VOICE_CHUNK_HEADER)));
  session.voiceNext = index + 1;
  if (session.voiceNext < count) return null;
  const audio = Buffer.concat(session.voiceParts).toString("utf8");
  session.voiceParts = [];
  session.voiceNext = 0;
  return audio.length && audio.length <= VOICE_MAX_DATA ? audio : null;
}

// ["vc", audio, alias, sid] -> NET_PACKET_VOICE chunks (shared/src/voice.c voice_relay).
function writeNativeVoice(session: NativeToKrunkerSession, args: unknown[]): void {
  if (typeof args[0] !== "string" || !args[0].length) return;
  const alias = Buffer.from(String(args[1] ?? ""), "utf8").subarray(0, 255);
  const msg = Buffer.concat([Buffer.from([alias.length]), alias, Buffer.from(args[0], "utf8")]);
  const sid = toInt(args[2], -1) >>> 0;
  const count = Math.ceil(msg.length / VOICE_CHUNK_DATA);
  for (let i = 0; i < count; i++) {
    const data = msg.subarray(i * VOICE_CHUNK_DATA, (i + 1) * VOICE_CHUNK_DATA);
    const header = Buffer.alloc(VOICE_CHUNK_HEADER);
    header.writeUInt32LE(sid, 0);
    header.writeUInt16LE(i, 4);
    header.writeUInt16LE(count, 6);
    writeNativeFrame(session.tcp, NET_PACKET.VOICE, Buffer.concat([header, data]));
  }
}

// Krunker v3.1.1 match events -> native packets (nativeEvents.ts).
function handleUpstreamEvent(session: NativeToKrunkerSession, label: string, args: unknown[]): void {
  switch (label) {
    case "vc":
      writeNativeVoice(session, args); // voiceChat(audio, alias, sid)
      break;
    case "ch": {
      // gB(name, text, type, color): type 1 game line, 2 server line, else a player line.
      const type = toInt(args[2], 0);
      const kind = type === 2 ? EV.NET_CHAT_KIND.SERVER : type === 1 ? EV.NET_CHAT_KIND.GAME : EV.NET_CHAT_KIND.PLAYER;
      const name = args[0] == null ? "" : String(args[0]);
      let uid = -1;
      for (const [knownUid, knownName] of session.nameByUid) if (knownName === name) uid = knownUid;
      writeNativeFrame(session.tcp, NET_PACKET.CHAT, EV.encodeChat({ uid, kind, name, text: EV.unescapeHtml(String(args[1] ?? "")) }));
      break;
    }
    case "3": {
      // g1(victimSid, deaths, killerSid, killCard, info, noFeed)
      const victimUid = toInt(args[0], -1);
      const killerSid = toInt(args[2], 0);
      const killerUid = killerSid && killerSid !== victimUid ? killerSid : -1;
      const card = Array.isArray(args[3]) ? args[3] : null;
      const info = args[4] && typeof args[4] === "object" ? (args[4] as Record<string, unknown>) : {};
      session.deathsByUid.set(victimUid, toInt(args[1]));
      if (killerUid >= 0) session.killsByUid.set(killerUid, (session.killsByUid.get(killerUid) ?? 0) + 1);
      writeNativeFrame(
        session.tcp,
        NET_PACKET.KILL,
        EV.encodeKill({
          victimUid,
          killerUid,
          weaponIndex: toInt(card ? card[0] : info.wId, -1),
          headshot: !!info.hs,
          wallbang: !!info.wb,
          fists: !!info.fs,
          victimDeaths: toInt(args[1]),
          killerKills: killerUid >= 0 ? session.killsByUid.get(killerUid) ?? 0 : 0,
          killerClass: killerUid >= 0 ? session.classByUid.get(killerUid) ?? 0 : -1,
          score: card ? toInt(card[1]) : 0,
          respawnDelay: 2.8,
        }),
      );
      break;
    }
    case "4": {
      if (args[0] == null) break; // object hit
      writeNativeFrame(
        session.tcp,
        NET_PACKET.HIT,
        EV.encodeHit({ targetUid: toInt(args[0]), damage: toNumber(args[1]), crit: !!args[2], kill: !!args[3] }),
      );
      break;
    }
    case "5": {
      if (args[1]) break; // spectated player's score
      const delta = toInt(args[0]);
      session.ownScore += delta;
      writeNativeFrame(session.tcp, NET_PACKET.SCORE, EV.encodeScore({ delta, score: session.ownScore }));
      break;
    }
    case "7": {
      const flat = Array.isArray(args[0]) ? args[0] : [];
      writeNativeFrame(session.tcp, NET_PACKET.LEADERBOARD, EV.encodeLeaderboard(boardFromFlat(session, flat, 2, 1)));
      break;
    }
    case "t": {
      const seconds = EV.parseTime(args[0]);
      if (seconds == null) break;
      writeNativeFrame(session.tcp, NET_PACKET.TIMER, EV.encodeTimer({ seconds, end: toInt(args[1]) === 1 }));
      break;
    }
    case "9": {
      // g7: [[sid, x, y, z, faceYaw?, facePitch?, trail, ...]]
      const shot = Array.isArray(args[0]) ? args[0] : null;
      if (!shot) break;
      const hasNormal = shot[4] != null && shot[5] != null;
      writeNativeFrame(
        session.tcp,
        NET_PACKET.SHOT,
        EV.encodeShot({
          uid: toInt(shot[0], -1),
          weaponIndex: session.lastStateByUid.get(toInt(shot[0], -1))?.loadoutIndex ?? -1,
          hit: [toNumber(shot[1]), toNumber(shot[2]), toNumber(shot[3])],
          hasNormal,
          normalYaw: toNumber(shot[4]),
          normalPitch: toNumber(shot[5]),
          trail: !!shot[6],
          first: true,
        }),
      );
      break;
    }
    case "pr": {
      // [x, y, z, yaw, pitch, weaponId (or stored index with args[7]), sid, isStored]
      const weaponIndex = args[7] ? 12 : toInt(args[5], -1);
      const flight = EV.PROJECTILE_FLIGHT[weaponIndex] || { speed: 0.5, gravity: 0, range: 1000 };
      const yaw = toNumber(args[3]) - Math.PI;
      const pitch = toNumber(args[4]);
      writeNativeFrame(
        session.tcp,
        NET_PACKET.PROJECTILE,
        EV.encodeProjectile({
          id: toInt(args[6], 0),
          ownerUid: -1,
          weaponIndex,
          position: [toNumber(args[0]), toNumber(args[1]), toNumber(args[2])],
          velocity: [
            flight.speed * Math.sin(yaw) * Math.cos(pitch),
            flight.speed * Math.sin(pitch),
            flight.speed * Math.cos(yaw) * Math.cos(pitch),
          ],
          gravity: flight.gravity,
          range: flight.range,
        }),
      );
      break;
    }
    case "pre": {
      if (toInt(args[1]) === 1) break; // own predicted shot counter, not a server slot id
      writeNativeFrame(
        session.tcp,
        NET_PACKET.PROJECTILE_END,
        EV.encodeProjectileEnd({ id: toInt(args[0]), position: [toNumber(args[2]), toNumber(args[3]), toNumber(args[4])], impact: !!args[5] }),
      );
      break;
    }
    case "ex":
      writeNativeFrame(
        session.tcp,
        NET_PACKET.EXPLOSION,
        EV.encodeExplosion({ position: [toNumber(args[0]), toNumber(args[1]), toNumber(args[2])], radius: toNumber(args[3]) }),
      );
      break;
    case "end": {
      // fq(spectating, didWin (-1 draw), endData, teamWinner, ...): endData.ed rows follow config.endStats.
      const result = args[1] === -1 ? -1 : args[1] ? 1 : 0;
      const endData = args[2] && typeof args[2] === "object" ? (args[2] as Record<string, unknown>) : {};
      const ed = Array.isArray(endData.ed) ? endData.ed : [];
      const board = boardFromFlat(session, ed, 6, 2);
      writeNativeFrame(session.tcp, NET_PACKET.END, EV.encodeEnd({ result, nextRound: 25, winnerUid: board[0]?.uid ?? -1, board }));
      break;
    }
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
      if (payload.length < 4 || (payload.length > EV.ENT_SIZE && payload.length !== EV.ENT_SPRAY_SIZE && payload.length !== EV.ENT_SKINS_SIZE &&
          payload.length !== EV.ENT_LAG_COMP_SIZE)) break;
      const classIndex = payload.readInt32LE(0);
      const secondary = payload.length >= 8 ? payload.readInt32LE(4) : -1;
      const c = EV.readCosmetics(payload, 8);
      const spectate = payload.length > EV.ENT_COSMETICS_SIZE && payload[EV.ENT_COSMETICS_SIZE] ? 1 : 0;
      // slidCont ("Non FPS Sliding"): absent in older native ENTs = the setting's default (on)
      const slidCont = payload.length > EV.ENT_COSMETICS_SIZE + 1 ? (payload[EV.ENT_COSMETICS_SIZE + 1] ? 1 : 0) : 1;
      // gameserver.ts ent slots: 3 hat, 4 back, 5 secIndex, 6 toggleSpect, 10 skinColIndex, 14 dye, 15 slidCont, 17 waist, 18 hairCol, 21 face
      const spray = payload.length >= EV.ENT_SPRAY_SIZE ? payload.readInt32LE(EV.ENT_SIZE) : 0; // ent[1] sprayIndex
      const skins = payload.length >= EV.ENT_SKINS_SIZE ? EV.readSkins(payload, EV.ENT_SPRAY_SIZE) : EV.SKINS_NONE; // ent[2], ent[9]
      // ent[11] lagCompensation (Settings > Network slider): V7, older native ENTs = the default 1
      const lagComp = payload.length >= EV.ENT_LAG_COMP_SIZE ? EV.lagCompFromEnt(payload.readFloatLE(EV.ENT_SKINS_SIZE)) : 1;
      const ent: unknown[] = [classIndex, spray, [skins.primary, skins.secondary], c.hat, c.back, secondary >= 0 ? secondary : null, spectate, 0, 0, skins.melee, 0, lagComp, 0, null, 0, slidCont, 0, -1, 0, 0, 0, 0];
      if (c.skinColor >= 0) ent[10] = EV.skinIndexFromColor(c.skinColor);
      if (c.dye >= 0) ent[14] = c.dye;
      ent[17] = c.waist;
      if (c.hairColor >= 0) ent[18] = EV.hairColorToKrunker(c.hairColor);
      ent[21] = c.face;
      sendUpstream(session, "ent", ent);
      break;
    }
    case NET_PACKET.INPUT: {
      const input = parseNativeInput(payload);
      if (input) sendInputToUpstream(session, input);
      break;
    }
    case NET_PACKET.INPUT_BATCH: { // one "i" per frame (the upstream accepts single frames)
      const count = payload.length ? payload[0] : 0;
      if (!count || payload.length !== 1 + count * 24) break;
      for (let i = 0; i < count; i++) {
        const input = parseNativeInput(payload.subarray(1 + i * 24, 1 + (i + 1) * 24));
        if (input) sendInputToUpstream(session, input);
      }
      break;
    }
    case NET_PACKET.CYCLE:
      if (!payload.length) sendUpstream(session, "cycle");
      break;
    case NET_PACKET.CHAT_SEND: {
      const text = EV.decodeChatSend(payload);
      if (text.trim().length) sendUpstream(session, "ct", text);
      break;
    }
    case NET_PACKET.VOICE_SEND: {
      const audio = takeVoiceChunk(session, payload);
      if (audio) sendUpstream(session, "vc", audio); // toggleRecord: io.send("vc", dataUrl)
      break;
    }
    case NET_PACKET.PONG:
      break; // upstream "pi" is answered directly in handleUpstreamPacket
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
    killsByUid: new Map(),
    deathsByUid: new Map(),
    ownScore: 0,
    pingMs: -1,
    voiceParts: [],
    voiceNext: 0,
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
