// The `--v10-to-native` handshake, headless: a fake KrunkNative server (TCP, knet frames) and a fake
// Krunker v10 client (WebSocket, msgpack + ahNum trailer) talk through the real bridge on loopback.
// Checks: the connect packets the v10 client needs, LOAD -> HELLO, ENTER_GAME -> ENT (V9 layout),
// chat -> CHAT_SEND, native PING -> PONG, and that the native INIT stops the translation (no `init`).
//   npm test            (node -r ts-node/register/transpile-only v10Proxy.test.ts)
import assert from "assert";
import http from "http";
import net from "net";
import WebSocket from "ws";
import * as EV from "./nativeEvents";
import * as UTILS from "./utils";
import * as T from "./v10Translate";
import * as v10Proxy from "./v10Proxy";

function waitFor<V>(read: () => V | undefined, what: string, ms = 3000): Promise<V> {
  return new Promise((resolve, reject) => {
    const started = Date.now();
    const tick = () => {
      const value = read();
      if (value !== undefined) return resolve(value);
      if (Date.now() - started > ms) return reject(new Error(`timed out waiting for ${what}`));
      setTimeout(tick, 5);
    };
    tick();
  });
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

function nativeFrame(type: number, payload: Buffer): Buffer {
  const head = Buffer.alloc(3);
  head[0] = type;
  head.writeUInt16BE(payload.length, 1);
  return Buffer.concat([head, payload]);
}

// ---- layout of the translated ENT, without a socket
{
  const en: unknown[] = new Array(T.EN_LENGTH).fill(0);
  en[T.EN.CLASS] = 2;
  en[T.EN.SECONDARY] = 5;
  en[T.EN.SPRAY] = 7;
  en[T.EN.SKINS] = [11, 12];
  en[T.EN.MELEE] = 13;
  en[T.EN.LAG_COMP] = 0.5;
  en[T.EN.ATTACH] = -1;
  en[T.EN.SPECTATE] = 0;
  en[T.EN.CHALLENGE] = 1;
  en[T.EN.ANON] = 1;
  en[T.EN.HAT] = 3;
  en[T.EN.BODY] = 4;
  en[T.EN.DYE] = 6;
  en[T.EN.WAIST] = 8;
  en[T.EN.FACE] = 9;
  en[T.EN.SKIN_COLOR] = 10;
  en[T.EN.HAIR] = -1;
  en[T.EN.VERIFIED] = 1;
  en[T.EN.PREMIUM] = 0;
  en[T.EN.CUSTOM_BADGE] = 1;
  en[T.EN.SHOE] = 15;
  en[T.EN.PET] = 21;
  en[T.EN.WRIST] = 25;
  en[T.EN.CHARMS] = [30, 31];
  en[T.EN.BACK] = 33;
  en[T.EN.HEAD] = 34;
  en[T.EN.PLAYER_CARD] = 35;
  const ent = T.encodeEnter(en)!;
  assert.strictEqual(ent.length, T.ENT_V9_SIZE);
  assert.strictEqual(T.ENT_V9_SIZE, EV.ENT_LAG_COMP_SIZE + 43);
  assert.strictEqual(ent.readInt32LE(0), 2); // class
  assert.strictEqual(ent.readInt32LE(4), 5); // secondary
  assert.strictEqual(ent.readInt32LE(16), 3); // hat (cosmetics block at 8)
  assert.strictEqual(ent.readInt32LE(20), 4); // body
  assert.strictEqual(ent.readInt32LE(24), 8); // waist
  assert.strictEqual(ent.readInt32LE(28), 9); // face
  assert.strictEqual(ent.readInt32LE(32), 6); // dye
  assert.strictEqual(ent[EV.ENT_COSMETICS_SIZE], 0); // spectate
  assert.strictEqual(ent[EV.ENT_COSMETICS_SIZE + 1], 1); // slidCont: default
  assert.strictEqual(ent.readInt32LE(EV.ENT_SIZE), 7); // spray
  assert.strictEqual(ent.readInt32LE(EV.ENT_SPRAY_SIZE), 11); // primary skin
  assert.strictEqual(ent.readInt32LE(EV.ENT_SPRAY_SIZE + 4), 12); // secondary skin
  assert.strictEqual(ent.readInt32LE(EV.ENT_SPRAY_SIZE + 8), 13); // melee skin
  assert.ok(Math.abs(ent.readFloatLE(EV.ENT_SKINS_SIZE) - 0.5) < 1e-6); // lag comp
  assert.strictEqual(ent.readInt32LE(EV.ENT_LAG_COMP_SIZE), -1); // attach
  assert.strictEqual(ent[EV.ENT_LAG_COMP_SIZE + 4], 1); // challMode
  assert.strictEqual(ent[EV.ENT_LAG_COMP_SIZE + 5], 1); // anonMode
  const v7 = EV.ENT_LAG_COMP_SIZE + 6;
  assert.strictEqual(ent.readInt32LE(v7), 34); // head
  assert.strictEqual(ent.readInt32LE(v7 + 4), 33); // back (en[30] is 33)
  assert.strictEqual(ent.readInt32LE(v7 + 8), 15); // shoe
  assert.strictEqual(ent.readInt32LE(v7 + 12), 21); // pet
  assert.strictEqual(ent.readInt32LE(v7 + 16), 25); // wrist
  assert.strictEqual(ent.readInt32LE(v7 + 20), 30); // charm 0
  assert.strictEqual(ent.readInt32LE(v7 + 24), 31); // charm 1
  assert.strictEqual(ent.readInt32LE(v7 + 28), 35); // player card
  assert.strictEqual(ent.readInt32LE(v7 + 32), -1); // badge
  assert.strictEqual(ent[v7 + 36], 0x09); // show badges: verified (bit 0) + custom (bit 3)
  assert.strictEqual(T.encodeEnter([1, 2]), null);
  assert.strictEqual(T.encodeChat("   "), null);
}

// ---- loadout slots in the ENTER_GAME array (v10.0.3 selectSkin / getMyLoadout): an owned copy is the
// string "<copyId>#<skinIndex>" (the copy's paint lives on that copy), and a metadata slot is {i, m}
// (m.paint is the copy's paint). Both keep their store index in the ENT: KrunkNative keeps no
// per-copy paint, so the paint itself has nothing to carry, but the skin it paints must not be lost.
{
  const en: unknown[] = new Array(T.EN_LENGTH).fill(0);
  en[T.EN.CLASS] = 2;
  en[T.EN.SECONDARY] = 5;
  en[T.EN.SPRAY] = 7;
  en[T.EN.SKINS] = ["812#4211", { i: 12, m: { paint: "#dc3c3c" } }];
  en[T.EN.MELEE] = "813#7446";
  en[T.EN.HAT] = "814#3";
  en[T.EN.LAG_COMP] = 0.5;
  en[T.EN.ATTACH] = -1;
  const ent = T.encodeEnter(en)!;
  assert.strictEqual(ent.readInt32LE(EV.ENT_SPRAY_SIZE), 4211, "primary: an owned copy keeps its store index");
  assert.strictEqual(ent.readInt32LE(EV.ENT_SPRAY_SIZE + 4), 12, "secondary: a {i, m} slot keeps its store index");
  assert.strictEqual(ent.readInt32LE(EV.ENT_SPRAY_SIZE + 8), 7446, "melee: an owned copy keeps its store index");
  assert.strictEqual(ent.readInt32LE(16), 3, "hat: an owned copy keeps its store index");
  // a plain index and -1 (none) are unchanged
  const plain: unknown[] = new Array(T.EN_LENGTH).fill(0);
  plain[T.EN.SKINS] = [11, -1];
  plain[T.EN.MELEE] = -1;
  const pe = T.encodeEnter(plain)!;
  assert.strictEqual(pe.readInt32LE(EV.ENT_SPRAY_SIZE), 11);
  assert.strictEqual(pe.readInt32LE(EV.ENT_SPRAY_SIZE + 4), -1);
  assert.strictEqual(pe.readInt32LE(EV.ENT_SPRAY_SIZE + 8), -1);
}

// ---- MODE_STATE (40): the joiner's mode snapshot, and OBJECT_STATE (100): the join's object list.
// Layouts from shared/include/shared.h (NetModeStatePacket, NetObjectStatePacket), little endian.
type TestFlag = { x: number; y: number; z: number; carrier: number; team: number; uid: number; active: boolean };
function modeStateBuffer(o: { teamScores: [number, number]; lives: number; locked: number; message: string;
  objective: number; moveLock: number; flags?: TestFlag[]; objectiveWeapon?: number }): Buffer {
  const flags = o.flags ?? [];
  const b = Buffer.alloc(387); // NetModeStatePacket, protocol 1.4.3 (flags of 25 bytes)
  b.writeInt32LE(0, 0); // mode_id
  b.writeInt32LE(o.teamScores[0], 4);
  b.writeInt32LE(o.teamScores[1], 8);
  b.write("AT BASE", 12, "utf8"); // flag_info[0]
  b.write("AT BASE", 44, "utf8"); // flag_info[1]
  b.writeInt32LE(o.objective, 76); // active_objective
  b.writeInt32LE(o.objectiveWeapon ?? -1, 80); // objective_weapon
  b[84] = o.moveLock;
  b[85] = o.locked;
  b.writeInt32LE(o.lives, 86);
  b[90] = flags.length; // flag_count
  flags.forEach((fl, f) => {
    const at = 91 + f * 25;
    b.writeFloatLE(fl.x, at);
    b.writeFloatLE(fl.y, at + 4);
    b.writeFloatLE(fl.z, at + 8);
    b.writeInt32LE(fl.carrier, at + 12);
    b.writeInt32LE(fl.team, at + 16);
    b.writeInt32LE(fl.uid, at + 20);
    b[at + 24] = fl.active ? 1 : 0;
  });
  b.write(o.message, 291, "utf8"); // message[96]
  return b;
}
type TestSpawn = { uid: number; isYou: boolean; active: boolean; classIndex: number; team: number; health: number; maxHealth: number;
  position: [number, number, number]; direction: [number, number]; name: string; secondary: number; hat: number; body: number;
  hair: number; skinColor: number; dye: number; waist: number; face: number; head: number; back: number; shoe: number; pet: number;
  wrist: number; charms: [number, number]; playerCard: number; headMlt: number };
// NetSpawnPacket, protocol 1.4.3 (150 bytes): see parseSpawn in v10Translate.ts for the offsets.
function spawnBuffer(o: TestSpawn): Buffer {
  const b = Buffer.alloc(150);
  b.writeInt32LE(o.uid, 0);
  b[4] = o.isYou ? 1 : 0;
  b[5] = o.active ? 1 : 0;
  b.writeInt32LE(o.classIndex, 6);
  b.writeInt32LE(o.team, 10);
  b.writeFloatLE(o.health, 14);
  b.writeInt32LE(o.maxHealth, 18);
  o.position.forEach((v, i) => b.writeFloatLE(v, 22 + i * 4));
  o.direction.forEach((v, i) => b.writeFloatLE(v, 34 + i * 4));
  b.write(o.name, 42, "utf8");
  b.writeInt32LE(o.secondary, 74);
  [o.skinColor, o.hair, o.hat, o.body, o.waist, o.face, o.dye].forEach((v, i) => b.writeInt32LE(v, 78 + i * 4));
  b.writeInt32LE(-1, 106); // attach
  b.writeFloatLE(o.headMlt, 110);
  [o.head, o.back, o.shoe, o.pet, o.wrist, o.charms[0], o.charms[1], o.playerCard, -1].forEach((v, i) => b.writeInt32LE(v, 114 + i * 4));
  return b;
}
// AI_SYNC_V7 (161): one chunk of bots, [sid, x, y, z, xD, yD, health, didMov, team] each (protocol 1.3 layout).
function botSyncBuffer(first: boolean, bots: number[][]): Buffer {
  const b = Buffer.alloc(2 + 31 * bots.length);
  b[0] = first ? 1 : 0;
  b[1] = bots.length;
  bots.forEach((v, i) => {
    const at = 2 + i * 31;
    b.writeInt32LE(v[0], at);
    [v[1], v[2], v[3]].forEach((x, k) => b.writeFloatLE(x, at + 4 + k * 4));
    b.writeFloatLE(v[4], at + 16);
    b.writeFloatLE(v[5], at + 20);
    b.writeInt32LE(v[6], at + 24);
    b[at + 28] = v[7];
    b.writeInt16LE(v[8], at + 29);
  });
  return b;
}
function objectStateBuffer(uid: number, kind: number, value: number): Buffer {
  const b = Buffer.alloc(9);
  b.writeInt32LE(uid, 0);
  b[4] = kind;
  b.writeInt32LE(value, 5);
  return b;
}
{
  const ffa = T.mapModeState(modeStateBuffer({ teamScores: [5, 7], lives: 3, locked: 1, message: "wt", objective: -1, moveLock: 0 }));
  assert.strictEqual(ffa.stop, undefined);
  assert.deepStrictEqual(ffa.messages, [["ts", [1, 5]], ["ts", [2, 7]], ["lv", [3]], ["lock", [true]], ["gmsg", ["wt"]]]);
  const quiet = T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: -1, locked: 0, message: "", objective: -1, moveLock: 0 }));
  assert.deepStrictEqual(quiet.messages, [["ts", [1, 0]], ["ts", [2, 0]]]); // no lives, not locked, no message
  // flags in play are ufl(uid, x, y, z, carrier || 0), after the team lines; a flag not in play is skipped
  const flagged = T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: -1, locked: 0, message: "", objective: -1, moveLock: 0,
    flags: [{ x: 1, y: 2, z: 3, carrier: -1, team: 1, uid: 7, active: true }, { x: 4, y: 5, z: 6, carrier: 42, team: 2, uid: 9, active: false }] }));
  assert.strictEqual(flagged.stop, undefined);
  assert.deepStrictEqual(flagged.messages, [["ts", [1, 0]], ["ts", [2, 0]], ["ufi", [1, "AT BASE"]], ["ufi", [2, "AT BASE"]], ["ufl", [7, 1, 2, 3, 0]]]);
  const carried = T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: -1, locked: 0, message: "", objective: -1, moveLock: 0,
    flags: [{ x: 0.5, y: 2, z: 3, carrier: 42, team: 2, uid: 9, active: true }] }));
  assert.deepStrictEqual(carried.messages.filter((m) => m[0] === "ufl"), [["ufl", [9, 0.5, 2, 3, 42]]]);
  assert.match(T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: 0, locked: 0, message: "", objective: -1, moveLock: 0,
    flags: [{ x: 0, y: 0, z: 0, carrier: -1, team: 1, uid: -1, active: true }] })).stop || "", /map object id/);
  // an active objective is sent by MODE_SYNC_V7 (with its time): MODE_STATE sends none of it
  const objective = T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: 0, locked: 0, message: "", objective: 2, moveLock: 0 }));
  assert.strictEqual(objective.stop, undefined);
  assert.deepStrictEqual(objective.messages, [["ts", [1, 0]], ["ts", [2, 0]], ["lv", [0]]]);
  // a weapon objective is a wep (the objective weapon's uid)
  const weapon = T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: -1, locked: 0, message: "", objective: 2, moveLock: 0, objectiveWeapon: 2 }));
  assert.deepStrictEqual(weapon.messages, [["ts", [1, 0]], ["ts", [2, 0]], ["wep", [2]]]);
  assert.match(T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: 0, locked: 0, message: "", objective: -1, moveLock: 1 })).stop || "", /lock/);
  assert.strictEqual(T.mapModeState(Buffer.alloc(10)).stop !== undefined, true);

  assert.deepStrictEqual(T.mapObjectState(objectStateBuffer(7, 0, 0)).messages, [["do", [7]]]);
  assert.deepStrictEqual(T.mapObjectState(objectStateBuffer(7, 1, 0)).messages, [["ro", [7]]]);
  assert.deepStrictEqual(T.mapObjectState(objectStateBuffer(7, 2, 0)).messages, [["gte", [7, false]]]);
  assert.deepStrictEqual(T.mapObjectState(objectStateBuffer(7, 2, 1)).messages, [["gte", [7, true]]]);
  assert.deepStrictEqual(T.mapObjectState(objectStateBuffer(7, 3, 5)).messages, [["bnk", [7, 5]]]);
  assert.deepStrictEqual(T.mapObjectState(objectStateBuffer(7, 4, 4)).messages, [["mo", [7, 4]]]);
  assert.deepStrictEqual(T.mapObjectState(objectStateBuffer(7, 5, 0)).messages, [["mf", [7]]]);
  assert.match(T.mapObjectState(objectStateBuffer(7, 9, 0)).stop || "", /kind/);
}

