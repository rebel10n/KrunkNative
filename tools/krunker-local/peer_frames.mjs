// The second player's part of the playability check (krunker-local.mjs playable): the v10 frames the first page's socket
// receives, decoded here, and the rows (2a)-(2d) read from them. Pure: no page, no bridge, no clock.
//   decodeV10Frame(bytes) -> { label, args } | null     msgpack [label, ...args] + the 2-byte ahNum trailer
//   peerRecord(frames, label, args, t)                    keeps what the rows read (add "0", leave "2", tracer "9", position "k")
//   peerRows(report, maxExc) -> [{ id, name, ok, detail }]
// The frame layouts are the bridge's (server/bridge/v10_translate.c): an add is ["0", [sid, uid, ...], 1] where sid is the
// socket id for our own player and the uid as a string for another player; a leave is ["2", uid]; a tracer is ["9", [shooter,
// hit x, y, z, normal yaw, pitch, trail, origin x, y, height, z, 0, 0]]; a position is ["k", [[uid, x, y, z, ...], ...], rate, t].

const CAP = 20000; // the frames kept per kind (a long run has a few thousand position records)

function readLen(b, i, bytes) {
  return bytes === 1 ? b[i] : bytes === 2 ? b.readUInt16BE(i) : b.readUInt32BE(i);
}
function readStr(b, i, len) {
  return [b.toString("utf8", i, i + len), i + len];
}
function readBin(b, i, lenBytes) {
  const len = readLen(b, i, lenBytes);
  const start = i + lenBytes;
  return [b.subarray(start, start + len), start + len];
}
function readArray(b, i, n) {
  const out = [];
  let at = i;
  for (let k = 0; k < n; k++) {
    const [v, next] = readValue(b, at);
    out.push(v);
    at = next;
  }
  return [out, at];
}
function readMap(b, i, n) {
  const out = {};
  let at = i;
  for (let k = 0; k < n; k++) {
    const [key, afterKey] = readValue(b, at);
    const [v, next] = readValue(b, afterKey);
    out[String(key)] = v;
    at = next;
  }
  return [out, at];
}

// one msgpack value at offset i: [value, next offset]. Extension types are not read (the frames the check needs have none).
function readValue(b, i) {
  const t = b[i];
  if (t <= 0x7f) return [t, i + 1];
  if (t >= 0xe0) return [t - 0x100, i + 1];
  if (t >= 0x80 && t <= 0x8f) return readMap(b, i + 1, t & 0x0f);
  if (t >= 0x90 && t <= 0x9f) return readArray(b, i + 1, t & 0x0f);
  if (t >= 0xa0 && t <= 0xbf) return readStr(b, i + 1, t & 0x1f);
  switch (t) {
    case 0xc0: return [null, i + 1];
    case 0xc2: return [false, i + 1];
    case 0xc3: return [true, i + 1];
    case 0xc4: return readBin(b, i + 1, 1);
    case 0xc5: return readBin(b, i + 1, 2);
    case 0xc6: return readBin(b, i + 1, 4);
    case 0xca: return [b.readFloatBE(i + 1), i + 5];
    case 0xcb: return [b.readDoubleBE(i + 1), i + 9];
    case 0xcc: return [b[i + 1], i + 2];
    case 0xcd: return [b.readUInt16BE(i + 1), i + 3];
    case 0xce: return [b.readUInt32BE(i + 1), i + 5];
    case 0xcf: return [Number(b.readBigUInt64BE(i + 1)), i + 9];
    case 0xd0: return [b.readInt8(i + 1), i + 2];
    case 0xd1: return [b.readInt16BE(i + 1), i + 3];
    case 0xd2: return [b.readInt32BE(i + 1), i + 5];
    case 0xd3: return [Number(b.readBigInt64BE(i + 1)), i + 9];
    case 0xd9: return readStr(b, i + 2, b[i + 1]);
    case 0xda: return readStr(b, i + 3, b.readUInt16BE(i + 1));
    case 0xdb: return readStr(b, i + 5, b.readUInt32BE(i + 1));
    case 0xdc: return readArray(b, i + 3, b.readUInt16BE(i + 1));
    case 0xdd: return readArray(b, i + 5, b.readUInt32BE(i + 1));
    case 0xde: return readMap(b, i + 3, b.readUInt16BE(i + 1));
    case 0xdf: return readMap(b, i + 5, b.readUInt32BE(i + 1));
    default: throw new Error(`msgpack type 0x${t.toString(16)} is not read`);
  }
}

// A v10 frame from the page's socket (the bytes of one binary message): [label, ...args] + the 2-byte trailer. null when the
// bytes are not one such frame (the check counts those as undecoded, it does not guess).
export function decodeV10Frame(bytes) {
  if (!bytes || bytes.length < 3) return null;
  const body = bytes.subarray(0, bytes.length - 2);
  try {
    const [v, end] = readValue(body, 0);
    if (end !== body.length || !Array.isArray(v) || typeof v[0] !== "string") return null;
    return { label: v[0], args: v.slice(1) };
  } catch (e) {
    return null;
  }
}

