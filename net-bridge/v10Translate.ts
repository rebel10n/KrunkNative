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
//   native MODE_STATE (40) with no flag and no objective -> ts (both teams), lv, lock, gmsg
//   native OBJECT_STATE (100) -> do, ro, gte, bnk, mo, mf (one per object change)
// After the init (the v10 server sends these in this order on a live join, see the report):
//   native GLOBAL_VALUE (121) -> gv;  PLAYER_SOUND (125) -> asnd / rsnd;  TIMER (15) -> t
//   native MODE_STATE (40), no objective and no move lock -> ts, ufi, ufl (flags in play), lv, lock, gmsg
//   native OBJECT_STATE (100) -> do, ro, gte, bnk, mo, mf
//   native EXT GAME_STATE (0x0901) -> gsc;  EXT GAME_ANNOUNCEMENT (0x0302, no ranked-queue action) -> gameAnnouncement
//   native PING_RESULT (19), SOCIAL_V7 (249): consumed (v10 has no message for them)
//   native GAME_CONFIG / NETWORK_DATA / EXT RANKED_MATCH (0x8050): the init's own parts, held for the INIT;
//                           a repeat after the init with the same values is consumed, a change stops
// Stop (first untranslated native packet after the init, reported by v10Proxy):
//   native SKINS (35) -> kept for the SPAWN that follows; native SPAWN (4) of a living player -> ADD_PLAYERS
//                           "0" (one 53-field add record, v10.js 220022-220025), and our own also start
//   native LEADERBOARD (14) -> not mapped: v10's "7" rows are [sid, ...mode.leaderStats] per mode, and the
//                           FFA leaderStats key list is not verified
//   Also stops: MODE_STATE with an active objective (its time is in MODE_SYNC_V7) or a move lock (lm needs a
//   position); EXT 0x0302 with a ranked-queue action (its v10 label key is not mapped yet).
//   KN ids that have no translation at all still stop the session by name.

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
  GAME_STATE_CHANGED: "gsc",
  ADD_PLAYERS: "0",
  UPDATE_LEADERS: "7",
  START_GAME: "start",
  GAME_ANNOUNCEMENT: "gameAnnouncement",
  UPDATE_FLAG: "ufl",
  UPDATE_FLAG_INFO: "ufi",
  TEAM_SCORE: "ts",
  UPDATE_LIVES: "lv",
  LOCK_PLAYER: "lock",
  UPDATE_GAME_MESSAGE: "gmsg",
  DESTROY_GAMEOBJECT: "do",
  RESPAWN_OBJ: "ro",
  UPDATE_GATE: "gte",
  UPDATE_BANK: "bnk",
  TELEPORT_TO_NODE: "mo",
  TELEPORT_FROM_NODE: "mf",
} as const);

// KN HELLO: this bridge speaks protocol 1.4.3 (NET_PROTOCOL_MAJOR/MINOR/PATCH, patch 3: the flag uid) and Krunker v10 only
// (NET_HELLO_RULESETS = 1 << 2), flavor nibble 0 (net_hello_make in shared/src/net.c).
export const HELLO_PAYLOAD = Buffer.from([1, 4, 3, 1 << 2]);

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
  SPAWN: 4, // NET_PACKET_SPAWN: a player's state at spawn (NetSpawnPacket, protocol 1.4.3 layout)
  LEADERBOARD: 14, // NET_PACKET_LEADERBOARD: the top 16 rows, NetLeaderboardPacket
  LEADER_STATS: 240, // NET_PACKET_LEADER_STATS: the same board right after LEADERBOARD, with the mode's leaderStats values
  AI_ADD: 90, // NET_PACKET_AI_ADD: the v3 encoding of a bot; a protocol 1.3+ peer gets AI_ADD_V7 as well (ai.c 2124-2131)
  AI_SYNC: 91, // NET_PACKET_AI_SYNC: the v3 encoding of the bots' positions; a 1.3+ peer gets AI_SYNC_V7 (161)
  AI_SYNC_V7: 161, // NET_PACKET_AI_SYNC_V7: the v7 positions (stride 9, v10's "ai" entries)
  AI_ADD_V7: 162, // NET_PACKET_AI_ADD_V7: the v7 record of a bot (AI.getData v7), the one v10's "aai" record is built from
  SKINS: 35, // NET_PACKET_SKINS: the weapon and melee skins, right before the SPAWN (NetSkinsPacket)
  SOCIAL_V7: 249, // NET_PACKET_SOCIAL_V7: the v7 chat / voice / spray ids this server takes (a capability notice)
  PING_RESULT: 19, // NET_PACKET_PING_RESULT: the smoothed ping, the answer to the bridge's PONG (v10 "pir" is a no-op)
  MODE_STATE: 40, // NET_PACKET_MODE_STATE: the joiner's mode snapshot (NetModeStatePacket)
  OBJECT_STATE: 100, // NET_PACKET_OBJECT_STATE: one map object's destroy / respawn / gate / bank / move
  MODE_SYNC_V7: 149, // NET_PACKET_MODE_SYNC_V7: team scores, rounds, lockout, objective time (joiners)
  EXT: 254,
} as const);
export const EXT_GAME_STATE = 0x0901; // NET_EXT_GAME_STATE, v10 "gsc"
export const EXT_GAME_ANNOUNCEMENT = 0x0302; // NET_EXT_GAME_ANNOUNCEMENT: v10 gameAnnouncement {message, actions}
export const EXT_RANKED_MATCH = 0x8050; // NET_EXT_KN_RANKED_MATCH: 1 in a ranked match; the init's isRanked (argument 16)

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