{
  // SKINS: uid, primary, secondary, melee. SPAWN -> the v10 add record (53 fields) and the start of our own spawn.
  const sk = Buffer.alloc(16);
  [2, 7, 8, 9].forEach((v, i) => sk.writeInt32LE(v, i * 4));
  assert.deepStrictEqual(T.parseSkins(sk), { uid: 2, primary: 7, secondary: 8, melee: 9 });
  const other = T.parseSpawn(spawnBuffer({ uid: 2, isYou: false, active: true, classIndex: 3, team: 1, health: 87.6, maxHealth: 100,
    position: [10.4, 20.6, -5], direction: [1.5, 0.25], name: "Ada", secondary: 4, hat: 3, body: 5, hair: 0x112233, skinColor: -1,
    dye: 8, waist: 6, face: 7, head: 15, back: 16, shoe: 17, pet: 18, wrist: 19, charms: [20, 21], playerCard: 22, headMlt: 1 }));
  assert.ok(other);
  const rec = T.buildPlayerAdd(other!, { uid: 2, primary: 7, secondary: 8, melee: 9 }, "SOCKET");
  assert.strictEqual(rec.length, T.ADD_RECORD_FIELDS);
  assert.deepStrictEqual(rec, ["2", 2, 10, 21, -5, "Ada", 3, 88, 100, 1, 0, 0, [7, 8], 3, 5, 4, 0, null, 0.25, 9, -1, -1, -1, -1, 8, 0, -1, 0, "Ada", 17, 6, 0,
    "#112233", 7, 18, -1, 19, 0, 1, [20, 21], 0, 16, 15, 22, 0, 0, 0, 0, 0, 0, 0, 0, -1]);
  const mine = T.parseSpawn(spawnBuffer({ uid: 1, isYou: true, active: true, classIndex: 4, team: 0, health: 100, maxHealth: 100,
    position: [0, 0, 0], direction: [0, 0], name: "me", secondary: -1, hat: -1, body: -1, hair: -1, skinColor: -1, dye: -1, waist: -1,
    face: -1, head: -1, back: -1, shoe: -1, pet: -1, wrist: -1, charms: [-1, -1], playerCard: -1, headMlt: 1 }));
  assert.strictEqual(T.buildPlayerAdd(mine!, undefined, "SOCKET")[0], "SOCKET"); // our id is the socket id
  assert.deepStrictEqual(T.buildStartGame(mine!, 300, 1), [300, 0, 0, 1, 0, 1, null, 4]);
  assert.strictEqual(T.parseSpawn(Buffer.alloc(78)), null);
  // LEADERBOARD: the free-for-all rows, and a stop for another mode
  const board = Buffer.alloc(257);
  board[0] = 2;
  [1, 40, 3, 1, 2, 10, 0, 4].forEach((v, i) => board.writeInt32LE(v, 1 + i * 4));
  assert.deepStrictEqual(T.mapLeaderboard(board, 0).messages, [["7", [[1, 40, 3, 1, 2, 10, 0, 4]]]]);
  assert.match(T.mapLeaderboard(board, 1).stop || "", /Free for All/);
}