// Keeps what the rows read, from one frame the first page's socket received at t (ms since the run's start).
export function peerRecord(f, label, args, t) {
  const push = (kind, rec) => { if (f[kind].length < CAP) f[kind].push(rec); };
  const a0 = args[0];
  if (label === "0" && Array.isArray(a0) && typeof a0[1] === "number") {
    push("zero", { t, sid: String(a0[0]), uid: a0[1] });
  } else if (label === "2" && typeof a0 === "number") {
    push("two", { t, uid: a0 });
  } else if (label === "9" && Array.isArray(a0) && typeof a0[0] === "number") {
    push("nine", { t, shooter: a0[0], origin: { x: a0[7], y: a0[8], height: a0[9], z: a0[10] } });
  } else if (label === "k" && Array.isArray(a0)) {
    for (const r of a0) if (Array.isArray(r) && typeof r[0] === "number") push("k", { t, uid: r[0], x: r[1], y: r[2], z: r[3] });
  }
}

// The rows (2a)-(2d) for a report of `krunker-local.mjs playable` (report.peer). A run without a second player gives no rows.
export function peerRows(r, maxExc = 0) {
  const p = r.peer;
  if (!p || !p.enabled) return [];
  const f = p.frames1 || { zero: [], two: [], nine: [], k: [] };
  const excAt1 = p.excAt1 || [];
  const sec = (ms) => `${(ms / 1000).toFixed(1)}s`;
  const rows = [];
  const add = (id, name, ok, detail) => rows.push({ id, name, ok: !!ok, detail });
  const ids = [
    ["2a", "second player's add arrives, its positions reach the first page"],
    ["2b", "second player's shot reaches the first page as tracer 9 with its uid"],
    ["2c", "second player's leave reaches the first page as label 2 with its uid, no exception"],
    ["2d", "second player's join gives no exception on either page"],
  ];
  if (!p.joined || p.uid == null) {
    const why = p.error ? `error: ${p.error}` : p.joined ? "its own add never arrived (no uid)" : "did not join a round";
    for (const [id, name] of ids) add(id, name, false, `second player ${why}`);
    return rows;
  }
  const uid = p.uid;
  const sid = String(uid);

  // (2a) the add: the first page's socket got the "0" for this uid, under its uid as the sid; the first page lists two players
  // in a round; the positions ("k", keyed by uid) of this uid moved
  const zero = f.zero.find((e) => e.uid === uid && e.sid === sid);
  const pageSample = (r.samples || []).find((s) => s.inRound && s.game && s.game.players >= 2);
  const ks = f.k.filter((e) => e.uid === uid);
  const xs = ks.map((e) => e.x), zs = ks.map((e) => e.z);
  const span = (v) => (v.length ? Math.max(...v) - Math.min(...v) : 0);
  const moved = ks.length >= 2 && (span(xs) > 0.5 || span(zs) > 0.5);
  add("2a", ids[0][1], zero && pageSample && moved,
    `add "0" for sid ${sid} at ${zero ? sec(zero.t) : "none"}; first page lists ${pageSample ? "2" : "<2"} players ` +
    `(${pageSample ? `at ${sec(pageSample.t)}` : "no round sample"}); "k" records for uid ${uid}: ${ks.length}, ` +
    `x ${xs.length ? xs[0].toFixed(2) : "-"} -> ${xs.length ? xs[xs.length - 1].toFixed(2) : "-"}, span x ${span(xs).toFixed(2)} z ${span(zs).toFixed(2)}`);

  // (2b) the shots: the peer's own page fired (its physObj count), and the first page got tracers with its uid as the shooter
  const first = (p.shotsAt || [])[0];
  const tracers = f.nine.filter((e) => e.shooter === uid && (first == null || e.t >= first - 500));
  const fired = p.shots || 0;
  add("2b", ids[1][1], fired > 0 && tracers.length > 0,
    `tracers 9 with shooter ${uid} on the first page: ${tracers.length}; the second player fired ${fired} (its own count)` +
    (tracers.length ? `, first at ${sec(tracers[0].t)}` : ""));

  // (2c) the leave: "2" [uid] after the peer left, and no exception on the first page in that window
  const left = p.leftAt;
  const leave = left == null ? null : f.two.find((e) => e.uid === uid && e.t >= left - 1000);
  const excLeave = left == null ? [] : excAt1.filter((t) => t >= left - 1000 && t <= left + 10000);
  add("2c", ids[2][1], leave && excLeave.length <= maxExc,
    `label 2 [${uid}] ${leave ? `at ${sec(leave.t)} (${((leave.t - left) / 1000).toFixed(1)}s after the leave)` : "not received"}; ` +
    `first page exceptions in the leave window: ${excLeave.length}`);

  // (2d) the join: no exception on the first page or the second page from its join start to a few seconds after it is in a round
  const jStart = p.joinStart ?? 0;
  const jEnd = (p.joinedAt ?? jStart) + 3000;
  const exc1 = excAt1.filter((t) => t >= jStart && t <= jEnd).length;
  const exc2 = (p.excAt || []).filter((t) => t >= jStart && t <= jEnd).length;
  add("2d", ids[3][1], exc1 <= maxExc && exc2 <= maxExc,
    `first page ${exc1} and second page ${exc2} exceptions during the second player's join (${sec(jStart)} to ${sec(jEnd)}), threshold ${maxExc}`);
  return rows;
}
