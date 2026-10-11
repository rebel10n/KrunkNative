"use strict";

// Match-event packets of the KrunkNative TCP protocol (shared/include/shared.h,
// NET_PACKET_CHAT_SEND .. NET_PACKET_LEAVE). Every struct is #pragma pack(1),
// little endian; strings are fixed-size NUL-terminated UTF-8. Used by proxy.ts
// (browser Krunker client <-> native server) and krunkerProxy.ts (native client
// <-> Krunker server) to translate them to/from the Krunker v3.1.1 labels.

export const NET_EVENT = Object.freeze({
  CHAT_SEND: 8, // C->S "ct"
  CHAT: 9, // S->C "ch"
  KILL: 10, // S->C "3"
  HIT: 11, // S->C "4"
  HEALTH: 12, // S->C "h"
  SCORE: 13, // S->C "5"
  LEADERBOARD: 14, // S->C "7"
  TIMER: 15, // S->C "t"
  END: 16, // S->C "end"
  PING: 17, // S->C "pi"
  PONG: 18, // C->S "po"
  PING_RESULT: 19, // S->C ping field of "l"
  LEAVE: 20, // S->C "2"
} as const);

export const NET_CHAT_KIND = Object.freeze({ PLAYER: 0, GAME: 1, SERVER: 2 } as const);

export const PLAYER_NAME_SIZE = 32;
export const NET_CHAT_TEXT_SIZE = 288;
export const NET_CHAT_MAX_CHARS = 70;
export const LEADERBOARD_MAX = 16;

const LEADERBOARD_SIZE = 1 + LEADERBOARD_MAX * 16;

export type ChatPacket = { uid: number; kind: number; name: string; text: string };
export type KillPacket = {
  victimUid: number;
  killerUid: number;
  weaponIndex: number;
  headshot: boolean;
  wallbang: boolean;
  fists: boolean;
  victimDeaths: number;
  killerKills: number;
  killerClass: number;
  score: number;
  respawnDelay: number;
};
export type HitPacket = { targetUid: number; damage: number; crit: boolean; kill: boolean };
export type HealthPacket = { uid: number; health: number; hasSource: boolean; sourceX: number; sourceZ: number };
export type ScorePacket = { delta: number; score: number };
export type LeaderboardEntry = { uid: number; score: number; kills: number; deaths: number };
export type TimerPacket = { seconds: number; end: boolean };
export type EndPacket = { result: number; nextRound: number; winnerUid: number; board: LeaderboardEntry[] };

function readCString(buffer: Buffer, offset: number, length: number): string {
  const slice = buffer.subarray(offset, Math.min(buffer.length, offset + length));
  const end = slice.indexOf(0);
  return slice.toString("utf8", 0, end >= 0 ? end : slice.length);
}

function writeCString(buffer: Buffer, offset: number, length: number, value: string): void {
  const bytes = Buffer.from(value || "", "utf8");
  let n = Math.min(bytes.length, length - 1);
  while (n > 0 && (bytes[n] & 0xc0) === 0x80) n--; // never cut a UTF-8 sequence
  bytes.copy(buffer, offset, 0, n);
}

// "MM:SS" like UTILS.getTime.
export function formatTime(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  return `${String(Math.floor(s / 60) % 60).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}`;
}

// Inverse of formatTime ("03:59" -> 239); null when the text is not a clock.
export function parseTime(text: unknown): number | null {
  const match = /^(\d+):(\d{1,2})$/.exec(String(text ?? ""));
  return match ? Number(match[1]) * 60 + Number(match[2]) : null;
}

export function encodeChatSend(text: string): Buffer {
  const chars = Array.from(String(text ?? "")).slice(0, NET_CHAT_MAX_CHARS).join("");
  const bytes = Buffer.from(chars, "utf8");
  const out = Buffer.alloc(Math.min(bytes.length, NET_CHAT_TEXT_SIZE - 1) + 1);
  bytes.copy(out, 0, 0, out.length - 1);
  return out;
}