{
  // MODE_SYNC_V7 (149): team scores (ts for scored teams), lockout (lkt, ms), objective (obj uid, ms, true)
  const sync = (o: { scores: number[]; rounds?: number; lockout?: number; objective?: number; objectiveMs?: number }) => {
    const b = Buffer.alloc(65);
    o.scores.forEach((v, t) => b.writeInt32LE(v, t * 4));
    b.writeInt32LE(o.rounds ?? 0, 24);
    b.writeInt32LE(o.lockout ?? 0, 53);
    b.writeInt32LE(o.objective ?? -1, 57);
    b.writeInt32LE(o.objectiveMs ?? 0, 61);
    return b;
  };
  assert.deepStrictEqual(T.mapModeSync(sync({ scores: [0, 4, 0, 9, 0, 0] }), false).messages, [["ts", [1, 4]], ["ts", [3, 9]]]);
  assert.deepStrictEqual(T.mapModeSync(sync({ scores: [0, 0, 0, 0, 0, 0], lockout: 1500 }), false).messages, [["lkt", [1500]]]);
  assert.deepStrictEqual(T.mapModeSync(sync({ scores: [0, 0, 0, 0, 0, 0], objective: 3, objectiveMs: 4000 }), false).messages, [["obj", [3, 4000, true]]]);
  assert.deepStrictEqual(T.mapModeSync(sync({ scores: [0, 0, 0, 0, 0, 0], objective: 3, objectiveMs: 4000 }), true).messages, []); // the weapon: wep was sent
  assert.match(T.mapModeSync(sync({ scores: [0, 0, 0, 0, 0, 0], rounds: 2 }), false).stop || "", /rounds/);
  // DOM_UPDATE (142): one flat uct list of [object id, capturedBy, captureT, lastTeamOn, teamsOn, mlt] per zone
  const dom = Buffer.alloc(497);
  dom[0] = 2;
  dom.writeInt32LE(0, 1 + 0 * 31 + 0); // uid (the dom_zones index)
  dom.writeInt32LE(1, 1 + 0 * 31 + 4); // captured_by
  dom.writeFloatLE(250, 1 + 0 * 31 + 8); // capture_t, ms
  dom.writeInt32LE(0, 1 + 0 * 31 + 12); // last_team_on: none
  dom[1 + 0 * 31 + 16] = 1; // teams_on_count
  dom.writeInt8(1, 1 + 0 * 31 + 17); // teams_on[0]
  dom.writeFloatLE(1.5, 1 + 0 * 31 + 23); // mlt
  dom.writeInt32LE(42, 1 + 0 * 31 + 27); // object_id
  dom.writeInt32LE(57, 1 + 1 * 31 + 27); // object_id of zone 1 (mlt 0 -> 1, no teams)
  assert.deepStrictEqual(T.mapDomUpdate(dom).messages, [["uct", [[42, 1, 250, null, [1], 1.5, 57, 0, 0, null, [], 1]]]]);
  dom.writeInt32LE(-1, 1 + 1 * 31 + 27);
  assert.match(T.mapDomUpdate(dom).stop || "", /map object id/);
  // AI_SYNC_V7 (161): one chunk of bots -> the list of 9 values per bot; a short chunk ends the sync
  const one = T.parseBotSyncChunk(botSyncBuffer(true, [[5, 1.26, 2.04, -3.5, 0.123, -0.5, 88, 1, 2]]));
  assert.ok(one);
  assert.strictEqual(one!.first, true);
  assert.deepStrictEqual(one!.entries, [5, 1.3, 2, -3.5, 0.12, -0.5, 88, true, 2]);
  assert.strictEqual(T.parseBotSyncChunk(botSyncBuffer(false, [])) !== null, true);
  assert.strictEqual(T.mapModeSync(Buffer.alloc(10), false).stop !== undefined, true);

  // AI_ADD_V7 (162): the v7 bot record -> aai [sid, index, name, x, y, z, dat]
  const bot = Buffer.alloc(237);
  bot.writeInt32LE(9, 0);
  bot.writeInt32LE(4, 4);
  bot.write("Bot", 8, "utf8");
  [1.5, 2, 3].forEach((v, i) => bot.writeFloatLE(v, 48 + i * 4));
  bot[60] = 1; // has_data
  bot[61] = 1; // has_anim
  bot[62] = 1; // no_vis
  bot.writeInt16LE(2, 66); // team
  bot.writeFloatLE(0.5, 68); // anim_speed
  bot.writeFloatLE(30, 72); // sound_range
  bot.writeFloatLE(100, 76); // max_health
  bot.writeFloatLE(1.5, 80); // m_size
  bot.writeFloatLE(0.25, 84); // m_y_off
  bot.writeFloatLE(0.5, 88); // m_r_off
  bot.writeFloatLE(0, 92); // hbh 0 -> 1
  bot.write("46544e", 96, "utf8"); // asset
  bot.writeInt32LE(7, 112); // idle_sound
  bot.write("a/b.glb", 116, "utf8"); // src
  bot.writeFloatLE(1.25, 140); // tag_scale
  bot.writeInt16LE(3, 144); // lvl
  bot[146] = 1; // side
  bot.writeFloatLE(0.5, 147); // opacity
  bot.writeFloatLE(0.5, 151); // alpha_test
  bot[155] = 2; // tex_encode
  bot[156] = 4; // frames
  bot.writeFloatLE(0.25, 157); // frame_time
  bot.writeFloatLE(0.75, 161); // r_anim
  bot[165] = 1; // r_anim_axis
  bot.writeFloatLE(0.125, 166); // bob_d
  bot.writeFloatLE(2, 170); // bob_s
  bot[63] = 0; // no_fog
  bot[64] = 1; // f_trans
  bot[65] = 1; // no_smooth
  const [botLabel, botArgs] = T.mapBotAdd(bot).messages[0];
  assert.strictEqual(botLabel, "aai");
  assert.strictEqual((botArgs as unknown[]).length, 1, "aai is one record argument");
  const botRec = (botArgs as unknown[][])[0];
  assert.deepStrictEqual(botRec.slice(0, 6), [9, 4, "Bot", 1.5, 2, 3]);
  assert.deepStrictEqual(botRec[6], {
    hasAnim: 1, animS: 0.5, soundR: 30, team: 2, health: 100, mSize: 1.5, mYOff: 0.25, mROff: 0.5, hbh: 1,
    asset: "46544e", idl: 7, src: "a/b.glb", noVis: 1, tagScale: 1.25, lvl: 3, side: 1, opacity: 0.5, alphaTest: 0.5,
    texEncode: 2, noSmooth: true, noFog: false, fTrans: true, frames: 4, frameT: 0.25, rAnim: 0.75, rAnimA: 1,
    bobD: 0.125, bobS: 2,
  });
  bot[60] = 0;
  assert.strictEqual(((T.mapBotAdd(bot).messages[0][1] as unknown[][])[0])[6], 0);
}

// ---- PICKUP (128, packed 26 bytes) as inat with no player, and MATCH_VOTE_STATE (181, 170 bytes) as mv [vo, mts]
{
  const pickup = (o: { uid: number; weapon: number; active: number; moved?: boolean; pos?: number[] }) => {
    const b = Buffer.alloc(26);
    b.writeInt32LE(o.uid, 0);
    b.writeInt32LE(o.weapon, 4);
    b[8] = o.active;
    b[9] = o.moved ? 1 : 0;
    (o.pos || [0, 0, 0]).forEach((v, i) => b.writeFloatLE(v, 10 + 4 * i));
    return b;
  };
  assert.deepStrictEqual(T.mapPickup(pickup({ uid: 7, weapon: 12, active: 1 })).messages, [["inat", [-1, null, null, 7, 12, true, true, null]]]);
  assert.deepStrictEqual(T.mapPickup(pickup({ uid: 7, weapon: 12, active: 0, moved: true, pos: [1, 2, 3] })).messages,
    [["inat", [-1, null, null, 7, 12, true, false, [1, 2, 3]]]]);
  // an empty pickup (weapon -1) has no weapon: the server sets it to undefined (v10.js 173166), which goes as null; its
  // active flag is the server's own (v10.js 173186, !!active)
  assert.deepStrictEqual(T.mapPickup(pickup({ uid: 9, weapon: -1, active: 1 })).messages, [["inat", [-1, null, null, 9, null, true, true, null]]]);
  assert.match(T.mapPickup(Buffer.alloc(25)).stop || "", /malformed/);

  const vote = (count: number, voted: number, entries: Array<[number, number, number]>) => {
    const b = Buffer.alloc(170);
    b[0] = count;
    b.writeInt8(voted, 1);
    entries.forEach(([mode, map, votes], i) => {
      const at = 2 + 42 * i;
      b.writeInt32LE(mode, at);
      b.writeInt32LE(map, at + 4);
      b.writeUInt16LE(votes, at + 8);
    });
    return b;
  };
  // card 0: two other votes; card 1: ours (under our socket id)
  assert.deepStrictEqual(T.mapMatchVoteState(vote(2, 1, [[0, 5, 2], [1, 9, 1]]), "sock").messages,
    [["mv", [{ vote0: ["0,5", 2], sock: ["1,9", 1] }, ["0,5", "1,9"]]]]);
  // our vote on a card with other votes: ours is 1, the rest of its weight is the anonymous voter
  assert.deepStrictEqual(T.mapMatchVoteState(vote(1, 0, [[2, 3, 4]]), "sock").messages, [["mv", [{ sock: ["2,3", 1], vote0: ["2,3", 3] }, ["2,3"]]]]);
  // no vote (count 0) clears the cards
  assert.deepStrictEqual(T.mapMatchVoteState(vote(0, -1, []), "sock").messages, [["mv", [{}, []]]]);
  assert.match(T.mapMatchVoteState(vote(5, -1, []), "sock").stop || "", /malformed/);
  assert.match(T.mapMatchVoteState(Buffer.alloc(169), "sock").stop || "", /malformed/);
}

