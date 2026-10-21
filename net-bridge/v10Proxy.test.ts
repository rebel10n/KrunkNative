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

  // 4. the native INIT: the translation stops (no init to the client), and the stop is recorded
  const initPayload = Buffer.alloc(18);
  initPayload[0] = 1;
  initPayload[1] = 4;
  initPayload[2] = 2;
  initPayload.writeInt32LE(0, 3); // map index
  initPayload.writeInt32LE(0, 7); // mode index
  initPayload.writeUInt16BE(30, 11); // tick rate
  initPayload.writeInt32LE(1, 13); // client id
  initPayload[17] = 3; // ruleset
  assert.ok(nativeSocket);
  const header = Buffer.from([T.NATIVE.INIT, 0, 18]);
  (nativeSocket as net.Socket).write(Buffer.concat([header, initPayload]));
  await waitFor(() => v10Proxy.untranslatedSeen.get(T.NATIVE.INIT), "INIT stop");
  await sleep(150);
  assert.strictEqual(clientMessages.some((m) => m.label === "init"), false);

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

  // 8. the stop is final: the INIT was the only untranslated packet, and no init reached the client
  assert.strictEqual(v10Proxy.untranslatedSeen.get(T.NATIVE.INIT), 1);
  assert.strictEqual(clientMessages.some((m) => m.label === "init"), false);

  ws.close();
  await sleep(50);
  v10Proxy.closeAllSessions(true);
  bridge.close();
  nativeServer.close();
  console.info("v10Proxy test: ok (connect, LOAD->HELLO, ENTER_GAME->ENT, chat, PING->PONG, INIT stops the translation)");
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