// ---- MODE_STATE and OBJECT_STATE: the mode snapshot and the object list a joiner gets after the INIT.
// Each maps to the v10 messages the server sends for the same change (v10.js send sites: ts 191530,
// lv 174052, lock 188300, gmsg 191369 / 191419, do 158238, ro 158290, gte 162647, bnk 162702,
// mo 187867, mf 187937). The handler parameters are the same as the join's sync-data use (v10.js 219045).

export type V10Message = [label: string, args: unknown[]];
export type Mapped = { messages: V10Message[]; stop?: string };

const MODE_STATE_SIZE = 387; // NetModeStatePacket: MODE_MAX_FLAGS 8 x NetModeFlag (protocol 1.4.3), NET_MODE_TEXT_SIZE 32
const MODE_FLAG_SIZE = 25; // NetModeFlag: float position[3], int32 carrier_uid, team, uid, uint8 active
const MODE_FLAGS_AT = 91;
const MODE_MESSAGE_AT = 291;
const OBJECT_STATE_SIZE = 9; // NetObjectStatePacket: int32 uid, uint8 kind, int32 value

function cText(b: Buffer): string {
  const nul = b.indexOf(0);
  return b.subarray(0, nul < 0 ? b.length : nul).toString("utf8");
}

// NetModeStatePacket (little endian): mode_id @0, team_scores[2] @4, flag_info[2][32] @12,
// active_objective @76, objective_weapon @80, move_lock @84, locked @85, lives @86, flag_count @90,
// flags[8] (25 bytes each) @91, message[96] @291.
// A flag in play (active) becomes ufl(uid, x, y, z, carrier || 0), the same arguments the v10 server
// sends (v10.js 188667); the team lines go first as ufi(team, text) (v10.js 188698). Flags not in play
// are not sent: v10 has no message for them, and the client keeps them at their map position.
export function mapModeState(p: Buffer): Mapped {
  if (p.length !== MODE_STATE_SIZE) return { messages: [], stop: "malformed" };
  if (p.readInt32LE(76) >= 0) return { messages: [], stop: "an active objective: v10 obj needs its time, which is in MODE_SYNC_V7" };
  if (p[84] !== 0) return { messages: [], stop: "move lock: v10 lm needs the player's position" };

  const messages: V10Message[] = [
    [V10_IN.TEAM_SCORE, [1, p.readInt32LE(4)]], // team 1 (mode team_scores[0])
    [V10_IN.TEAM_SCORE, [2, p.readInt32LE(8)]], // team 2 (mode team_scores[1])
  ];
  const flags: V10Message[] = [];
  const count = Math.min(p[90], 8);
  for (let f = 0; f < count; f++) {
    const at = MODE_FLAGS_AT + f * MODE_FLAG_SIZE;
    if (p[at + 24] === 0) continue; // not in play for this mode
    const uid = p.readInt32LE(at + 20);
    if (uid < 0) return { messages: [], stop: "an active flag with no map object id" };
    const carrier = p.readInt32LE(at + 12);
    flags.push([V10_IN.UPDATE_FLAG, [uid, p.readFloatLE(at), p.readFloatLE(at + 4), p.readFloatLE(at + 8), carrier >= 0 ? carrier : 0]]);
  }
  if (flags.length) {
    messages.push([V10_IN.UPDATE_FLAG_INFO, [1, cText(p.subarray(12, 44))]], [V10_IN.UPDATE_FLAG_INFO, [2, cText(p.subarray(44, 76))]]);
    messages.push(...flags);
  }
  const lives = p.readInt32LE(86);
  if (lives >= 0) messages.push([V10_IN.UPDATE_LIVES, [lives]]);
  if (p[85] !== 0) messages.push([V10_IN.LOCK_PLAYER, [true]]);
  const message = cText(p.subarray(MODE_MESSAGE_AT, MODE_MESSAGE_AT + 96));
  if (message.length) messages.push([V10_IN.UPDATE_GAME_MESSAGE, [message]]); // "wt" is the v10 waiting text too
  return { messages };
}