// NET 9 (NetChatPacket, 325 bytes): the kinds are the v7 line's (found junk 3, ranked 4, player 0, game 1, server 2)
{
  const v3 = (kind: number, uid: number, name: string, text: string) => {
    const b = Buffer.alloc(325);
    b.writeInt32LE(uid, 0);
    b[4] = kind;
    b.write(name, 5, "utf8");
    b.write(text, 37, "utf8");
    return b;
  };
  assert.deepStrictEqual(T.mapChat(v3(3, -1, "", "You found [1.25] Junk"), EV.escapeHtml).messages,
    [["ch", [-1, null, "<span style='color:#fff'>You</span> found <span style='color:#cc51f8'>[1.25]</span> Junk", 1]]]);
  assert.match(T.mapChat(v3(3, -1, "", "You found [1.2] Junk x"), EV.escapeHtml).stop || "", /found-junk/);
  assert.deepStrictEqual(T.mapChat(v3(4, -1, "", "Queued for ranked"), EV.escapeHtml).messages, [["ch", [-1, null, "Queued for ranked", 2, "#5ce05a"]]]);
  assert.match(T.mapChat(v3(7, -1, "", "x"), EV.escapeHtml).stop || "", /chat kind 7/);
  assert.match(T.mapChat(Buffer.alloc(324), EV.escapeHtml).stop || "", /malformed/);
  assert.deepStrictEqual(T.mapChat(v3(2, -1, "", "a <i>"), EV.escapeHtml).messages, [["ch", [-1, null, "a &lt;i&gt;", 2, null, null, null, null]]]);
}

// ---- q (INPUT, v10.js 221540-221580): the official client's input window -> NET_PACKET_INPUT_BATCH (111)
// The q is [resync, firstIsn, dts, dtLen, angles?, changes?, syncTimes?] (v10.js 221577); each frame's 15 fields are
// the v10 frame of the window: sequence, dt (ms), pitch and yaw (milli-radians, the first frame's, then the deltas), then
// fields 4..14 (move index, shoot, ADS, jump, reload, crouch, scroll, swap, speed, reset, interact) from "j-k" changes.
// The NetInputPacket: x_dir is the pitch, y_dir the yaw (bots.c: in.x_dir = pitch, in.y_dir = yaw).
{
  const st = T.newInputState();
  // frame 0 (isn 5): 16 ms, pitch 0.1, yaw -0.2, move index 3, shoot; every field sent (the first frame restates them)
  const allChanges = (j: number, over: Record<string, number>) => {
    const c: Record<string, number> = {};
    for (let k = 4; k <= 14; k++) c[`${j}-${k}`] = 0;
    return { ...c, ...Object.fromEntries(Object.entries(over).map(([k, v]) => [k, v])) };
  };
  const first = T.decodeInputBatch(st, [0, 5, "1616", 2, [100, -200, 5, 2], allChanges(0, { "0-4": 3, "0-5": 1 })]);
  assert.ok(first);
  assert.strictEqual(first!.length, 2);
  // seq, move (index 3 -> 2: the movDirs order), delta, pitch (x_dir), yaw (y_dir), flags
  assert.strictEqual(first![0].seq, 5);
  assert.strictEqual(first![0].moveDir, 2);
  assert.ok(Math.abs(first![0].delta - 0.016) < 1e-9);
  assert.ok(Math.abs(first![0].xDir - 0.1) < 1e-9, "x_dir is the pitch");
  assert.ok(Math.abs(first![0].yDir + 0.2) < 1e-9, "y_dir is the yaw");
  assert.strictEqual(first![0].flags, T.NET_INPUT.SHOOT);
  // frame 1 (isn 6): the angles are deltas (5 and 2 milli-radians), the same fields: no change sent, so they carry
  assert.strictEqual(first![1].seq, 6);
  assert.ok(Math.abs(first![1].xDir - 0.105) < 1e-9, "pitch accumulates the delta");
  assert.ok(Math.abs(first![1].yDir + 0.198) < 1e-9, "yaw accumulates the delta");
  assert.strictEqual(first![1].moveDir, 2);
  assert.strictEqual(first![1].flags, T.NET_INPUT.SHOOT);

  // each flag and value, one at a time (a fresh state each: the field is the only change)
  const one = (over: Record<string, number>) => T.decodeInputBatch(T.newInputState(), [1, 1, "16", 2, [0, 0], allChanges(0, over)])![0];
  assert.strictEqual(one({ "0-4": -1 }).moveDir, -1, "moveLock: no move direction");
  assert.strictEqual(one({ "0-4": 0 }).moveDir, 7, "move index 0 is direction 7");
  assert.strictEqual(one({ "0-4": 8 }).moveDir, -1, "a move index past the eight directions is no direction");
  assert.strictEqual(one({ "0-6": 1 }).flags, T.NET_INPUT.SCOPE, "ADS (scope) is field 6");
  assert.strictEqual(one({ "0-7": 1 }).flags, T.NET_INPUT.JUMP, "jump is field 7");
  assert.strictEqual(one({ "0-8": 1 }).flags, T.NET_INPUT.RELOAD, "reload is field 8");
  assert.strictEqual(one({ "0-9": 1 }).flags, T.NET_INPUT.CROUCH, "crouch is field 9");
  assert.strictEqual(one({ "0-10": -1 }).scroll, -1, "scroll is field 10");
  assert.strictEqual(one({ "0-11": 1 }).swap, 1, "weapon swap is field 11");
  assert.strictEqual(one({ "0-12": 0.5 }).slowdown, 5, "the slowdown (1 - speedLmt, in tenths) is field 12");
  assert.strictEqual(one({ "0-13": 1 }).flags, T.NET_INPUT.RESET_START, "reset 1 is reset to start");
  assert.strictEqual(one({ "0-13": 2 }).flags, T.NET_INPUT.RESET_LAST, "reset 2 is reset to last");
  assert.strictEqual(one({ "0-14": 1 }).flags, T.NET_INPUT.INTERACT, "interact is field 14");

  // a window with no angles keeps the last ones; a resync (1) restarts the deltas and the fields
  const st2 = T.newInputState();
  T.decodeInputBatch(st2, [0, 1, "16", 2, [100, 200], allChanges(0, { "0-5": 1 })]);
  const kept = T.decodeInputBatch(st2, [0, 2, "16", 2, 0, { "0-4": 2 }])!;
  assert.ok(Math.abs(kept[0].xDir - 0.1) < 1e-9 && kept[0].flags === T.NET_INPUT.SHOOT, "no angles: the window before's, the shoot carries");
  const resynced = T.decodeInputBatch(st2, [1, 9, "16", 2, 0, { "0-4": 2 }])!;
  assert.strictEqual(resynced[0].flags, 0, "a resync restarts the fields: no shoot");
  assert.strictEqual(T.decodeInputBatch(st2, [0, 1, 0, 0, 0, 0]), null, "a non-number dt list is malformed");
  assert.deepStrictEqual(T.decodeInputBatch(st2, [0, 1, "", 0, 0, 0]), [], "an empty window has no frames");
  // the batch encodes the NetInputPacket layout: count, then 24 bytes per frame
  const packed = T.encodeInputBatch(first!);
  assert.strictEqual(packed.length, 1 + 24 * 2);
  assert.strictEqual(packed[0], 2);
  assert.strictEqual(packed.readInt32LE(1), 5);
  assert.strictEqual(packed.readInt32LE(1 + 4), 2);
}

// ---- RECON (110, NetReconPacket, 89 bytes): the owner's authoritative state -> v10 "l" (the sync array, 26 slots;
// the handler reads them by index, v10.js 220091-220140). The packet's layout (packed, shared.h): ack_seq @0, position
// @4, velocity @16, yaw @28, aim_val @32, crouch_val @36, loadout @40, slide_timer @44, x_vel_c @48, z_vel_c @52, on_wall
// @56, flags @57, wall_jump_count @58, can_slide_timer @60 (s), slide_ms @64 (double), grapple_active @72, grapple @73.
{
  const recon = Buffer.alloc(89);
  recon.writeInt32LE(12, 0); // ack_seq
  [1.5, 2.25, -3].forEach((v, i) => recon.writeFloatLE(v, 4 + 4 * i)); // position
  [0.5, -1, 2].forEach((v, i) => recon.writeFloatLE(v, 16 + 4 * i)); // velocity: x, y, z
  recon.writeFloatLE(0.75, 28); // yaw
  recon.writeFloatLE(0.25, 32); // aim_val (KN: 0 hip, 1 ADS)
  recon.writeFloatLE(0.5, 36); // crouch_val
  recon.writeInt32LE(2, 40); // loadout_index
  recon.writeFloatLE(0.5, 44); // slide_timer (s): not used by the v10 record
  recon.writeFloatLE(3, 48); // x_vel_cylinder
  recon.writeFloatLE(4, 52); // z_vel_cylinder
  recon[56] = 1; // on_wall: side 1
  recon[57] = 0x01 | 0x10 | 0x20; // on ground, can slide, on terrain
  recon.writeUInt16LE(3, 58); // wall_jump_count
  recon.writeFloatLE(0.25, 60); // can_slide_timer (s): canSlideT is ms
  recon.writeDoubleLE(420, 64); // slide_ms
  recon[72] = 0; // no grapple
  const r = T.mapRecon(recon);
  assert.deepStrictEqual(r.messages, [["l", [[12, 0, 1.5, 2.25, -3, -1, 0.5, 2, 0.75, true, false, false, 3, 1, false, 0.75, 0.5, 2, 420, true, true, 3, 4, null, 250, null]]]]);
  // the aim is the inverse of the decoder's (aimVal = 1 - aim_val): a grapple reads its point and range
  recon[57] = 0x02; // did jump
  recon[72] = 1;
  [10, 11, 12, 13].forEach((v, i) => recon.writeFloatLE(v, 73 + 4 * i));
  const g = T.mapRecon(recon).messages[0][1][0] as unknown[];
  assert.strictEqual(g[9], false, "onGround is the flag");
  assert.strictEqual(g[10], true, "didJump is the flag");
  assert.deepStrictEqual(g[23], [10, 11, 12, 13], "the grapple: its point and range");
  assert.strictEqual(g[24], 250, "canSlideT: 0.25 s is 250 ms");
  assert.match(T.mapRecon(Buffer.alloc(58)).stop || "", /malformed/, "a short recon is malformed");
}

