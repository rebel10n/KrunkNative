// Unit checks of the bridge (webBridge.ts): /native admission (allowlist, close codes), the graceful
// stop's ERROR frame, the stop reason file and the frame cursor that keeps the frame out of the
// middle of the server's.
//   npm test            (node -r ts-node/register/transpile-only webBridge.test.ts)
import assert from "assert";
import fs from "fs";
import os from "os";
import path from "path";
import type { IncomingMessage } from "http";
import { isBrainPath } from "./brainProxy";
import { generatedAssetsVersion } from "./staticFiles";
import { admit, CLOSE_POLICY, errorFrame, scanFrames, stopMessage, type FrameCursor } from "./webBridge";

// admission (no KRUNK_NATIVE_TARGETS here: the default 127.0.0.1:21015-21078 plus any loopback)
const upgrade = (url: string) => ({ url, headers: {}, socket: { remoteAddress: "127.0.0.1" } }) as unknown as IncomingMessage;
assert.strictEqual(admit(upgrade("/native")), null);
assert.strictEqual(admit(upgrade("/native?target=127.0.0.1:21016")), null);
assert.strictEqual(admit(upgrade("/native?target=localhost:30000")), null);
for (const bad of ["/native?target=example.com:21015", "/native?target=10.0.0.1:21015", "/native?target=127.0.0.1", "/native?target=127.0.0.1:99999"]) {
  const refused = admit(upgrade(bad));
  assert.ok(refused, bad);
  assert.strictEqual(refused.close, CLOSE_POLICY, bad); // final for the client (net_web.c): no retry loop
  assert.strictEqual(refused.status, 403, bad);
  assert.strictEqual(refused.reason, "target not allowed", bad);
}

const frame = errorFrame("GAME UPDATING", "004004");
assert.strictEqual(frame.length, 3 + 104);
assert.strictEqual(frame[0], 30);
assert.strictEqual(frame.readUInt16BE(1), 104);
assert.strictEqual(frame.subarray(3, 3 + 96).toString("utf8").replace(/\0+$/, ""), "GAME UPDATING");
assert.strictEqual(frame.subarray(3 + 96).toString("utf8").replace(/\0+$/, ""), "004004");
const long = errorFrame("x".repeat(300), "0123456789");
assert.strictEqual(long.length, frame.length);
assert.strictEqual(long[3 + 95], 0);
assert.strictEqual(long[3 + 96 + 7], 0);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kn-bridge-test-"));
const file = path.join(dir, "reason");
assert.deepStrictEqual(stopMessage(""), { message: "Server restarting", code: "004006" });
assert.deepStrictEqual(stopMessage(file), { message: "Server restarting", code: "004006" });
fs.writeFileSync(file, "update\n");
assert.deepStrictEqual(stopMessage(file), { message: "GAME UPDATING", code: "004004" });
const old = new Date(Date.now() - 16 * 60 * 1000);
fs.utimesSync(file, old, old);
assert.deepStrictEqual(stopMessage(file), { message: "Server restarting", code: "004006" });
fs.rmSync(dir, { recursive: true, force: true });

// two frames: [1][0 2][a b] [2][0 0], split at every offset
const stream = Buffer.from([1, 0, 2, 0x61, 0x62, 2, 0, 0]);
for (let cut = 0; cut <= stream.length; cut++) {
  const cursor: FrameCursor = { left: 0, header: [] };
  assert.strictEqual(scanFrames(cursor, stream.subarray(0, cut), false), cut);
  const end = scanFrames(cursor, stream.subarray(cut), true);
  const boundary = cut === 0 || cut === 5 || cut === 8 ? 0 : cut < 5 ? 5 - cut : 8 - cut;
  assert.strictEqual(end, boundary, `cut ${cut}`);
}
const inside: FrameCursor = { left: 0, header: [] };
scanFrames(inside, Buffer.from([1, 0, 9, 1]), false);
assert.strictEqual(scanFrames(inside, Buffer.from([2, 3]), true), -1);
assert.strictEqual(inside.left, 6);
console.log("webBridge tests OK");

// net-bridge --web proxies every brain route the client uses (the menu's streams, Community &
// Events, login, the user-asset mirror), and never the page's own files
for (const p of ["/mm/status", "/api/profile", "/social/ws", "/ai/chat", "/auth/login", "/streams/top", "/streams/thumb",
                 "/creators/events", "/user-assets/123/tex.png"]) assert.strictEqual(isBrainPath(p), true, p);
for (const p of ["/", "/index.html", "/assets/textures/a.png", "/native", "/client.wasm", "/streamsx", "/js/loader.js"])
  assert.strictEqual(isBrainPath(p), false, p);

// assets/version.txt without a deploy file: a 16-hex hash of the tree's names, sizes and mtimes (staticFiles.ts)
(async () => {
  const tree = fs.mkdtempSync(path.join(os.tmpdir(), "kn-version-test-"));
  fs.mkdirSync(path.join(tree, "textures"));
  fs.writeFileSync(path.join(tree, "textures", "a.png"), "one");
  const first = await generatedAssetsVersion(tree);
  assert.match(first, /^[0-9a-f]{16}$/);
  assert.strictEqual(await generatedAssetsVersion(tree), first); // once per process
  const other = fs.mkdtempSync(path.join(os.tmpdir(), "kn-version-test-"));
  fs.mkdirSync(path.join(other, "textures"));
  fs.writeFileSync(path.join(other, "textures", "a.png"), "three");
  assert.notStrictEqual(await generatedAssetsVersion(other), first); // a changed size changes it
  await assert.rejects(generatedAssetsVersion(path.join(tree, "missing")));
  fs.rmSync(tree, { recursive: true, force: true });
  fs.rmSync(other, { recursive: true, force: true });
})().catch((err) => {
  console.error(err);
  process.exit(1);
});
// the server list (the Servers window's finder, ?finder=/servers) is exact: its sub-paths are the brain's
// heartbeats and stay off the page's origin
for (const p of ["/servers", "/finder/servers"]) assert.strictEqual(isBrainPath(p), true, p);
for (const p of ["/servers/heartbeat", "/servers/", "/finder/servers/submit", "/serversx"]) assert.strictEqual(isBrainPath(p), false, p);