// NetObjectStatePacket kinds (shared.h NetObjectStateKind) -> the v10 label of the same change.
export function mapObjectState(p: Buffer): Mapped {
  if (p.length !== OBJECT_STATE_SIZE) return { messages: [], stop: "malformed" };
  const uid = p.readInt32LE(0);
  const kind = p[4];
  const value = p.readInt32LE(5);
  switch (kind) {
    case 0:
      return { messages: [[V10_IN.DESTROY_GAMEOBJECT, [uid]]] };
    case 1:
      return { messages: [[V10_IN.RESPAWN_OBJ, [uid]]] };
    case 2:
      return { messages: [[V10_IN.UPDATE_GATE, [uid, value !== 0]]] };
    case 3:
      return { messages: [[V10_IN.UPDATE_BANK, [uid, value]]] };
    case 4:
      return { messages: [[V10_IN.TELEPORT_TO_NODE, [uid, value]]] };
    case 5:
      return { messages: [[V10_IN.TELEPORT_FROM_NODE, [uid]]] };
    default:
      return { messages: [], stop: `object kind ${kind} is not mapped` };
  }
}

// NetGameAnnouncementPacket (the body after the EXT subtype): char message[256], uint8 ranked_queue,
// char ranked_label[64]. v10's card reads message and actions; the ranked-queue action's label key is
// not mapped yet, so a card with that action is not translated.
export function mapGameAnnouncement(body: Buffer): Mapped {
  if (body.length !== 256 + 1 + 64) return { messages: [], stop: "malformed" };
  const message = cText(body.subarray(0, 256));
  if (body[256] !== 0) return { messages: [], stop: "the ranked-queue action: its v10 label key is not mapped yet" };
  return { messages: [[V10_IN.GAME_ANNOUNCEMENT, [{ message, actions: [] }]]] };
}

// ---- SKINS and SPAWN: a player's spawn, as the v10 add record (ADD_PLAYERS "0") and the start of ours.

// NetSkinsPacket: int32 uid, PlayerSkins {primary, secondary, melee} (int32 each).
export type NativeSkins = { uid: number; primary: number; secondary: number; melee: number };
export function parseSkins(p: Buffer): NativeSkins | null {
  if (p.length !== 16) return null;
  return { uid: p.readInt32LE(0), primary: p.readInt32LE(4), secondary: p.readInt32LE(8), melee: p.readInt32LE(12) };
}