export function decodeChatSend(payload: Buffer): string {
  return readCString(payload, 0, Math.min(payload.length, NET_CHAT_TEXT_SIZE));
}

export function encodeChat(chat: ChatPacket): Buffer {
  const out = Buffer.alloc(4 + 1 + PLAYER_NAME_SIZE + NET_CHAT_TEXT_SIZE);
  out.writeInt32LE(chat.uid | 0, 0);
  out[4] = chat.kind & 0xff;
  writeCString(out, 5, PLAYER_NAME_SIZE, chat.name);
  writeCString(out, 5 + PLAYER_NAME_SIZE, NET_CHAT_TEXT_SIZE, chat.text);
  return out;
}

export function decodeChat(payload: Buffer): ChatPacket | null {
  if (payload.length !== 4 + 1 + PLAYER_NAME_SIZE + NET_CHAT_TEXT_SIZE) return null;
  return {
    uid: payload.readInt32LE(0),
    kind: payload[4],
    name: readCString(payload, 5, PLAYER_NAME_SIZE),
    text: readCString(payload, 5 + PLAYER_NAME_SIZE, NET_CHAT_TEXT_SIZE),
  };
}

export function encodeKill(kill: KillPacket): Buffer {
  const out = Buffer.alloc(35);
  out.writeInt32LE(kill.victimUid | 0, 0);
  out.writeInt32LE(kill.killerUid | 0, 4);
  out.writeInt32LE(kill.weaponIndex | 0, 8);
  out[12] = kill.headshot ? 1 : 0;
  out[13] = kill.wallbang ? 1 : 0;
  out[14] = kill.fists ? 1 : 0;
  out.writeInt32LE(kill.victimDeaths | 0, 15);
  out.writeInt32LE(kill.killerKills | 0, 19);
  out.writeInt32LE(kill.killerClass | 0, 23);
  out.writeInt32LE(kill.score | 0, 27);
  out.writeFloatLE(kill.respawnDelay, 31);
  return out;
}

export function decodeKill(payload: Buffer): KillPacket | null {
  if (payload.length !== 35) return null;
  return {
    victimUid: payload.readInt32LE(0),
    killerUid: payload.readInt32LE(4),
    weaponIndex: payload.readInt32LE(8),
    headshot: !!payload[12],
    wallbang: !!payload[13],
    fists: !!payload[14],
    victimDeaths: payload.readInt32LE(15),
    killerKills: payload.readInt32LE(19),
    killerClass: payload.readInt32LE(23),
    score: payload.readInt32LE(27),
    respawnDelay: payload.readFloatLE(31),
  };
}

export function encodeHit(hit: HitPacket): Buffer {
  const out = Buffer.alloc(10);
  out.writeInt32LE(hit.targetUid | 0, 0);
  out.writeInt32LE(Math.round(hit.damage) | 0, 4);
  out[8] = hit.crit ? 1 : 0;
  out[9] = hit.kill ? 1 : 0;
  return out;
}

export function decodeHit(payload: Buffer): HitPacket | null {
  if (payload.length !== 10) return null;
  return { targetUid: payload.readInt32LE(0), damage: payload.readInt32LE(4), crit: !!payload[8], kill: !!payload[9] };
}

export function encodeHealth(health: HealthPacket): Buffer {
  const out = Buffer.alloc(17);
  out.writeInt32LE(health.uid | 0, 0);
  out.writeFloatLE(health.health, 4);
  out[8] = health.hasSource ? 1 : 0;
  out.writeFloatLE(health.sourceX || 0, 9);
  out.writeFloatLE(health.sourceZ || 0, 13);
  return out;
}

export function decodeHealth(payload: Buffer): HealthPacket | null {
  if (payload.length !== 17) return null;
  return {
    uid: payload.readInt32LE(0),
    health: payload.readFloatLE(4),
    hasSource: !!payload[8],
    sourceX: payload.readFloatLE(9),
    sourceZ: payload.readFloatLE(13),
  };
}

