// What the bridge sent, and what the page did with it (tools/krunker-local/playable.sh, krunker-local.mjs playable).
//
// The bridge traces one line per message it sends the client ("[v10-proxy] <sid> -> <label>") and one per frame it gets
// from the client ("[v10-proxy] <sid> <- <label>"). bridgeLabelCounter counts those lines. A count is what the bridge
// sent: it says nothing about whether the page entered the round. pageRounds says that, per INIT and END the bridge sent,
// from the harness samples (the page's state, one sample about every second, each with the bridge's counts so far).
import fs from "node:fs";

// The counts of the lines that end in "<arrow> <label>" ("->" for what the bridge sent, "<-" for what it got), read from
// the trace file. The file is read from its first byte: the playable run starts the bridge with a fresh trace (playable.sh
// truncates it), so a counter created for the run counts that run only. Each call reads only what was added since the last
// one. A file that is replaced starts again from zero: another inode, shorter than what was read, or rewritten in place
// (its first bytes differ from the first bytes read). A line counts when its last characters are the label, so the
// bridge's other lines ("v10 client ... -> native host:port (lobby ...)") do not.
export function bridgeLabelCounter(file, arrow = "->") {
  const re = arrow === "->" ? / -> (\S+)$/ : / <- (\S+)$/;
  const counts = new Map();
  let pos = 0;
  let ident = null; // dev:ino of the file read so far
  let head = Buffer.alloc(0); // the first bytes of the file as read: a rewritten file no longer starts with them
  let partial = Buffer.alloc(0); // the bytes after the last newline (a line not yet complete)
  const restart = () => { pos = 0; head = Buffer.alloc(0); partial = Buffer.alloc(0); counts.clear(); };
  return () => {
    if (!file || !fs.existsSync(file)) return counts;
    const fd = fs.openSync(file, "r");
    try {
      const st = fs.fstatSync(fd);
      const id = `${st.dev}:${st.ino}`;
      const headOk = () => {
        if (!head.length) return true;
        const now = Buffer.alloc(head.length);
        return fs.readSync(fd, now, 0, now.length, 0) === head.length && now.equals(head);
      };
      if ((ident !== null && id !== ident) || st.size < pos || (pos > 0 && !headOk())) restart();
      ident = id;
      const buf = Buffer.alloc(st.size - pos);
      const got = fs.readSync(fd, buf, 0, buf.length, pos);
      if (!head.length && got) head = Buffer.from(buf.subarray(0, Math.min(got, 64)));
      pos += got;
      const data = Buffer.concat([partial, buf.subarray(0, got)]);
      const cut = data.lastIndexOf(0x0a);
      if (cut < 0) { partial = data; return counts; }
      partial = data.subarray(cut + 1);
      for (const line of data.subarray(0, cut).toString("utf8").split("\n")) {
        const m = re.exec(line);
        if (m) counts.set(m[1], (counts.get(m[1]) || 0) + 1);
      }
    } finally {
      fs.closeSync(fd);
    }
    return counts;
  };
}

// What the page did with each INIT and END the bridge sent. A sample belongs to the round of the bridge's count at that
// sample: bridge.init k is round k (its INIT was sent and no later one), bridge.end k is END k (the round that ended).
//   init k: active = samples with the local player active in the page's player list; inRound = samples with the page in the
//           round (the game instance open and the player active, see PLAY_SNAP_JS); hud = samples with the health box laid out.
//   end k:  endUI = samples with the page's end screen shown after END k.
// Samples without a bridge count (an old report, a sample before the first message) count toward no round.
export function pageRounds(samples) {
  const inits = new Map();
  const ends = new Map();
  for (const s of samples || []) {
    const b = s.bridge || {};
    const k = Number(b.init) || 0;
    if (k > 0) {
      const r = inits.get(k) || { init: k, samples: 0, active: 0, inRound: 0, hud: 0 };
      r.samples++;
      if (s.game && s.game.localActive) r.active++;
      if (s.inRound) r.inRound++;
      if (s.hud && s.hud.health) r.hud++;
      inits.set(k, r);
    }
    const e = Number(b.end) || 0;
    if (e > 0) {
      const r = ends.get(e) || { end: e, samples: 0, endUI: 0 };
      r.samples++;
      if (s.endUI) r.endUI++;
      ends.set(e, r);
    }
  }
  return { inits: [...inits.values()].sort((a, b) => a.init - b.init), ends: [...ends.values()].sort((a, b) => a.end - b.end) };
}