// NetSpawnPacket with the protocol 1.3.1 WP-C and WP-F tails (150 bytes): uid @0, is_you @4, active @5,
// class_index @6, team @10, health @14 (float), max_health @18, position @22 (3 floats), direction @34
// (2 floats), name @42 (32), secondary_index @74, PlayerCosmetics @78 (skin colour, hair colour, hat, body,
// waist, face, dye: 7 int32), attach_index @106, head_mlt @110 (float), PlayerCosmeticsV7 @114 (head, back,
// shoe, pet, wrist, charms[2], player_card, badge: 9 int32).
export type NativeSpawn = {
  uid: number;
  isYou: boolean;
  active: boolean;
  classIndex: number;
  team: number;
  health: number;
  maxHealth: number;
  position: [number, number, number];
  direction: [number, number];
  name: string;
  secondaryIndex: number;
  cosmetics: { skinColor: number; hairColor: number; hat: number; body: number; waist: number; face: number; dye: number };
  attachIndex: number;
  headMlt: number;
  v7: { head: number; back: number; shoe: number; pet: number; wrist: number; charms: [number, number]; playerCard: number; badge: number };
};
const SPAWN_SIZE = 150;
export function parseSpawn(p: Buffer): NativeSpawn | null {
  if (p.length !== SPAWN_SIZE) return null;
  const nul = p.indexOf(0, 42);
  const v = (at: number) => p.readInt32LE(at);
  return {
    uid: v(0),
    isYou: p[4] !== 0,
    active: p[5] !== 0,
    classIndex: v(6),
    team: v(10),
    health: p.readFloatLE(14),
    maxHealth: v(18),
    position: [p.readFloatLE(22), p.readFloatLE(26), p.readFloatLE(30)],
    direction: [p.readFloatLE(34), p.readFloatLE(38)],
    name: p.subarray(42, nul < 0 || nul > 74 ? 74 : nul).toString("utf8"),
    secondaryIndex: v(74),
    cosmetics: {
      skinColor: v(78),
      hairColor: v(82),
      hat: v(86),
      body: v(90),
      waist: v(94),
      face: v(98),
      dye: v(102),
    },
    attachIndex: v(106),
    headMlt: p.readFloatLE(110),
    v7: {
      head: v(114),
      back: v(118),
      shoe: v(122),
      pet: v(126),
      wrist: v(130),
      charms: [v(134), v(138)],
      playerCard: v(142),
      badge: v(146),
    },
  };
}

// The v10 add record (v10.js getPlayerAddData, 53 fields, read by the client's ADD_PLAYERS handler at
// 220022-220025). Our own player's id is the socket id the bridge sent in io-init (the client compares it).
// Fields KN does not send are 0 (level, clan, featured, upgrades and so on), and the ones the server reads
// from the account are -1 (badge, title, prop, pc stat).
export const ADD_RECORD_FIELDS = 53;
export function buildPlayerAdd(spawn: NativeSpawn, skins: NativeSkins | undefined, socketId: string): unknown[] {
  const r = (n: number) => Math.round(n);
  const pair = skins ? [skins.primary, skins.secondary] : [-1, -1];
  const skinColor = spawn.cosmetics.skinColor;
  return [
    spawn.isYou ? socketId : String(spawn.uid), // 0 id
    spawn.uid, // 1 sid
    r(spawn.position[0]), // 2 x
    r(spawn.position[1]), // 3 y
    r(spawn.position[2]), // 4 z
    spawn.name, // 5 name
    spawn.classIndex, // 6 classIndex
    r(spawn.health), // 7 health
    r(spawn.maxHealth), // 8 maxHealth
    spawn.team, // 9 team
    0, // 10 level (not in SPAWN)
    0, // 11 clan
    pair, // 12 skins [primary, secondary]
    spawn.cosmetics.hat, // 13 hatIndex
    spawn.cosmetics.body, // 14 bodyIndex (the v7 ent[4] slot)
    spawn.secondaryIndex >= 0 ? spawn.secondaryIndex : null, // 15 secIndex
    0, // 16 hack
    null, // 17 customLoadout
    spawn.direction[1], // 18 xDire (the yaw; proxy.ts reads direction[1] the same way)
    skins ? skins.melee : -1, // 19 meleeIndex
    skinColor < 0 ? -1 : EV.skinIndexFromColor(skinColor), // 20 skinColIndex
    -1, // 21 propID
    spawn.attachIndex, // 22 attachIndex
    -1, // 23 pcStatIndex
    spawn.cosmetics.dye, // 24 dyeIndex
    0, // 25 featured
    -1, // 26 flg
    0, // 27 premium
    spawn.name, // 28 getName
    spawn.v7.shoe, // 29 shoeIndex
    spawn.cosmetics.waist, // 30 waistIndex
    0, // 31 account id
    spawn.cosmetics.hairColor < 0 ? 0 : EV.hairColorToKrunker(spawn.cosmetics.hairColor), // 32 hairCol
    spawn.cosmetics.face, // 33 faceIndex
    spawn.v7.pet, // 34 petIndex
    spawn.v7.badge, // 35 custom badge
    spawn.v7.wrist, // 36 wristIndex
    0, // 37 isJuggernaut
    spawn.headMlt, // 38 headMlt
    spawn.v7.charms, // 39 charms
    0, // 40 upgrades
    spawn.v7.back, // 41 backIndex
    spawn.v7.head, // 42 headIndex
    spawn.v7.playerCard, // 43 playerCardIndex
    0, // 44 isBot
    0, // 45 isBusy
    0, // 46 rank
    0, // 47 clanId
    0, // 48 clanLogo
    0, // 49 clan
    0, // 50 clanBannerColor
    0, // 51 verified
    -1, // 52 titleIndex
  ];
}