// ---- EXT 0x8120 (NetServerLinePacket, 324 bytes: int32 uid, name[32], text[288]): a server line about a player, the
// template's first "$" is the name (net_format_server_line; client/src/net/game_events.c events_server_line). v10 has no
// template key for it, so it goes as a server line: ch [-1, null, text, 2, null, null, null, null].
{
  const line = (uid: number, name: string, text: string) => {
    const b = Buffer.alloc(324);
    b.writeInt32LE(uid, 0);
    b.write(name, 4, "utf8");
    b.write(text, 36, "utf8");
    return b;
  };
  assert.deepStrictEqual(T.mapServerLine(line(3, "Ada", "$ joined the game"), EV.escapeHtml).messages,
    [["ch", [-1, null, "Ada joined the game", 2, null, null, null, null]]]);
  assert.deepStrictEqual(T.mapServerLine(line(-1, "", "Round starts"), EV.escapeHtml).messages,
    [["ch", [-1, null, "Round starts", 2, null, null, null, null]]], "a template with no $ shows as it is");
  assert.deepStrictEqual(T.mapServerLine(line(3, "a<b", "$ left"), EV.escapeHtml).messages,
    [["ch", [-1, null, "a&lt;b left", 2, null, null, null, null]]], "the name is escaped like the line");
  assert.match(T.mapServerLine(Buffer.alloc(323), EV.escapeHtml).stop || "", /malformed/);
}

// ---- SHOT (21, NetShotPacket, 31 bytes): one hitscan ray of another player's shot -> the tracer "9" (v10.js 173392, the
// handler _73392, and the server's broadcast at 173054-173066: [sid, hit x, y, z, normal yaw, pitch | nil, trail, shooter
// x, y, height, z, bulletType, impactType], one array argument). The shooter's origin is its last state (the handler uses
// the player object when it has one). KN has no bullet or impact type table: both 0 (the weapon's default, `|| 0`).
{
  const shot = (uid: number, hit: number[], normal: number[] | null, trail: number) => {
    const b = Buffer.alloc(31);
    b.writeInt32LE(uid, 0);
    b.writeInt32LE(3, 4); // weapon index
    hit.forEach((v, i) => b.writeFloatLE(v, 8 + 4 * i));
    b[20] = normal ? 1 : 0;
    if (normal) {
      b.writeFloatLE(normal[0], 21);
      b.writeFloatLE(normal[1], 25);
    }
    b[29] = trail;
    return b;
  };
  const shooters = new Map([[9, { x: 1.26, y: 2, z: 3, height: 9.2 }]]);
  assert.deepStrictEqual(T.mapShot(shot(9, [10.44, 2, -5.06], [0.5, 1.2], 1), shooters).messages,
    [["9", [[9, 10.4, 2, -5.1, 0.5, 1.2, 1, 1.3, 2, 9.2, 3, 0, 0]]]], "a hit on a map object: its normal, the origin rounded to 0.1");
  assert.deepStrictEqual(T.mapShot(shot(9, [4, 5, 6], null, 0), shooters).messages,
    [["9", [[9, 4, 5, 6, null, null, 0, 1.3, 2, 9.2, 3, 0, 0]]]], "no normal: nil angles");
  // a shooter the bridge has no state for: the origin is the hit point, height 0 (the handler draws nothing for it)
  assert.deepStrictEqual(T.mapShot(shot(4, [1, 2, 3], null, 0), shooters).messages,
    [["9", [[4, 1, 2, 3, null, null, 0, 1, 2, 0, 3, 0, 0]]]], "an unknown shooter: the hit point is the origin");
  assert.match(T.mapShot(Buffer.alloc(30), shooters).stop || "", /malformed/);
}

// ---- LEAVE (20, NetLeavePacket, 4 bytes: int32 uid): another player disconnected -> "2" (REMOVE_PLAYER, v10.js 97927; its
// handler _73270 takes the one sid: it clears that player's entry). The uid is the sid.
{
  const leave = (uid: number) => {
    const b = Buffer.alloc(4);
    b.writeInt32LE(uid, 0);
    return b;
  };
  assert.deepStrictEqual(T.mapLeave(leave(9)).messages, [["2", [9]]]);
  assert.match(T.mapLeave(Buffer.alloc(3)).stop || "", /malformed/);

// ---- Joiner waiting to spawn: v10's KILL_PLAYER "3" (handler _73275, v10.js 220335) starts the death UI and the
// respawn countdown (forceSpawn + 1 seconds after 2.8 s, v10.js 220379). Its arguments: victim sid, deaths,
// killer sid (0: none), assists [], weapon (nil), headshot.
assert.deepStrictEqual(T.mapDeathCountdown(1).messages, [["3", [1, 0, 0, [], null, false]]]);

// ... then "frs" (the server's ENTER_GAME label, v10.js 97956; the client's handler 222318 calls enterGame(true)), which
// makes the client send "en" (enterGame sends it, v10.js 208129).
assert.deepStrictEqual(T.mapEnterGame().messages, [["frs", [true]]]);
}

