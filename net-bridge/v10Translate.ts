"use strict";

// Krunker v10.0.x client packets -> KrunkNative native (knet TCP) packets, pure functions.
// The socket side is v10Proxy.ts; this file has no network code so the layouts can be unit tested.
//
// v10 side: msgpack `[label, ...args]` + a 2-byte ahNum trailer (v10.js _9203 / _9210).
// Native side: shared/include/shared.h NET_PACKET_* frames, [type u8][len u16 BE][payload].
//
// Translated so far (see v10Proxy.ts for the order they run in):
//   load (LOAD)           -> HELLO (1)         the client is ready for INIT
//   en   (ENTER_GAME)     -> ENT (3), V9 form  the join: class, loadout, cosmetics, lag comp, badges
//   ct   (CHAT_MESSAGE)   -> CHAT_SEND (8)
//   native PING (17)      -> PONG (18)         the native probe, echoed verbatim
//   native MAP_INFO (25)  -> MAP_REQUEST (26)  the bridge never has the map: it asks by hash
//   native MAP_DATA (27)  -> reassembled JSON text, the map object in init argument 9
//   native GAME_CONFIG (183) -> the settings object, init argument 6
//   native NETWORK_DATA (182) -> the networking object, init argument 12
//   native INIT (2)       -> init (INIT_GAME), 20 arguments as the v10 server sends them
//                           (v10.js 222270); held until the map JSON is whole
//   native GLOBAL_VALUE (121) -> gv (key, value); a reset is a no-op before the first gv
//   native PLAYER_SOUND (125) -> asnd (add) / rsnd (remove)
//   native TIMER (15)     -> t (seconds, end)
// Stop (first untranslated native packet after the init, reported by v10Proxy):
//   native MODE_STATE (40), the full mode snapshot (NetModeStatePacket) -> the v10 mode labels;
//   not mapped yet. See doc/v10/net.md and the report.

import * as EV from "./nativeEvents";

export const NATIVE = Object.freeze({
  HELLO: 1,
  INIT: 2,
  ENT: 3,
  CHAT_SEND: EV.NET_EVENT.CHAT_SEND,
  PING: EV.NET_EVENT.PING,
  PONG: EV.NET_EVENT.PONG,
} as const);

// v10 labels. Outgoing = client -> server (v10.js _33976), incoming = server -> client (_33977).
export const V10_OUT = Object.freeze({
  LOAD: "load",
  ENTER_GAME: "en",
  CHAT_MESSAGE: "ct",
  PING: "po",
  PONG: "v",
} as const);
export const V10_IN = Object.freeze({
  IO_INIT: "io-init",
  INIT_LOAD: "load",
  GAME_INSTANCE_ID: "inst-id",
  INIT_GAME: "init",
  UPDATE_GLOBAL_VALUE: "gv",
  ADD_PLAYER_SOUND: "asnd",
  REMOVE_PLAYER_SOUND: "rsnd",
  UPDATE_TIMER: "t",
} as const);

// KN HELLO: this bridge speaks protocol 1.4.2 (NET_PROTOCOL_MAJOR/MINOR/PATCH) and Krunker v10 only
// (NET_HELLO_RULESETS = 1 << 2), flavor nibble 0 (net_hello_make in shared/src/net.c).
export const HELLO_PAYLOAD = Buffer.from([1, 4, 2, 1 << 2]);

// The v10 ENTER_GAME ("en") array, positions as the client builds them (v10.js 208129, 35 entries).
// Cosmetic positions are randomised through _69645(null, type): the type id is in the comment.
export const EN = Object.freeze({
  CLASS: 0,
  SPRAY: 1,
  SKINS: 2, // [primary, secondary] weapon skins
  HAT: 3, // type 1
  BODY: 4, // type 2 (KN: PlayerCosmetics.body)
  SECONDARY: 5,
  SPECTATE: 6,
  CHALLENGE: 7, // settings challMode
  MELEE: 9, // type 3, weapon skin of the melee
  SKIN_COLOR: 10, // skinColIndex
  LAG_COMP: 11,
  ATTACH: 12, // attachIndex, -1 none
  PC_STAT: 13,
  DYE: 14, // type 5
  SHOE: 15, // type 8
  WAIST: 16, // type 6
  HAIR: 17, // hairCol
  ANON: 19, // settings anonMode
  FACE: 20, // type 7
  PET: 21, // type 9
  VERIFIED: 22,
  PREMIUM: 23,
  CUSTOM_BADGE: 24,
  WRIST: 25, // type 11
  CHARMS: 29, // type 12/13, [primary, secondary]
  BACK: 30, // type 14
  HEAD: 31, // type 15
  PLAYER_CARD: 32, // type 16
} as const);
export const EN_LENGTH = 35;