export function encodeScore(score: ScorePacket): Buffer {
  const out = Buffer.alloc(8);
  out.writeInt32LE(score.delta | 0, 0);
  out.writeInt32LE(score.score | 0, 4);
  return out;
}

export function decodeScore(payload: Buffer): ScorePacket | null {
  if (payload.length !== 8) return null;
  return { delta: payload.readInt32LE(0), score: payload.readInt32LE(4) };
}

function writeBoard(out: Buffer, offset: number, board: LeaderboardEntry[]): void {
  const rows = board.slice(0, LEADERBOARD_MAX);
  out[offset] = rows.length;
  rows.forEach((row, i) => {
    const at = offset + 1 + i * 16;
    out.writeInt32LE(row.uid | 0, at);
    out.writeInt32LE(row.score | 0, at + 4);
    out.writeInt32LE(row.kills | 0, at + 8);
    out.writeInt32LE(row.deaths | 0, at + 12);
  });
}

function readBoard(payload: Buffer, offset: number): LeaderboardEntry[] {
  const count = Math.min(payload[offset], LEADERBOARD_MAX);
  const rows: LeaderboardEntry[] = [];
  for (let i = 0; i < count; i++) {
    const at = offset + 1 + i * 16;
    rows.push({
      uid: payload.readInt32LE(at),
      score: payload.readInt32LE(at + 4),
      kills: payload.readInt32LE(at + 8),
      deaths: payload.readInt32LE(at + 12),
    });
  }
  return rows;
}

export function encodeLeaderboard(board: LeaderboardEntry[]): Buffer {
  const out = Buffer.alloc(LEADERBOARD_SIZE);
  writeBoard(out, 0, board);
  return out;
}

export function decodeLeaderboard(payload: Buffer): LeaderboardEntry[] | null {
  if (payload.length !== LEADERBOARD_SIZE) return null;
  return readBoard(payload, 0);
}

export function encodeTimer(timer: TimerPacket): Buffer {
  const out = Buffer.alloc(5);
  out.writeInt32LE(Math.max(0, timer.seconds | 0), 0);
  out[4] = timer.end ? 1 : 0;
  return out;
}

export function decodeTimer(payload: Buffer): TimerPacket | null {
  if (payload.length !== 5) return null;
  return { seconds: payload.readInt32LE(0), end: !!payload[4] };
}

export function encodeEnd(end: EndPacket): Buffer {
  const out = Buffer.alloc(9 + LEADERBOARD_SIZE);
  out.writeInt8(Math.max(-1, Math.min(1, end.result | 0)), 0);
  out.writeInt32LE(end.nextRound | 0, 1);
  out.writeInt32LE(end.winnerUid | 0, 5);
  writeBoard(out, 9, end.board);
  return out;
}

export function decodeEnd(payload: Buffer): EndPacket | null {
  if (payload.length !== 9 + LEADERBOARD_SIZE) return null;
  return {
    result: payload.readInt8(0),
    nextRound: payload.readInt32LE(1),
    winnerUid: payload.readInt32LE(5),
    board: readBoard(payload, 9),
  };
}

export function encodeInt32(value: number): Buffer {
  const out = Buffer.alloc(4);
  out.writeInt32LE(value | 0, 0);
  return out;
}

export function encodeUInt32(value: number): Buffer {
  const out = Buffer.alloc(4);
  out.writeUInt32LE(value >>> 0, 0);
  return out;
}

export function decodeInt32(payload: Buffer): number | null {
  return payload.length === 4 ? payload.readInt32LE(0) : null;
}

export function decodeUInt32(payload: Buffer): number | null {
  return payload.length === 4 ? payload.readUInt32LE(0) : null;
}

// io_ct escaping (the Krunker client renders chat as HTML).
export function escapeHtml(text: string): string {
  return text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#x27;");
}

