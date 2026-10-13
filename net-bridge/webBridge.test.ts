// Unit checks of the bridge's graceful stop (webBridge.ts): the ERROR frame, the stop reason file
// and the frame cursor that keeps the frame out of the middle of the server's.
//   npm test            (node -r ts-node/register/transpile-only webBridge.test.ts)
import assert from "assert";
import fs from "fs";
import os from "os";
import path from "path";
import { errorFrame, scanFrames, stopMessage, type FrameCursor } from "./webBridge";

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