// NetEntPacket (shared.h): V7 (lagComp) is the EV.ENT_LAG_COMP_SIZE prefix. V8 adds attach (int32),
// challMode and anonMode (u8 each); V9 adds PlayerCosmeticsV7 (head, back, shoe, pet, wrist, charms[2],
// player_card, badge: 9 int32 = 36 bytes) and show_badges (u8). Sent in full: the packed layout is
// read as a prefix by every reader.
const ENT_V8_ATTACH = EV.ENT_LAG_COMP_SIZE;
const ENT_V8_CHALL = ENT_V8_ATTACH + 4;
const ENT_V8_ANON = ENT_V8_CHALL + 1;
const ENT_V9_COSMETICS = ENT_V8_ANON + 1;
const ENT_V9_SHOW_BADGES = ENT_V9_COSMETICS + 36;
export const ENT_V9_SIZE = ENT_V9_SHOW_BADGES + 1;

function storeIndex(value: unknown): number {
  const n = Number(value);
  return Number.isInteger(n) && n >= 0 ? n : -1;
}

function int32(value: unknown, fallback: number): number {
  const n = Number(value);
  return Number.isFinite(n) ? n | 0 : fallback;
}

function flag(value: unknown): number {
  return value ? 1 : 0;
}

// The KN ENT (V9) for an "en" array. Returns null when the array is not the v10 shape.
export function encodeEnter(en: unknown): Buffer | null {
  if (!Array.isArray(en) || en.length < EN.SHOE + 1) return null;
  const out = Buffer.alloc(ENT_V9_SIZE);
  out.writeInt32LE(int32(en[EN.CLASS], 0), 0);
  out.writeInt32LE(int32(en[EN.SECONDARY], -1), 4);

  // EV.cosmeticsFromEnt reads the cosmetic slots of the KN ENT array (its doc: ent[3] hat, ent[4] body,
  // ent[10] skin colour, ent[14] dye, ent[17] waist, ent[18] hair, ent[21] face).
  const slots: unknown[] = [];
  slots[3] = en[EN.HAT];
  slots[4] = en[EN.BODY];
  slots[10] = en[EN.SKIN_COLOR];
  slots[14] = en[EN.DYE];
  slots[17] = en[EN.WAIST];
  slots[18] = en[EN.HAIR];
  slots[21] = en[EN.FACE];
  EV.writeCosmetics(out, 8, EV.cosmeticsFromEnt(slots));

  out[EV.ENT_COSMETICS_SIZE] = flag(en[EN.SPECTATE]);
  out[EV.ENT_COSMETICS_SIZE + 1] = 1; // slidCont: not in the v10 array, the setting's default
  out.writeInt32LE(int32(en[EN.SPRAY], 0), EV.ENT_SIZE);
  EV.writeSkins(out, EV.ENT_SPRAY_SIZE, EV.skinsFromKrunker(en[EN.SKINS], en[EN.MELEE]));
  out.writeFloatLE(EV.lagCompFromEnt(en[EN.LAG_COMP]), EV.ENT_SKINS_SIZE);

  out.writeInt32LE(int32(en[EN.ATTACH], -1), ENT_V8_ATTACH);
  out[ENT_V8_CHALL] = flag(en[EN.CHALLENGE]);
  out[ENT_V8_ANON] = flag(en[EN.ANON]);

  const charms = Array.isArray(en[EN.CHARMS]) ? (en[EN.CHARMS] as unknown[]) : [];
  const v7 = [
    storeIndex(en[EN.HEAD]), // head
    storeIndex(en[EN.BACK]), // back
    storeIndex(en[EN.SHOE]), // shoe
    storeIndex(en[EN.PET]), // pet
    storeIndex(en[EN.WRIST]), // wrist
    storeIndex(charms[0]), // charms[0]
    storeIndex(charms[1]), // charms[1]
    storeIndex(en[EN.PLAYER_CARD]), // player_card
    -1, // badge: the server takes the account's
  ];
  v7.forEach((v, i) => out.writeInt32LE(v, ENT_V9_COSMETICS + i * 4));
  out[ENT_V9_SHOW_BADGES] =
    (flag(en[EN.VERIFIED]) ? 0x01 : 0) | (flag(en[EN.PREMIUM]) ? 0x02 : 0) | (flag(en[EN.CUSTOM_BADGE]) ? 0x08 : 0);
  return out;
}