export function unescapeHtml(text: string): string {
  return text
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"')
    .replace(/&#x27;/g, "'")
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, "&");
}

// ---- combat effects (NET_PACKET_SHOT .. NET_PACKET_EXPLOSION)

// Spectating (shared.h NET_PACKET_SPECTATE / NET_PACKET_SPECTATE_TARGET).
export const NET_SPECTATE = Object.freeze({
  SPECTATE: 28, // S->C NetSpectatePacket {uint8 spectating}: Krunker "start" with spectating = true
  SPECTATE_TARGET: 29, // C->S NetSpectateTargetPacket {int32 uid}: Krunker "fpSp"
} as const);

export const NET_FX = Object.freeze({
  SHOT: 21, // S->C "9"
  PROJECTILE: 22, // S->C "pr"
  PROJECTILE_END: 23, // S->C "pre"
  EXPLOSION: 24, // S->C "ex"
} as const);

export type ShotPacket = {
  uid: number;
  weaponIndex: number;
  hit: [number, number, number];
  hasNormal: boolean;
  normalYaw: number;
  normalPitch: number;
  trail: boolean;
  first: boolean;
};
export type ProjectilePacket = {
  id: number;
  ownerUid: number;
  weaponIndex: number;
  position: [number, number, number];
  velocity: [number, number, number];
  gravity: number;
  range: number;
};
export type ProjectileEndPacket = { id: number; position: [number, number, number]; impact: boolean };
export type ExplosionPacket = { position: [number, number, number]; radius: number };

// weapons.js projectile spd/grav by weapon id (8 RPG, 12 thrown knife, 13 Crossbow, 18 Blaster).
export const PROJECTILE_FLIGHT: Record<number, { speed: number; gravity: number; range: number }> = {
  8: { speed: 0.72, gravity: 0.00008, range: 1500 },
  12: { speed: 0.38, gravity: 0.0002, range: 1500 },
  13: { speed: 0.68, gravity: 0, range: 1500 },
  18: { speed: 1, gravity: 0, range: 500 },
};

export function encodeShot(shot: ShotPacket): Buffer {
  const out = Buffer.alloc(31);
  out.writeInt32LE(shot.uid | 0, 0);
  out.writeInt32LE(shot.weaponIndex | 0, 4);
  out.writeFloatLE(shot.hit[0], 8);
  out.writeFloatLE(shot.hit[1], 12);
  out.writeFloatLE(shot.hit[2], 16);
  out[20] = shot.hasNormal ? 1 : 0;
  out.writeFloatLE(shot.normalYaw || 0, 21);
  out.writeFloatLE(shot.normalPitch || 0, 25);
  out[29] = shot.trail ? 1 : 0;
  out[30] = shot.first ? 1 : 0;
  return out;
}

export function decodeShot(payload: Buffer): ShotPacket | null {
  if (payload.length !== 31) return null;
  return {
    uid: payload.readInt32LE(0),
    weaponIndex: payload.readInt32LE(4),
    hit: [payload.readFloatLE(8), payload.readFloatLE(12), payload.readFloatLE(16)],
    hasNormal: !!payload[20],
    normalYaw: payload.readFloatLE(21),
    normalPitch: payload.readFloatLE(25),
    trail: !!payload[29],
    first: !!payload[30],
  };
}

export function encodeProjectile(p: ProjectilePacket): Buffer {
  const out = Buffer.alloc(44);
  out.writeInt32LE(p.id | 0, 0);
  out.writeInt32LE(p.ownerUid | 0, 4);
  out.writeInt32LE(p.weaponIndex | 0, 8);
  p.position.forEach((v, i) => out.writeFloatLE(v, 12 + i * 4));
  p.velocity.forEach((v, i) => out.writeFloatLE(v, 24 + i * 4));
  out.writeFloatLE(p.gravity, 36);
  out.writeFloatLE(p.range, 40);
  return out;
}