// START_GAME ("start") for our own spawn, with the arguments the v10 server sends (v10.js 69423) and the
// handler reads (v10.js 218695): gameTimer, team, spectating, challMode, moveLock, 1, null, classIndex.
export function buildStartGame(spawn: NativeSpawn, gameTimer: number, challMode: number): unknown[] {
  return [gameTimer, spawn.team, 0, challMode, 0, 1, null, spawn.classIndex];
}

// ---- LEADERBOARD and AI_ADD

// NetLeaderboardPacket: uint8 count, then 16 rows of int32 uid, score, kills, deaths (257 bytes).
// v10's "7" (UPDATE_LEADERS, v10.js 173566) is a flat list of [sid, ...mode.leaderStats] per row; the
// client's handler reads rows of leaderStats.length + 1 (v10.js 220483). Free for All's leaderStats is
// ["score", "kills", "deaths"] (v10.js 105417, mode id "ffa"), the same fields and order as the row, and the
// v10 transforms ("downs", "points") are for other keys, so the values pass through. KN's FFA mode id is 0.
const LEADERBOARD_SIZE = 1 + 16 * 16;
const MODE_ID_FFA = 0;
export function mapLeaderboard(p: Buffer, modeIndex: number): Mapped {
  if (p.length !== LEADERBOARD_SIZE) return { messages: [], stop: "malformed" };
  if (modeIndex !== MODE_ID_FFA) return { messages: [], stop: "leaderStats of this mode are not verified (only Free for All)" };
  const rows: number[] = [];
  const count = Math.min(p[0], 16);
  for (let i = 0; i < count; i++) {
    const at = 1 + i * 16;
    rows.push(p.readInt32LE(at), p.readInt32LE(at + 4), p.readInt32LE(at + 8), p.readInt32LE(at + 12));
  }
  return { messages: [[V10_IN.UPDATE_LEADERS, [rows]]] };
}

// NetLeaderStatsPacket: uint8 count, uint8 stat_count, int32 uid[16], int32 values[16][4] (322 bytes). After a
// LEADERBOARD it repeats the rows; for free for all (3 stats) the rows are the ones already sent as "7", so the
// packet is consumed when they match, and stops when they differ.
const LEADER_STATS_SIZE = 2 + 16 * 4 + 16 * 4 * 4;
export function checkLeaderStats(p: Buffer, rows: number[] | null): string | null {
  if (p.length !== LEADER_STATS_SIZE) return "malformed";
  if (!rows) return "no LEADERBOARD before it";
  const count = Math.min(p[0], 16);
  if (p[1] !== 3) return "leaderStats of this mode are not verified (only Free for All)";
  if (rows.length !== count * 4) return "the rows differ from the LEADERBOARD";
  for (let i = 0; i < count; i++) {
    const uid = p.readInt32LE(2 + i * 4);
    if (uid !== rows[i * 4]) return "the rows differ from the LEADERBOARD";
    for (let k = 0; k < 3; k++) {
      if (p.readInt32LE(2 + 16 * 4 + (i * 4 + k) * 4) !== rows[i * 4 + 1 + k]) return "the values differ from the LEADERBOARD";
    }
  }
  return null;
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
  ranked: boolean,
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
    ranked,
    null,
    null,
    null,
  ];
}