// KN CHAT_SEND for a "ct" text; null for a blank line (the v10 client never sends one, but KN drops it too).
export function encodeChat(text: unknown): Buffer | null {
  if (typeof text !== "string" || !text.trim().length) return null;
  return EV.encodeChatSend(text);
}

// ---- native -> v10: the map, config and network data that come before the INIT

export const NATIVE_MORE = Object.freeze({
  MAP_INFO: 25,
  MAP_REQUEST: 26,
  MAP_DATA: 27,
  BILLBOARD: 33,
  BILLBOARD_IMAGE: 36,
  NETWORK_DATA: 182, // NET_PACKET_NETWORK_DATA (LOBBY + 2)
  GAME_CONFIG: 183, // NET_PACKET_GAME_CONFIG (LOBBY + 3)
  GLOBAL_VALUE: 121, // NET_PACKET_GLOBAL_VALUE (TRIGGERS + 1): the join's global values, v10 "gv"
  PLAYER_SOUND: 125, // NET_PACKET_PLAYER_SOUND (TRIGGERS + 5): add ("asnd") or remove ("rsnd") a loop
  TIMER: 15, // NET_PACKET_TIMER: seconds left and the end-screen flag, v10 "t"
  EXT: 254,
} as const);
export const EXT_GAME_STATE = 0x0901; // NET_EXT_GAME_STATE, v10 "gsc"

const MAP_INFO_SIZE = 44; // NetMapInfoPacket: int32 map_index, uint32 hash, uint32 size, char name[32]
const MAP_DATA_HEADER = 10; // NetMapDataPacket: uint32 hash, uint32 offset, uint16 length
const GAME_CONFIG_HEADER = 8; // NetGameConfigChunk: uint32 total, uint32 offset
const NETWORK_DATA_SIZE = 10; // NetNetworkDataPacket: uint16 send_hz, tick_hz, sync_hz, float view_distance
const INIT_SIZE_V1 = 17; // NetInitPacket without the ruleset byte

export type NativeMapInfo = { mapIndex: number; hash: number; size: number; name: string };

export function parseMapInfo(p: Buffer): NativeMapInfo | null {
  if (p.length !== MAP_INFO_SIZE) return null;
  const nul = p.indexOf(0, 12);
  return {
    mapIndex: p.readInt32LE(0),
    hash: p.readUInt32LE(4),
    size: p.readUInt32LE(8),
    name: p.subarray(12, nul < 0 ? MAP_INFO_SIZE : nul).toString("utf8"),
  };
}

// NET_PACKET_MAP_REQUEST: the hash of the map JSON the client lacks (net_handle_map_request).
export function encodeMapRequest(hash: number): Buffer {
  const out = Buffer.alloc(4);
  out.writeUInt32LE(hash >>> 0, 0);
  return out;
}

export function parseMapData(p: Buffer): { hash: number; offset: number; data: Buffer } | null {
  if (p.length < MAP_DATA_HEADER) return null;
  if (p.length !== MAP_DATA_HEADER + p.readUInt16LE(8)) return null;
  return { hash: p.readUInt32LE(0), offset: p.readUInt32LE(4), data: p.subarray(MAP_DATA_HEADER) };
}

export function parseGameConfigChunk(p: Buffer): { total: number; offset: number; data: Buffer } | null {
  if (p.length < GAME_CONFIG_HEADER) return null;
  return { total: p.readUInt32LE(0), offset: p.readUInt32LE(4), data: p.subarray(GAME_CONFIG_HEADER) };
}

// v10 applyNetworking reads snf (sendFrequency), tf (tickFrequency), sf (syncFrequency).
export function parseNetworkData(p: Buffer): Record<string, number> | null {
  if (p.length !== NETWORK_DATA_SIZE) return null;
  return { snf: p.readUInt16LE(0), tf: p.readUInt16LE(2), sf: p.readUInt16LE(4) };
}

// NetValuePacket: uint8 flags, char key[TRIGGER_VALUE_KEY_SIZE] (TEXT_MAX 70 + NUL), double value.
const VALUE_KEY_SIZE = 71;
const VALUE_RESET = 0x01; // NET_VALUE_RESET: clear the client's set first, which v10 "gv" cannot do

export function parseGlobalValue(p: Buffer): { reset: boolean; key: string; value: number } | null {
  if (p.length !== 1 + VALUE_KEY_SIZE + 8) return null;
  const nul = p.indexOf(0, 1);
  const end = nul < 0 || nul > VALUE_KEY_SIZE ? 1 + VALUE_KEY_SIZE : nul;
  return { reset: (p[0] & VALUE_RESET) !== 0, key: p.subarray(1, end).toString("utf8"), value: p.readDoubleLE(1 + VALUE_KEY_SIZE) };
}