export function decodeProjectile(payload: Buffer): ProjectilePacket | null {
  if (payload.length !== 44) return null;
  return {
    id: payload.readInt32LE(0),
    ownerUid: payload.readInt32LE(4),
    weaponIndex: payload.readInt32LE(8),
    position: [payload.readFloatLE(12), payload.readFloatLE(16), payload.readFloatLE(20)],
    velocity: [payload.readFloatLE(24), payload.readFloatLE(28), payload.readFloatLE(32)],
    gravity: payload.readFloatLE(36),
    range: payload.readFloatLE(40),
  };
}

export function encodeProjectileEnd(p: ProjectileEndPacket): Buffer {
  const out = Buffer.alloc(17);
  out.writeInt32LE(p.id | 0, 0);
  p.position.forEach((v, i) => out.writeFloatLE(v, 4 + i * 4));
  out[16] = p.impact ? 1 : 0;
  return out;
}

export function decodeProjectileEnd(payload: Buffer): ProjectileEndPacket | null {
  if (payload.length !== 17) return null;
  return {
    id: payload.readInt32LE(0),
    position: [payload.readFloatLE(4), payload.readFloatLE(8), payload.readFloatLE(12)],
    impact: !!payload[16],
  };
}

export function encodeExplosion(e: ExplosionPacket): Buffer {
  const out = Buffer.alloc(16);
  e.position.forEach((v, i) => out.writeFloatLE(v, i * 4));
  out.writeFloatLE(e.radius, 12);
  return out;
}

export function decodeExplosion(payload: Buffer): ExplosionPacket | null {
  if (payload.length !== 16) return null;
  return { position: [payload.readFloatLE(0), payload.readFloatLE(4), payload.readFloatLE(8)], radius: payload.readFloatLE(12) };
}

// ---- cosmetics (PlayerCosmetics in ENT after secondary_index and in SPAWN after it)

export type Cosmetics = { skinColor: number; hairColor: number; hat: number; back: number; waist: number; face: number; dye: number };
export const COSMETICS_NONE: Cosmetics = { skinColor: -1, hairColor: -1, hat: -1, back: -1, waist: -1, face: -1, dye: -1 };
export const COSMETICS_SIZE = 28;
export const ENT_COSMETICS_SIZE = 8 + COSMETICS_SIZE; // class, secondary, cosmetics (NET_ENT_PACKET_V3_SIZE)
export const ENT_SIZE = ENT_COSMETICS_SIZE + 2; // + spectate (ent[6]) + slidCont (ent[15]) (NET_ENT_PACKET_V4_SIZE)
export const ENT_SPRAY_SIZE = ENT_SIZE + 4; // + int32 sprayIndex (ent[1]) (NET_ENT_PACKET_V5_SIZE)
export const ENT_SKINS_SIZE = ENT_SPRAY_SIZE + 12; // + PlayerSkins: ent[2] weapon skins + ent[9] meleeIndex (NET_ENT_PACKET_V6_SIZE)
export const SPAWN_V1_SIZE = 78;
export const SPAWN_COSMETICS_SIZE = SPAWN_V1_SIZE + COSMETICS_SIZE;

// config.skinColors[i].col (Krunker sends the index, KrunkNative the colour).
export const SKIN_COLORS = [0x805c4a, 0xa77860, 0xd38d6f, 0xcc997e, 0xedbfa6];

export function skinColorFromIndex(index: unknown): number {
  const i = Number(index);
  return Number.isInteger(i) && i >= 0 && i < SKIN_COLORS.length ? SKIN_COLORS[i] : -1;
}

export function skinIndexFromColor(color: number): number {
  const i = SKIN_COLORS.indexOf(color);
  return i >= 0 ? i : 0;
}