async function main(): Promise<void> {
  // ---- the fake KrunkNative server: records every frame the bridge writes, sends what the test asks
  const nativeFrames: Array<{ type: number; payload: Buffer }> = [];
  let nativeSocket: net.Socket | null = null;
  let nativeBuffer = Buffer.alloc(0);
  const nativeServer = net.createServer((socket) => {
    nativeSocket = socket;
    socket.on("data", (chunk) => {
      nativeBuffer = Buffer.concat([nativeBuffer, chunk]);
      while (nativeBuffer.length >= 3) {
        const size = nativeBuffer.readUInt16BE(1);
        if (nativeBuffer.length < 3 + size) break;
        nativeFrames.push({ type: nativeBuffer[0], payload: Buffer.from(nativeBuffer.subarray(3, 3 + size)) });
        nativeBuffer = nativeBuffer.subarray(3 + size);
      }
    });
  });
  await new Promise<void>((resolve) => nativeServer.listen(0, "127.0.0.1", () => resolve()));
  const nativePort = (nativeServer.address() as net.AddressInfo).port;

  // ---- the bridge in v10 mode, on loopback
  const bridge = http.createServer();
  v10Proxy.attach(bridge, { nativeHost: "127.0.0.1", nativePort });
  await new Promise<void>((resolve) => bridge.listen(0, "127.0.0.1", () => resolve()));
  const bridgePort = (bridge.address() as net.AddressInfo).port;

  // ---- the fake v10 client
  const clientMessages: Array<{ label: string; args: unknown[] }> = [];
  const ws = new WebSocket(`ws://127.0.0.1:${bridgePort}${v10Proxy.WS_PATH}?gameId=test`);
  ws.binaryType = "arraybuffer";
  ws.on("message", (data: Buffer) => {
    const [, label, args] = UTILS.decodePacket(data);
    clientMessages.push({ label, args });
  });
  await new Promise<void>((resolve, reject) => {
    ws.once("open", () => resolve());
    ws.once("error", reject);
  });
  const sendV10 = (label: string, ...args: unknown[]) => {
    ws.send(UTILS.encodeNetworkMessage(UTILS.serializeNetworkMessage(label, ...args), 0x2a));
  };

  // 1. the connect packets the v10 client needs (io-init, load, inst-id), each with a trailer
  const socketIo = await waitFor(() => clientMessages.find((m) => m.label === "io-init"), "io-init");
  assert.strictEqual(typeof socketIo.args[0], "string");
  assert.strictEqual((socketIo.args[0] as string).length, 12);
  const load = await waitFor(() => clientMessages.find((m) => m.label === "load"), "INIT_LOAD");
  assert.deepStrictEqual(load.args, [20000, socketIo.args[0]]);
  const inst = await waitFor(() => clientMessages.find((m) => m.label === "inst-id"), "inst-id");
  assert.deepStrictEqual(inst.args, ["KN:local"]);

  // 2. the bridge opens the native socket and writes the forwarded address first
  const forwarded = await waitFor(() => nativeFrames[0], "forwarded frame");
  assert.strictEqual(forwarded.type, UTILS.NET_PACKET_FORWARDED);
  assert.strictEqual(forwarded.payload.length, 9); // "127.0.0.1" (loopback peer, no forwarding header)
  assert.strictEqual(nativeFrames.length, 1); // nothing else until the client speaks

  // 3. LOAD -> HELLO: protocol 1.4.5 (NET_PROTOCOL_PATCH 5, the NetStatePacket layout), Krunker v10 ruleset bit
  sendV10("load");
  const hello = await waitFor(() => nativeFrames.find((f) => f.type === T.NATIVE.HELLO), "HELLO");
  assert.deepStrictEqual([...hello.payload], [1, 4, 5, 4]);
  sendV10("load"); // a second LOAD sends no second HELLO
  await sleep(100);
  assert.strictEqual(nativeFrames.filter((f) => f.type === T.NATIVE.HELLO).length, 1);

  // 4. the map, the config and the network data come before the INIT (net_send_init), and the INIT
  //    comes before the map bytes: the bridge asks for the map (MAP_REQUEST), holds the init until the
  //    JSON is whole, then sends one init with the map object in argument 9 (no /maps/<id> fetch).
  const mapJson = JSON.stringify({ name: "Test Map", objects: [], padding: "m".repeat(700) });
  const mapBytes = Buffer.from(mapJson, "utf8");
  const mapHash = 0xc0ffee;
  const mapInfo = Buffer.alloc(44);
  mapInfo.writeInt32LE(3, 0); // map index
  mapInfo.writeUInt32LE(mapHash, 4);
  mapInfo.writeUInt32LE(mapBytes.length, 8);
  mapInfo.write("Test Map", 12, "utf8");
  const settings = { hitBoxPad: 1, timeLimit: 5 };
  const configBytes = Buffer.from(JSON.stringify({ host: 1, settings }), "utf8");
  const cut = 20;
  const configChunk = (offset: number, data: Buffer) => {
    const head = Buffer.alloc(8);
    head.writeUInt32LE(configBytes.length, 0);
    head.writeUInt32LE(offset, 4);
    return Buffer.concat([head, data]);
  };
  const networkData = Buffer.alloc(10);
  networkData.writeUInt16LE(10, 0); // send_hz
  networkData.writeUInt16LE(30, 2); // tick_hz
  networkData.writeUInt16LE(1, 4); // sync_hz
  const init = Buffer.alloc(18);
  init[0] = 1;
  init[1] = 4;
  init[2] = 2;
  init.writeInt32LE(3, 3); // map index
  init.writeInt32LE(0, 7); // mode index: Free for All
  init.writeUInt16LE(30, 11); // tick rate
  init.writeInt32LE(1, 13); // client id
  init[17] = 3; // ruleset
  assert.ok(nativeSocket);
  const native = nativeSocket as net.Socket;
  native.write(nativeFrame(25, mapInfo)); // MAP_INFO
  native.write(nativeFrame(T.NATIVE.INIT, init)); // INIT, before the map bytes and the config (the server's order)
  native.write(nativeFrame(183, configChunk(0, configBytes.subarray(0, cut)))); // GAME_CONFIG 1
  native.write(nativeFrame(183, configChunk(cut, configBytes.subarray(cut)))); // GAME_CONFIG 2
  native.write(nativeFrame(182, networkData)); // NETWORK_DATA
  native.write(nativeFrame(254, Buffer.from([0x50, 0x80, 0]))); // EXT 0x8050: not a ranked match
  const request = await waitFor(() => nativeFrames.find((f) => f.type === 26), "MAP_REQUEST");
  assert.strictEqual(request.payload.readUInt32LE(0), mapHash);
  await sleep(100);
  assert.strictEqual(clientMessages.some((m) => m.label === "init"), false); // held until the map is whole
  const half = 496;
  const mapChunk = (offset: number, data: Buffer) => {
    const head = Buffer.alloc(10);
    head.writeUInt32LE(mapHash, 0);
    head.writeUInt32LE(offset, 4);
    head.writeUInt16LE(data.length, 8);
    return nativeFrame(27, Buffer.concat([head, data]));
  };
  native.write(mapChunk(0, mapBytes.subarray(0, half)));
  native.write(mapChunk(half, mapBytes.subarray(half)));
  const initMsg = await waitFor(() => clientMessages.find((m) => m.label === "init"), "init");
  assert.strictEqual(clientMessages.filter((m) => m.label === "init").length, 1);
  const args = initMsg.args;
  assert.strictEqual(args.length, 20); // the v10 server's INIT_GAME list (v10.js 222270)
  assert.strictEqual(args[0], 3); // mapIndex
  assert.strictEqual(args[1], 0); // modeIndex
  assert.deepStrictEqual(args[6], settings); // config: the GAME_CONFIG settings
  assert.strictEqual(args[9] && (args[9] as { data: unknown }).data, mapJson); // customMapData.data: the map JSON text
  assert.deepStrictEqual(args[12], { snf: 10, tf: 30, sf: 1 }); // networkData
  assert.strictEqual(args[13], true); // the create-game path's init 4th argument (doc/v10/constants-1003.md 9)
  assert.strictEqual(args[14], false); // isOfficial: a Box Office game is not a krunker.io official game
  assert.strictEqual(args[15], false); // isComp
  assert.strictEqual(args[16], false); // isRanked

  // a reset-only global value before any gv clears nothing the client holds: no message, no stop
  const reset = Buffer.alloc(80);
  reset[0] = 0x01; // NET_VALUE_RESET, no key
  native.write(nativeFrame(121, reset));
  // a global value after the INIT is a gv to the client (flags 0, the key, a double)
  const globalValue = Buffer.alloc(80);
  globalValue[0] = 0;
  globalValue.write("score_limit", 1, "utf8");
  globalValue.writeDoubleLE(2.5, 72);
  native.write(nativeFrame(121, globalValue));
  const gv = await waitFor(() => clientMessages.find((m) => m.label === "gv"), "gv");
  assert.deepStrictEqual(gv.args, ["score_limit", 2.5]);

  // a player sound (add) after the INIT is an asnd to the client: uid, name, {loop, vol, rate}
  const sound = Buffer.alloc(39);
  sound.writeInt32LE(2, 0);
  sound[4] = 1; // add
  sound[6] = 1; // loop
  sound.writeFloatLE(0.5, 7);
  sound.writeFloatLE(1.25, 11);
  sound.write("footstep", 15, "utf8");
  native.write(nativeFrame(125, sound));
  const asnd = await waitFor(() => clientMessages.find((m) => m.label === "asnd"), "asnd");
  assert.deepStrictEqual(asnd.args, [2, "footstep", { loop: true, vol: 0.5, rate: 1.25 }]);

  // a player sound (remove) is an rsnd to the client: uid, name, force
  const removed = Buffer.from(sound);
  removed[4] = 0; // remove
  native.write(nativeFrame(125, removed));
  const rsnd = await waitFor(() => clientMessages.find((m) => m.label === "rsnd"), "rsnd");
  assert.deepStrictEqual(rsnd.args, [2, "footstep", false]);

  // the round timer is a t [seconds, end] to the client
  const timer = Buffer.alloc(5);
  timer.writeInt32LE(300, 0);
  native.write(nativeFrame(15, timer));
  const t = await waitFor(() => clientMessages.find((m) => m.label === "t"), "t");
  assert.deepStrictEqual(t.args, [300, 0]);

  // the joiner's mode snapshot: team scores, lives and the lock, then the object list
  native.write(nativeFrame(40, modeStateBuffer({ teamScores: [5, 7], lives: 3, locked: 0, message: "", objective: -1, moveLock: 0 })));
  await waitFor(() => clientMessages.filter((m) => m.label === "ts").length >= 2 ? true : undefined, "ts");
  const lv = await waitFor(() => clientMessages.find((m) => m.label === "lv"), "lv");
  assert.deepStrictEqual(lv.args, [3]);
  assert.deepStrictEqual(clientMessages.filter((m) => m.label === "ts").map((m) => m.args), [[1, 5], [2, 7]]);
  native.write(nativeFrame(100, objectStateBuffer(4, 0, 0))); // do 4
  native.write(nativeFrame(100, objectStateBuffer(9, 2, 1))); // gte 9 open
  await waitFor(() => clientMessages.find((m) => m.label === "gte"), "gte");
  assert.deepStrictEqual(clientMessages.find((m) => m.label === "do")?.args, [4]);
  assert.deepStrictEqual(clientMessages.find((m) => m.label === "gte")?.args, [9, true]);

  // the ping result answers the bridge's PONG: v10's socket-ping result handler is empty, so it is consumed
  native.write(nativeFrame(19, Buffer.from([0x40, 0, 0, 0])));
  await sleep(50);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("PING_RESULT"), undefined);

  // the v7 capability notice (249): the bridge sends chat as CHAT_SEND, so it needs nothing
  native.write(nativeFrame(249, Buffer.from([1])));
  await sleep(50);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("SOCIAL_V7"), undefined);

  // the server repeats the config and the networking after the init (the HELLO path): the same values are no change
  for (const chunk of [configChunk(0, configBytes.subarray(0, cut)), configChunk(cut, configBytes.subarray(cut))]) native.write(nativeFrame(183, chunk));
  native.write(nativeFrame(182, networkData));
  await sleep(50);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("GAME_CONFIG"), undefined);

  // the match phase after the init is a gsc [state] (KN GameState 3 IN_PROGRESS = v10 gameState 3)
  native.write(nativeFrame(254, Buffer.from([0x01, 0x09, 0x03])));
  const gsc = await waitFor(() => clientMessages.find((m) => m.label === "gsc"), "gsc");
  assert.deepStrictEqual(gsc.args, [3]);

  // the game announcement card (EXT 0x0302): its message is a gameAnnouncement {message, actions}
  const card = Buffer.alloc(321);
  card.write("Welcome to the test", 0, "utf8");
  native.write(nativeFrame(254, Buffer.concat([Buffer.from([0x02, 0x03]), card])));
  const announce = await waitFor(() => clientMessages.find((m) => m.label === "gameAnnouncement"), "gameAnnouncement");
  assert.deepStrictEqual(announce.args, [{ message: "Welcome to the test", actions: [] }]);

  // SKINS, then the SPAWN of another player and of ours: the add record (ADD_PLAYERS "0") and our start
  native.write(nativeFrame(35, Buffer.from(Buffer.concat([Buffer.from([0x02, 0, 0, 0]), Buffer.from([0x07, 0, 0, 0, 0x08, 0, 0, 0, 0x09, 0, 0, 0])]))));
  native.write(nativeFrame(4, spawnBuffer({ uid: 2, isYou: false, active: true, classIndex: 3, team: 1, health: 90, maxHealth: 100,
    position: [1, 2, 3], direction: [0, 0], name: "other", secondary: -1, hat: -1, body: -1, hair: -1, skinColor: -1, dye: -1, waist: -1,
    face: -1, head: -1, back: -1, shoe: -1, pet: -1, wrist: -1, charms: [-1, -1], playerCard: -1, headMlt: 1 })));
  const addOther = await waitFor(() => clientMessages.find((m) => m.label === "0" && (m.args[0] as unknown[])[1] === 2), "ADD_PLAYERS (other)");
  assert.deepStrictEqual((addOther.args[0] as unknown[]).slice(12, 13), [[7, 8]]); // the SKINS of uid 2
  assert.strictEqual((addOther.args[0] as unknown[]).length, 53);
  assert.strictEqual(addOther.args[1], 1);
  native.write(nativeFrame(4, spawnBuffer({ uid: 1, isYou: true, active: true, classIndex: 4, team: 0, health: 100, maxHealth: 100,
    position: [1, 2, 3], direction: [0, 0], name: "me", secondary: -1, hat: -1, body: -1, hair: -1, skinColor: -1, dye: -1, waist: -1,
    face: -1, head: -1, back: -1, shoe: -1, pet: -1, wrist: -1, charms: [-1, -1], playerCard: -1, headMlt: 1 })));
  const addMine = await waitFor(() => clientMessages.find((m) => m.label === "0" && (m.args[0] as unknown[])[1] === 1), "ADD_PLAYERS (us)");
  assert.strictEqual((addMine.args[0] as unknown[])[0], socketIo.args[0]); // our id is the io-init socket id
  const start = await waitFor(() => clientMessages.find((m) => m.label === "start"), "start");
  assert.deepStrictEqual(start.args, [300, 0, 0, 0, 0, 1, null, 4]); // gameTimer from the last t, our class

  // the leaderboard (14, free for all: the rows are [sid, score, kills, deaths]) and a bot (AI_ADD 90 -> aai)
  const board = Buffer.alloc(257);
  board[0] = 1;
  [1, 12, 2, 3].forEach((v, i) => board.writeInt32LE(v, 1 + i * 4));
  native.write(nativeFrame(14, board));
  const rows = await waitFor(() => clientMessages.find((m) => m.label === "7"), "7");
  assert.deepStrictEqual(rows.args, [[1, 12, 2, 3]]);
  // the LEADER_STATS that repeats the board (uid, then 3 values per row) is consumed
  const stats = Buffer.alloc(322);
  stats[0] = 1;
  stats[1] = 3;
  stats.writeInt32LE(1, 2);
  [12, 2, 3].forEach((v, i) => stats.writeInt32LE(v, 66 + i * 4));
  native.write(nativeFrame(240, stats));
  await sleep(50);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("LEADER_STATS"), undefined);
  // chat kinds 3 (found junk: the v10 line the client builds) and 4 (ranked: the v10 direct line in green)
  const junk = Buffer.alloc(330);
  junk[9] = 3;
  junk.write("You found [1.25] Junk", 42, "utf8");
  assert.deepStrictEqual(T.mapChatV7(junk, EV.escapeHtml).messages, [["ch", [-1, null, "<span style='color:#fff'>You</span> found <span style='color:#cc51f8'>[1.25]</span> Junk", 1]]]);
  junk.write("You found [1.2] Junk x", 42, "utf8");
  assert.match(T.mapChatV7(junk, EV.escapeHtml).stop || "", /found-junk/);
  const ranked = Buffer.alloc(330);
  ranked[9] = 4;
  ranked.write("Queued for ranked", 42, "utf8");
  assert.deepStrictEqual(T.mapChatV7(ranked, EV.escapeHtml).messages, [["ch", [-1, null, "Queued for ranked", 2, "#5ce05a"]]]);
  assert.strictEqual(T.mapState(Buffer.alloc(53)), null); // a short STATE is malformed

  // the per-tick state record: the 13 fields of v10's "k" record, from KN's 79-byte state
  const st = Buffer.alloc(79);
  st.writeInt32LE(4, 0);
  st.writeFloatLE(1.256, 13);
  st.writeFloatLE(2, 17);
  st.writeFloatLE(-3.5, 21);
  st.writeFloatLE(0.5, 37);
  st.writeFloatLE(1, 41);
  st.writeFloatLE(0.25, 57);
  st[61] = 1;
  st.writeFloatLE(0.6, 53);
  st.writeInt32LE(2, 9);
  st.writeFloatLE(1, 49);
  st[62] = 1;
  [1, 2, 3].forEach((v, i) => st.writeFloatLE(v, 63 + i * 4));
  st.writeFloatLE(42.4, 75);
  const rec = T.mapState(st);
  assert.ok(rec);
  assert.deepStrictEqual(rec!.record, [4, 1.26, 2, -3.5, 57, 29, 0.25, 1, 1, 2, 1, [1, 2, 3], 42]);

  // the v3 bot add (90) is consumed: no aai, and no stop (the v7 record, 162, is the one for a v7 peer)
  const bot = Buffer.alloc(101);
  bot.writeInt32LE(5, 0);
  bot.writeInt32LE(0, 4);
  bot.write("Bot", 8, "utf8");
  native.write(nativeFrame(90, bot));
  await sleep(50);
  assert.strictEqual(clientMessages.some((m) => m.label === "aai"), false);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("AI_ADD"), undefined);
  native.write(nativeFrame(91, Buffer.from([1, 0]))); // the v3 sync with no entries: consumed as well
  await sleep(50);
  assert.strictEqual(clientMessages.some((m) => m.label === "ai"), false);

  // the joiner's mode sync (149): the scored teams as ts, no rounds, the lockout and the objective
  const modeSync = Buffer.alloc(65);
  [0, 5, 7, 0, 0, 0].forEach((v, t) => modeSync.writeInt32LE(v, t * 4));
  modeSync.writeInt32LE(-1, 57); // no objective
  native.write(nativeFrame(149, modeSync));
  const ts57 = await waitFor(() => clientMessages.filter((m) => m.label === "ts").length >= 4 ? clientMessages.filter((m) => m.label === "ts") : undefined, "ts (sync)");
  assert.deepStrictEqual(ts57.slice(-2).map((m) => m.args), [[1, 5], [2, 7]]);
  // a bot (162) reaches the client as aai
  const bot7 = Buffer.alloc(237);
  bot7.writeInt32LE(5, 0);
  bot7.writeInt32LE(0, 4);
  bot7.write("Bot", 8, "utf8");
  bot7[60] = 0; // no data: dat 0
  native.write(nativeFrame(162, bot7));
  const aai7 = await waitFor(() => clientMessages.find((m) => m.label === "aai"), "aai (v7)");
  assert.deepStrictEqual(aai7.args, [[5, 0, "Bot", 0, 0, 0, 0]]); // one record argument

  // the domination zones (142): one uct list; the zone's map object id is its uid
  const domFrame = Buffer.alloc(497);
  domFrame[0] = 1;
  domFrame.writeInt32LE(0, 1);
  domFrame.writeInt32LE(1, 5);
  domFrame.writeFloatLE(0, 9);
  domFrame.writeInt32LE(0, 13);
  domFrame.writeFloatLE(1, 24);
  domFrame.writeInt32LE(42, 28);
  native.write(nativeFrame(142, domFrame));
  const uct = await waitFor(() => clientMessages.find((m) => m.label === "uct"), "uct");
  assert.deepStrictEqual(uct.args, [[42, 1, 0, null, [], 1]]);
  // the bot positions (161): one short chunk is one ai list with the bot in it
  native.write(nativeFrame(161, botSyncBuffer(true, [[5, 1, 2, 3, 0, 0, 88, 0, 0]])));
  const ai = await waitFor(() => clientMessages.find((m) => m.label === "ai"), "ai");
  assert.deepStrictEqual(ai.args, [[5, 1, 2, 3, 0, 0, 88, false, 0]]);

  // a chat line for a v7 peer (245): ch (channel, name, text, kind, _, _, account, _), the text escaped
  const chatLine = Buffer.alloc(330);
  chatLine.writeInt8(0, 0); // channel: global
  chatLine.writeInt32LE(7, 1); // account id
  chatLine.writeInt32LE(3, 5); // sender uid
  chatLine[9] = 0; // a player
  chatLine.write("Ada", 10, "utf8");
  chatLine.write("hi <b>", 42, "utf8");
  native.write(nativeFrame(245, chatLine));
  const ch = await waitFor(() => clientMessages.find((m) => m.label === "ch"), "ch");
  assert.deepStrictEqual(ch.args, [0, "Ada", EV.escapeHtml("hi <b>"), 0, null, null, 7, null]);

  // a v3 chat line (NET 9, NetChatPacket: uid, kind, name, text): the game, server and trigger lines a v7 peer still gets.
  // It has no channel and no account: channel -1 (all), account null; a server line (kind 2) has no name
  const serverLine = Buffer.alloc(325);
  serverLine.writeInt32LE(-1, 0);
  serverLine[4] = 2;
  serverLine.write("Round 2 starts", 37, "utf8");
  native.write(nativeFrame(9, serverLine));
  const ch9 = await waitFor(() => clientMessages.filter((m) => m.label === "ch")[1], "ch (v3 server line)");
  assert.deepStrictEqual(ch9.args, [-1, null, "Round 2 starts", 2, null, null, null, null]);
  const playerLine = Buffer.alloc(325);
  playerLine.writeInt32LE(3, 0);
  playerLine[4] = 0;
  playerLine.write("Ada", 5, "utf8");
  playerLine.write("hi <b>", 37, "utf8");
  native.write(nativeFrame(9, playerLine));
  const ch9p = await waitFor(() => clientMessages.filter((m) => m.label === "ch")[2], "ch (v3 player line)");
  assert.deepStrictEqual(ch9p.args, [-1, "Ada", EV.escapeHtml("hi <b>"), 0, null, null, null, null]);

  // the per-tick player states (STATE, 6): our own is not sent back; another player's is a k with its 13-field record
  const stateOf = (uid: number, x: number) => {
    const b = Buffer.alloc(79);
    b.writeInt32LE(uid, 0);
    b.writeFloatLE(x, 13);
    b.writeFloatLE(2, 17);
    b.writeFloatLE(3, 21);
    b.writeFloatLE(0.5, 37); // pitch
    b.writeFloatLE(1, 41); // yaw
    b.writeFloatLE(0.25, 57); // step_val
    b[61] = 1; // on ground
    b.writeFloatLE(0.6, 53); // crouch
    b.writeInt32LE(2, 9); // loadout
    b.writeFloatLE(1, 49); // aim
    b[62] = 0; // no grapple
    b.writeFloatLE(42.4, 75); // ping
    return b;
  };
  native.write(nativeFrame(6, stateOf(1, 9))); // ours: not sent back
  native.write(nativeFrame(6, stateOf(9, 1.256)));
  const kMsg = await waitFor(() => clientMessages.find((m) => m.label === "k"), "k");
  assert.strictEqual((kMsg.args[0] as unknown[][]).length, 1);
  assert.deepStrictEqual((kMsg.args[0] as unknown[][])[0], [9, 1.26, 2, 3, 57, 29, 0.25, 1, 1, 2, 1, 0, 42]);
  assert.strictEqual(kMsg.args[1], 10);

  // SHOT (21): player 9 fires one ray: the observer's tracer "9" (its origin is 9's state: 1.26, 2, 3, height 11 - 3 * 0.6)
  const rayFrame = Buffer.alloc(31);
  rayFrame.writeInt32LE(9, 0);
  [4, 5, 6].forEach((v, i) => rayFrame.writeFloatLE(v, 8 + 4 * i));
  rayFrame[29] = 1; // trail
  native.write(nativeFrame(21, rayFrame));
  const tracer = await waitFor(() => clientMessages.find((m) => m.label === "9"), "9 (the tracer)");
  assert.deepStrictEqual(tracer.args, [[9, 4, 5, 6, null, null, 1, 1.3, 2, 9.2, 3, 0, 0]]);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("SHOT"), undefined, "the shot does not stop the session");

  // our own SPAWN while we wait to join (not active): the add record for us, then v10's death for us (KILL_PLAYER "3"),
  // which starts the respawn countdown on the client
  const addsForMeBeforeDead = clientMessages.filter((m) => m.label === "0" && (m.args[0] as unknown[])[1] === 1).length;
  native.write(nativeFrame(4, spawnBuffer({ uid: 1, isYou: true, active: false, classIndex: 4, team: 0, health: 100, maxHealth: 100,
    position: [1, 2, 3], direction: [0, 0], name: "me", secondary: -1, hat: -1, body: -1, hair: -1, skinColor: -1, dye: -1, waist: -1,
    face: -1, head: -1, back: -1, shoe: -1, pet: -1, wrist: -1, charms: [-1, -1], playerCard: -1, headMlt: 1 })));
  const countdown = await waitFor(() => clientMessages.find((m) => m.label === "3"), "3 (the death for the countdown)");
  assert.deepStrictEqual(countdown.args, [1, 0, 0, [], null, false]);
  // right after the countdown starts, "frs": the client calls enterGame(true) and then sends "en" (checked in section 5)
  const frs = await waitFor(() => clientMessages.find((m) => m.label === "frs"), "frs (the enter game after the countdown)");
  assert.deepStrictEqual(frs.args, [true]);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("SPAWN"), undefined, "a waiting joiner's spawn does not stop the session");
  // our player is added at "en", not at the dead SPAWN: the client's init (which fetches the map) must not see a player before it ran
  assert.strictEqual(clientMessages.filter((m) => m.label === "0" && (m.args[0] as unknown[])[1] === 1).length, addsForMeBeforeDead,
    "our dead SPAWN adds no player (the add is for players in play)");

  // LEAVE (20): player 9 disconnects: the observer's "2" [9], and the session stays up (the next packets still go through)
  const leaveFrame = Buffer.alloc(4);
  leaveFrame.writeInt32LE(9, 0);
  native.write(nativeFrame(20, leaveFrame));
  const gone = await waitFor(() => clientMessages.find((m) => m.label === "2"), "2 (the leave)");
  assert.deepStrictEqual(gone.args, [9]);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("LEAVE"), undefined, "a leave does not stop the session");
  native.write(nativeFrame(6, stateOf(11, 2.5))); // a player still in the game: its state still comes through
  await waitFor(() => clientMessages.filter((m) => m.label === "k").length >= 2 ? true : undefined, "k after the leave");

  // 6a. the owner's state (RECON 110) -> l, one argument (the array)
  const reconFrame = Buffer.alloc(89);
  reconFrame.writeInt32LE(4, 0);
  reconFrame.writeFloatLE(7, 4);
  native.write(nativeFrame(110, reconFrame));
  const l = await waitFor(() => clientMessages.find((m) => m.label === "l"), "l");
  assert.strictEqual(l.args.length, 1);
  assert.strictEqual((l.args[0] as unknown[]).length, 26);
  assert.strictEqual((l.args[0] as unknown[])[0], 4);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("RECON"), undefined);

  // CHEAT (60): one chat command's flag and on / off is the v10 label's one argument (v10.js 220244-220264)
  for (const [flag, label, on] of [[0, "noc", 1], [1, "unl", 1], [2, "fh", 1], [7, "frz", 1], [1, "unl", 0]] as [number, string, number][]) {
    const before = clientMessages.filter((m) => m.label === label).length;
    native.write(nativeFrame(60, Buffer.from([flag, on])));
    const msg = await waitFor(() => clientMessages.filter((m) => m.label === label)[before], `CHEAT ${flag} -> ${label}`);
    assert.deepStrictEqual(msg.args, [on === 1], `CHEAT ${flag} -> ${label} ${on}`);
  }
  // the pickup (128): inat with no player (sid -1), the pickup flag, the state; the match vote state (181): mv [vo, mts]
  const pickupFrame = Buffer.alloc(26);
  pickupFrame.writeInt32LE(4, 0);
  pickupFrame.writeInt32LE(17, 4);
  pickupFrame[8] = 1;
  native.write(nativeFrame(128, pickupFrame));
  const inat = await waitFor(() => clientMessages.find((m) => m.label === "inat"), "inat");
  assert.deepStrictEqual(inat.args, [-1, null, null, 4, 17, true, true, null]);
  const voteFrame = Buffer.alloc(170);
  voteFrame[0] = 1; // one card
  voteFrame.writeInt8(0, 1); // ours
  voteFrame.writeInt32LE(0, 2);
  voteFrame.writeInt32LE(5, 6);
  voteFrame.writeUInt16LE(1, 10);
  native.write(nativeFrame(181, voteFrame));
  const mv = await waitFor(() => clientMessages.find((m) => m.label === "mv"), "mv");
  assert.deepStrictEqual(mv.args, [{ [socketIo.args[0] as string]: ["0,5", 1] }, ["0,5"]]);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("PICKUP"), undefined);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("MATCH_VOTE_STATE"), undefined);
  // KrunkNative's account rights (EXT 0x8001) have no v10 message: consumed, nothing sent, no stop
  const rights = Buffer.alloc(6);
  rights.writeUInt16LE(T.EXT_ACCOUNT_RIGHTS, 0);
  native.write(nativeFrame(254, rights));
  await sleep(50);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("EXT 0x8001"), undefined);
  // PLAYER_INFO (34): KN's account record for the nametags, after SPAWN; no v10 message: consumed, no stop
  native.write(nativeFrame(34, Buffer.alloc(21)));
  await sleep(50);
  assert.strictEqual(v10Proxy.untranslatedSeen.get("native 34"), undefined);

  // borders (3) has no v10 label: it still stops the translation, named CHEAT (NATIVE_NAMES)
  native.write(nativeFrame(60, Buffer.from([3, 1])));
  await waitFor(() => v10Proxy.untranslatedSeen.get("CHEAT"), "stop at CHEAT borders");

  // 5. ENTER_GAME -> ENT: the client's "en" array becomes the KN join packet
  const en: unknown[] = new Array(T.EN_LENGTH).fill(-1);
  en[T.EN.CLASS] = 4;
  en[T.EN.SECONDARY] = -1;
  en[T.EN.SPRAY] = -1;
  en[T.EN.SKINS] = [-1, -1];
  en[T.EN.MELEE] = -1;
  en[T.EN.LAG_COMP] = 1;
  en[T.EN.ATTACH] = -1;
  en[T.EN.SPECTATE] = 0;
  en[T.EN.CHALLENGE] = 0;
  en[T.EN.ANON] = 0;
  en[T.EN.CHARMS] = [-1, -1];
  sendV10("en", en, 16, 18, false, false, false, false, false, "", false);
  const ent = await waitFor(() => nativeFrames.find((f) => f.type === T.NATIVE.ENT), "ENT");
  assert.deepStrictEqual(ent.payload, T.encodeEnter(en));
  assert.strictEqual(ent.payload.readInt32LE(0), 4);

  // 6. chat -> CHAT_SEND
  sendV10("ct", "hello there");
  const chat = await waitFor(() => nativeFrames.find((f) => f.type === T.NATIVE.CHAT_SEND), "CHAT_SEND");
  assert.deepStrictEqual(chat.payload, EV.encodeChatSend("hello there"));

  // 6b. the input window (q): the official client's frames, sent to the native input batch (111): seq 5 and 6, shoot
  sendV10("q", 0, 5, "1616", 2, [100, -200, 5, 2], { "0-4": 3, "0-5": 1, "0-6": 0, "0-7": 0, "0-8": 0, "0-9": 0, "0-10": 0, "0-11": 0, "0-12": 0, "0-13": 0, "0-14": 0 });
  const batch = await waitFor(() => nativeFrames.find((f) => f.type === T.NATIVE_MORE.INPUT_BATCH), "INPUT_BATCH");
  assert.strictEqual(batch.payload[0], 2);
  assert.strictEqual(batch.payload.readInt32LE(1), 5);
  assert.strictEqual(batch.payload.readInt32LE(1 + 24), 6);
  assert.strictEqual(batch.payload.readUInt8(1 + 20), T.NET_INPUT.SHOOT); // frame 0's flags: 20 bytes into the frame

  // 7. native PING -> PONG, echoed
  (nativeSocket as net.Socket).write(Buffer.from([T.NATIVE.PING, 0, 4, 0x78, 0x56, 0x34, 0x12]));
  const pong = await waitFor(() => nativeFrames.find((f) => f.type === T.NATIVE.PONG), "PONG");
  assert.deepStrictEqual([...pong.payload], [0x78, 0x56, 0x34, 0x12]);

  // 8. the stop is final: one init, one stop, and no init after it
  assert.strictEqual(v10Proxy.untranslatedSeen.get("CHEAT"), 1);
  assert.strictEqual(clientMessages.filter((m) => m.label === "init").length, 1);
  assert.strictEqual(clientMessages.filter((m) => m.label === "gv").length, 1);

  ws.close();
  await sleep(50);
  v10Proxy.closeAllSessions(true);
  bridge.close();
  nativeServer.close();
  console.info("v10Proxy test: ok (connect, LOAD->HELLO, the chunked map INIT, gv, asnd/rsnd, t, ENTER_GAME->ENT, chat, PING->PONG, then the stop)");
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