// NetPlayerSoundPacket: int32 uid, uint8 add, force, loop, float volume, rate, char name[24].
const SOUND_NAME_SIZE = 24;
export function parsePlayerSound(p: Buffer): { uid: number; add: boolean; force: boolean; loop: boolean; volume: number; rate: number; name: string } | null {
  if (p.length !== 15 + SOUND_NAME_SIZE) return null;
  const nul = p.indexOf(0, 15);
  return {
    uid: p.readInt32LE(0),
    add: p[4] !== 0,
    force: p[5] !== 0,
    loop: p[6] !== 0,
    volume: p.readFloatLE(7),
    rate: p.readFloatLE(11),
    name: p.subarray(15, nul < 0 || nul > 15 + SOUND_NAME_SIZE ? 15 + SOUND_NAME_SIZE : nul).toString("utf8"),
  };
}

// NetTimerPacket: int32 seconds, uint8 end (1: the end-screen countdown to the next round).
export function parseTimer(p: Buffer): { seconds: number; end: boolean } | null {
  if (p.length !== 5) return null;
  return { seconds: p.readInt32LE(0), end: p[4] !== 0 };
}

export type NativeInit = { mapIndex: number; modeIndex: number; tickRate: number; clientId: number; ruleset: number };

export function parseInit(p: Buffer): NativeInit | null {
  if (p.length < INIT_SIZE_V1) return null;
  return {
    mapIndex: p.readInt32LE(3),
    modeIndex: p.readInt32LE(7),
    tickRate: p.readUInt16LE(11),
    clientId: p.readInt32LE(13),
    ruleset: p.length > INIT_SIZE_V1 ? p[INIT_SIZE_V1] : 0,
  };
}

// Chunks of one JSON text, in order (the server streams them in order over TCP). add() refuses an
// out-of-order or overlong chunk; the caller stops the translation on that.
export class ChunkAssembly {
  private readonly buf: Buffer;
  private end = 0;

  constructor(readonly total: number) {
    this.buf = Buffer.alloc(total);
  }

  add(offset: number, data: Buffer): boolean {
    if (offset !== this.end || offset + data.length > this.total) return false;
    data.copy(this.buf, offset);
    this.end += data.length;
    return true;
  }

  get complete(): boolean {
    return this.end === this.total;
  }

  text(): string {
    return this.buf.toString("utf8");
  }
}

// The v10 config object (init argument 6) from the GAME_CONFIG JSON: KN writes the settings under the
// v10 config variable names (shared/src/game_config.c game_config_write), plus the class list.
export function buildConfig(text: string): Record<string, unknown> {
  const msg = JSON.parse(text) as { settings?: unknown; classes?: unknown };
  const settings = msg && typeof msg.settings === "object" && msg.settings ? msg.settings : {};
  const config: Record<string, unknown> = { ...(settings as Record<string, unknown>) };
  if (Array.isArray(msg.classes)) config.classes = msg.classes;
  return config;
}

// The v10 INIT_GAME arguments, positions as the v10 server broadcasts them (v10.js 222270, 20 values)
// and as the client's handler reads them (v10.js 219045 INIT_GAME, 219055 _72811):
//   0 mapIndex, 1 modeIndex, 2 eventIndex, 3-4 team scores (not in INIT: null), 5 host, 6 config,
//   7 customData, 8 modURL, 9 customMapData (the map: `data` is the JSON text, so the client does not
//   fetch /maps/<id>), 10 mapHashes, 11 sync data, 12 networkData, 13 init's 4th parameter, 14 isOfficial,
//   15 isComp, 16 isRanked, 17 sessionId, 18 roundId, 19 gameTimer.
// Arguments 20 (overrideScripts) and 21 (gameState) are not in the server's list: the GAME_STATE EXT
// that follows the INIT carries the state (not translated yet).
export function buildInitArgs(
  init: NativeInit,
  mapText: string,
  config: Record<string, unknown>,
  networkData: Record<string, number>,
): unknown[] {
  return [
    init.mapIndex,
    init.modeIndex,
    0,
    null,
    null,
    null,
    config,
    null,
    0,
    { data: mapText, id: null, creator: "", info: {}, accType: 1 },
    null,
    {},
    networkData,
    true,
    false,
    false,
    false,
    null,
    null,
    null,
  ];
}