// hairCol: Krunker keeps a hex colour string ("#rrggbb" / "rrggbb"); 0/undefined = class colour.
export function hairColorFromKrunker(value: unknown): number {
  if (typeof value === "number") return value > 0 ? value | 0 : -1;
  const match = /^#?([0-9a-f]{6})$/i.exec(String(value ?? ""));
  return match ? Number.parseInt(match[1], 16) : -1;
}

export function hairColorToKrunker(color: number): string | number {
  return color >= 0 ? `#${color.toString(16).padStart(6, "0")}` : 0;
}

function cosmeticIndex(value: unknown): number {
  const n = Number(value);
  return Number.isInteger(n) && n >= 0 ? n : -1;
}

// Krunker ent array -> PlayerCosmetics (gameserver.ts applyEntData: 3 hat, 4 back, 10 skinColIndex,
// 14 dye, 17 waist, 18 hairCol, 21 face).
export function cosmeticsFromEnt(ent: unknown[]): Cosmetics {
  return {
    skinColor: skinColorFromIndex(ent[10]),
    hairColor: hairColorFromKrunker(ent[18]),
    hat: cosmeticIndex(ent[3]),
    back: cosmeticIndex(ent[4]),
    waist: cosmeticIndex(ent[17]),
    face: cosmeticIndex(ent[21]),
    dye: cosmeticIndex(ent[14]),
  };
}

// Krunker spawn ("0") array -> PlayerCosmetics (13 hat, 14 back, 20 skinCol, 25 dye, 33 waist, 35 hairCol, 37 face).
export function cosmeticsFromSpawn(spawn: unknown[]): Cosmetics {
  return {
    skinColor: skinColorFromIndex(spawn[20]),
    hairColor: hairColorFromKrunker(spawn[35]),
    hat: cosmeticIndex(spawn[13]),
    back: cosmeticIndex(spawn[14]),
    waist: cosmeticIndex(spawn[33]),
    face: cosmeticIndex(spawn[37]),
    dye: cosmeticIndex(spawn[25]),
  };
}

// ---- weapon skins (PlayerSkins at the end of ENT V6 and in NET_PACKET_SKINS after the uid)

export const NET_PACKET_SKINS = 35; // S->C right before every SPAWN: int32 uid + PlayerSkins
export type WeaponSkins = { primary: number; secondary: number; melee: number };
export const SKINS_NONE: WeaponSkins = { primary: -1, secondary: -1, melee: -1 };
export const SKINS_PACKET_SIZE = 4 + 12;

// Krunker ent ([2] = [primary, secondary], [9] = meleeIndex) or spawn ([12] skins, [19] meleeIndex)
export function skinsFromKrunker(skins: unknown, melee: unknown): WeaponSkins {
  const pair = Array.isArray(skins) ? skins : [];
  return { primary: cosmeticIndex(pair[0]), secondary: cosmeticIndex(pair[1]), melee: cosmeticIndex(melee) };
}

export function writeSkins(out: Buffer, offset: number, s: WeaponSkins): void {
  [s.primary, s.secondary, s.melee].forEach((v, i) => out.writeInt32LE(v | 0, offset + i * 4));
}

export function readSkins(payload: Buffer, offset: number): WeaponSkins {
  if (payload.length < offset + 12) return { ...SKINS_NONE };
  return { primary: payload.readInt32LE(offset), secondary: payload.readInt32LE(offset + 4), melee: payload.readInt32LE(offset + 8) };
}

export function writeCosmetics(out: Buffer, offset: number, c: Cosmetics): void {
  [c.skinColor, c.hairColor, c.hat, c.back, c.waist, c.face, c.dye].forEach((v, i) => out.writeInt32LE(v | 0, offset + i * 4));
}

export function readCosmetics(payload: Buffer, offset: number): Cosmetics {
  if (payload.length < offset + COSMETICS_SIZE) return { ...COSMETICS_NONE };
  const v = (i: number) => payload.readInt32LE(offset + i * 4);
  return { skinColor: v(0), hairColor: v(1), hat: v(2), back: v(3), waist: v(4), face: v(5), dye: v(6) };
}
