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

// ---- MODE_STATE (40): the joiner's mode snapshot, and OBJECT_STATE (100): the join's object list.
// Layouts from shared/include/shared.h (NetModeStatePacket, NetObjectStatePacket), little endian.
function modeStateBuffer(o: { teamScores: [number, number]; lives: number; locked: number; message: string;
  objective: number; moveLock: number; flagCount: number }): Buffer {
  const b = Buffer.alloc(347);
  b.writeInt32LE(0, 0); // mode_id
  b.writeInt32LE(o.teamScores[0], 4);
  b.writeInt32LE(o.teamScores[1], 8);
  b.writeInt32LE(o.objective, 76); // active_objective
  b.writeInt32LE(-1, 80); // objective_weapon
  b[84] = o.moveLock;
  b[85] = o.locked;
  b.writeInt32LE(o.lives, 86);
  b[90] = o.flagCount; // flag_count; flags (8 x 20 bytes) follow at 91
  b.write(o.message, 251, "utf8"); // message[96]
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
  const ffa = T.mapModeState(modeStateBuffer({ teamScores: [5, 7], lives: 3, locked: 1, message: "wt", objective: -1, moveLock: 0, flagCount: 0 }));
  assert.strictEqual(ffa.stop, undefined);
  assert.deepStrictEqual(ffa.messages, [["ts", [1, 5]], ["ts", [2, 7]], ["lv", [3]], ["lock", [true]], ["gmsg", ["wt"]]]);
  const quiet = T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: -1, locked: 0, message: "", objective: -1, moveLock: 0, flagCount: 0 }));
  assert.deepStrictEqual(quiet.messages, [["ts", [1, 0]], ["ts", [2, 0]]]); // no lives, not locked, no message
  assert.match(T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: 0, locked: 0, message: "", objective: -1, moveLock: 0, flagCount: 1 })).stop || "", /flag/);
  assert.match(T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: 0, locked: 0, message: "", objective: 2, moveLock: 0, flagCount: 0 })).stop || "", /objective/);
  assert.match(T.mapModeState(modeStateBuffer({ teamScores: [0, 0], lives: 0, locked: 0, message: "", objective: -1, moveLock: 1, flagCount: 0 })).stop || "", /lock/);
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

  // 3. LOAD -> HELLO: protocol 1.4.2, Krunker v10 ruleset bit
  sendV10("load");
  const hello = await waitFor(() => nativeFrames.find((f) => f.type === T.NATIVE.HELLO), "HELLO");
  assert.deepStrictEqual([...hello.payload], [1, 4, 2, 4]);
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
  init.writeInt32LE(1, 7); // mode index
  init.writeUInt16LE(30, 11); // tick rate
  init.writeInt32LE(1, 13); // client id
  init[17] = 3; // ruleset
  assert.ok(nativeSocket);
  const native = nativeSocket as net.Socket;
  native.write(nativeFrame(25, mapInfo)); // MAP_INFO
  native.write(nativeFrame(183, configChunk(0, configBytes.subarray(0, cut)))); // GAME_CONFIG 1
  native.write(nativeFrame(183, configChunk(cut, configBytes.subarray(cut)))); // GAME_CONFIG 2
  native.write(nativeFrame(182, networkData)); // NETWORK_DATA
  native.write(nativeFrame(T.NATIVE.INIT, init)); // INIT, before the map bytes
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
  assert.strictEqual(args[1], 1); // modeIndex
  assert.deepStrictEqual(args[6], settings); // config: the GAME_CONFIG settings
  assert.strictEqual(args[9] && (args[9] as { data: unknown }).data, mapJson); // customMapData.data: the map JSON text
  assert.deepStrictEqual(args[12], { snf: 10, tf: 30, sf: 1 }); // networkData
  assert.strictEqual(args[13], true);

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
  native.write(nativeFrame(40, modeStateBuffer({ teamScores: [5, 7], lives: 3, locked: 0, message: "", objective: -1, moveLock: 0, flagCount: 0 })));
  await waitFor(() => clientMessages.filter((m) => m.label === "ts").length >= 2 ? true : undefined, "ts");
  const lv = await waitFor(() => clientMessages.find((m) => m.label === "lv"), "lv");
  assert.deepStrictEqual(lv.args, [3]);
  assert.deepStrictEqual(clientMessages.filter((m) => m.label === "ts").map((m) => m.args), [[1, 5], [2, 7]]);
  native.write(nativeFrame(100, objectStateBuffer(4, 0, 0))); // do 4
  native.write(nativeFrame(100, objectStateBuffer(9, 2, 1))); // gte 9 open
  await waitFor(() => clientMessages.find((m) => m.label === "gte"), "gte");
  assert.deepStrictEqual(clientMessages.find((m) => m.label === "do")?.args, [4]);
  assert.deepStrictEqual(clientMessages.find((m) => m.label === "gte")?.args, [9, true]);

  // the mode sync (149) is the next packet with no translation yet: the translation stops there
  native.write(nativeFrame(149, Buffer.alloc(4 * 4 + 4 + 1 + 4 * 4 + 4 + 4 + 4 + 4)));
  await waitFor(() => v10Proxy.untranslatedSeen.get("MODE_SYNC_V7"), "stop at MODE_SYNC_V7");

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

  // 7. native PING -> PONG, echoed
  (nativeSocket as net.Socket).write(Buffer.from([T.NATIVE.PING, 0, 4, 0x78, 0x56, 0x34, 0x12]));
  const pong = await waitFor(() => nativeFrames.find((f) => f.type === T.NATIVE.PONG), "PONG");
  assert.deepStrictEqual([...pong.payload], [0x78, 0x56, 0x34, 0x12]);

  // 8. the stop is final: one init, one stop, and no init after it
  assert.strictEqual(v10Proxy.untranslatedSeen.get("MODE_SYNC_V7"), 1);
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
