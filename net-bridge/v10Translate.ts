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
// Stopped at (first untranslated, reported by v10Proxy):
//   native INIT (2)       -> v10 init (INIT_GAME). The v10 INIT_GAME handler needs the map: an official
//                           map is fetched by id from the REST maps API (`/maps/<id>`, v10.js 26585,
//                           219048) unless arg 9 carries the map object (`data`). The native map
//                           stream (MAP_INFO 25 / MAP_DATA 27) is not reassembled, and the 35-argument
//                           INIT_GAME list is not built. See doc/v10/net.md and the report.

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
