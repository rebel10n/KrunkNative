// The second player's frame decoder and rows (peer_frames.mjs): msgpack [label, ...args] + the 2-byte trailer, the records the
// rows read, and the rows over a report (PASS when the frames say so, FAIL when they do not).
//   node --test tools/krunker-local/peer_frames.test.mjs   (make test runs it)
import { test } from "node:test";
import assert from "node:assert/strict";
import { decodeV10Frame, peerRecord, peerRows } from "./peer_frames.mjs";

// msgpack bytes for the frames the rows read (the layouts in peer_frames.mjs)
const f32 = (v) => { const b = Buffer.alloc(5); b[0] = 0xca; b.writeFloatBE(v, 1); return b; };
const str = (s) => Buffer.concat([Buffer.from([0xa0 | s.length]), Buffer.from(s)]);
const trailer = Buffer.from([0x01, 0x02]);
const frame = (...parts) => Buffer.concat([...parts, trailer]);

test("a position frame decodes to its label and records", () => {
  // ["k", [[12, 10, 2, 20, ...4 more]], 10, 500] : the record is [uid, x, y, z, ...] (fixarray of 4 here)
  const rec = Buffer.concat([Buffer.from([0x94, 0x0c]), f32(10), f32(2), f32(20)]);
  const bytes = frame(Buffer.from([0x94]), str("k"), Buffer.from([0x91]), rec, Buffer.from([0x0a, 0xcd, 0x01, 0xf4]));
  const d = decodeV10Frame(bytes);
  assert.ok(d, "the frame decodes");
  assert.equal(d.label, "k");
  assert.equal(d.args[0][0][0], 12);
  assert.equal(d.args[0][0][1], 10);
  assert.equal(d.args[0][0][3], 20);
  assert.equal(d.args[1], 10);
  assert.equal(d.args[2], 500);
});

test("a frame that is not msgpack [label, ...] is null, not a guess", () => {
  assert.equal(decodeV10Frame(Buffer.from([0x01, 0x02, 0x03])), null, "an int, not an array");
  assert.equal(decodeV10Frame(Buffer.from([0x91, 0x01, 0x01, 0x02])), null, "an array whose first item is no label");
  assert.equal(decodeV10Frame(Buffer.from([0x92, 0xa1, 0x32, 0x01, 0x01, 0x02, 0x09])), null, "bytes left over after the value");
});

test("the records the rows read: add, leave, tracer and positions", () => {
  const f = { zero: [], two: [], nine: [], k: [] };
  // add of another player: sid "12", uid 12
  peerRecord(f, "0", [[ "12", 12, 1, 2, 3, "peer", 4, 1, 2, 3 ], 1], 100);
  peerRecord(f, "2", [12], 200);
  // tracer: [shooter, hit x, y, z, normal yaw, pitch, trail, origin x, y, height, z, 0, 0]
  peerRecord(f, "9", [[12, 4, 5, 6, null, null, 1, 11.5, 2, 11, 20, 0, 0]], 300);
  peerRecord(f, "k", [[[12, 10, 2, 20], [7, 1, 2, 3]], 10, 0], 400);
  assert.deepEqual(f.zero, [{ t: 100, sid: "12", uid: 12 }]);
  assert.deepEqual(f.two, [{ t: 200, uid: 12 }]);
  assert.deepEqual(f.nine, [{ t: 300, shooter: 12, trail: 1, origin: { x: 11.5, y: 2, height: 11, z: 20 } }]);
  assert.deepEqual(f.k, [{ t: 400, uid: 12, x: 10, y: 2, z: 20 }, { t: 400, uid: 7, x: 1, y: 2, z: 3 }]);
});

// a report of a round where the second player (uid 12) joins, moves, shoots twice and leaves; the first page sees all of it
function report(over = {}) {
  const base = {
    samples: [{ t: 1000, inRound: true, game: { players: 2 } }],
    peer: {
      enabled: true, joined: true, uid: 12, socketId: "abcdefghijkl", joinStart: 500, joinedAt: 4000, leftAt: 30000,
      shotsAt: [20000, 20600], shots: 2, excAt: [], excAt1: [],
      frames1: {
        zero: [{ t: 3000, sid: "12", uid: 12 }],
        two: [{ t: 30500, uid: 12 }],
        nine: [{ t: 20100, shooter: 12, origin: {} }, { t: 20700, shooter: 12, origin: {} }],
        k: [{ t: 21000, uid: 12, x: 10, y: 2, z: 20 }, { t: 22000, uid: 12, x: 12.5, y: 2, z: 20 }],
      },
    },
  };
  return { ...base, ...over, peer: { ...base.peer, ...(over.peer || {}) } };
}

test("a run where the second player's frames say so: every row PASSES", () => {
  const rows = peerRows(report(), 0);
  assert.deepEqual(rows.map((x) => x.id), ["2a", "2b", "2c", "2d"]);
  assert.ok(rows.every((x) => x.ok), JSON.stringify(rows, null, 1));
});

test("no second player (KN_PLAYABLE_PLAYERS=1): no rows", () => {
  assert.deepEqual(peerRows({ peer: { enabled: false } }), []);
  assert.deepEqual(peerRows({}), []);
});

test("the second player did not join: all four rows FAIL with that reason", () => {
  const rows = peerRows(report({ peer: { joined: false, uid: null } }));
  assert.equal(rows.length, 4);
  assert.ok(rows.every((x) => !x.ok && /did not join/.test(x.detail)));
});

test("positions that do not move fail (2a), and so does a tracer that is not the shooter's", () => {
  const still = report({ peer: { frames1: { zero: [{ t: 3000, sid: "12", uid: 12 }], two: [{ t: 30500, uid: 12 }],
    nine: [{ t: 20100, shooter: 9, origin: {} }], k: [{ t: 21000, uid: 12, x: 10, y: 2, z: 20 }, { t: 22000, uid: 12, x: 10, y: 2, z: 20 }] } } });
  const rows = peerRows(still);
  assert.equal(rows.find((x) => x.id === "2a").ok, false, "still positions");
  assert.equal(rows.find((x) => x.id === "2b").ok, false, "a tracer from shooter 9, not 12");
});

test("an exception on the first page in the leave window fails (2c); one in the join window fails (2d)", () => {
  const rows = peerRows(report({ peer: { excAt1: [30000, 5000] } }), 0);
  assert.equal(rows.find((x) => x.id === "2c").ok, false, "exception at 30 s, the leave at 30.5 s");
  assert.equal(rows.find((x) => x.id === "2d").ok, false, "exception at 5 s, inside the join (0.5 s to 7 s)");
  assert.equal(rows.find((x) => x.id === "2d").ok, false);
  const lenient = peerRows(report({ peer: { excAt1: [5000] } }), 1);
  assert.equal(lenient.find((x) => x.id === "2d").ok, true, "one exception is at the threshold of 1");
});
