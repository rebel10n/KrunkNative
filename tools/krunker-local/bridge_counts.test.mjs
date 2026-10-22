// The counting rule behind the playable report's bridge counts and the page's per-round lines (bridge_counts.mjs).
//   node --test tools/krunker-local/bridge_counts.test.mjs   (make test runs it)
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { bridgeLabelCounter, pageRounds } from "./bridge_counts.mjs";

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "bridge-counts-"));
const file = path.join(dir, "bridge.out");
process.on("exit", () => fs.rmSync(dir, { recursive: true, force: true }));
const line = (sid, text) => `[v10-proxy] ${sid} ${text}\n`;
const obj = (m) => Object.fromEntries(m);

test("counts what the bridge sent (-> label) and what it got (<- label) apart", () => {
  fs.writeFileSync(file, [
    "[v10-bridge] v10 client ws://127.0.0.1:1/ws -> native 127.0.0.1:2 (lobby KN:local)\n",
    line("RArHT", "-> io-init"), line("RArHT", "<- load"), line("RArHT", "-> init"),
    line("RArHT", "-> start"), line("RArHT", "-> start"), line("RArHT", "<- en"),
    line("RArHT", "native MAP_DATA (506 B)"), // a native packet: no arrow, not counted
    line("RArHT", "-> end"),
  ].join(""));
  const sent = bridgeLabelCounter(file);
  const got = bridgeLabelCounter(file, "<-");
  assert.deepEqual(obj(sent()), { "io-init": 1, init: 1, start: 2, end: 1 });
  assert.deepEqual(obj(got()), { load: 1, en: 1 });
});

test("a later call counts only the lines added since the last one", () => {
  fs.writeFileSync(file, line("RArHT", "-> init"));
  const counter = bridgeLabelCounter(file);
  assert.equal(counter().get("init"), 1);
  fs.appendFileSync(file, line("RArHT", "-> init") + line("RArHT", "-> start"));
  const c = counter();
  assert.equal(c.get("init"), 2);
  assert.equal(c.get("start"), 1);
  assert.equal(counter().get("init"), 2, "no new lines: nothing is counted twice");
});

test("a line still being written counts once, when its newline arrives", () => {
  fs.writeFileSync(file, "[v10-proxy] RArHT -> ini");
  const counter = bridgeLabelCounter(file);
  assert.equal(counter().size, 0, "no newline yet: not a line");
  fs.appendFileSync(file, "t");
  assert.equal(counter().size, 0, "still no newline");
  fs.appendFileSync(file, "\n");
  assert.equal(counter().get("init"), 1, "the line is whole now: one init");
  assert.equal(counter().get("init"), 1, "and it is not counted again");
  fs.appendFileSync(file, line("RArHT", "-> start"));
  assert.equal(counter().get("start"), 1);
});

test("a multi-byte character split across two reads does not break the line", () => {
  const full = Buffer.from("[v10-proxy] RArHT é -> init\n", "utf8"); // é is c3 a9: the first read stops after c3
  const cut = full.indexOf(0xc3) + 1;
  fs.writeFileSync(file, full.subarray(0, cut));
  const counter = bridgeLabelCounter(file);
  assert.equal(counter().size, 0);
  fs.appendFileSync(file, full.subarray(cut));
  assert.equal(counter().get("init"), 1);
});

test("a file replaced by another (a new run's trace) starts the count again", () => {
  fs.writeFileSync(file, line("RArHT", "-> init") + line("RArHT", "-> init"));
  const counter = bridgeLabelCounter(file);
  assert.equal(counter().get("init"), 2);
  const next = file + ".next";
  fs.writeFileSync(next, line("abcde", "-> init") + line("abcde", "-> start") + line("abcde", "-> end") + "x".repeat(200) + "\n");
  fs.renameSync(next, file); // another inode, larger than what was read
  const c = counter();
  assert.equal(c.get("init"), 1, "the old run's two inits are not counted");
  assert.equal(c.get("start"), 1);
});

test("a file rewritten in place and grown past the last read starts again", () => {
  fs.writeFileSync(file, line("RArHT", "-> init") + line("RArHT", "-> init"));
  const counter = bridgeLabelCounter(file);
  assert.equal(counter().get("init"), 2);
  fs.writeFileSync(file, line("zzzzz", "-> end") + "y".repeat(300) + "\n"); // same inode, larger, different first bytes
  const c = counter();
  assert.equal(c.get("init"), undefined, "the old run's inits are gone");
  assert.equal(c.get("end"), 1);
});

test("a file that does not exist counts nothing and does not throw", () => {
  const counter = bridgeLabelCounter(path.join(dir, "missing.out"));
  assert.equal(counter().size, 0);
  assert.equal(bridgeLabelCounter(undefined)().size, 0);
});

test("pageRounds: the bridge's INITs and ENDs beside what the page did (the last playable run's shape)", () => {
  const s = (init, end, extra = {}) => ({ bridge: { init, end, start: init }, ...extra });
  const active = { game: { localActive: true } };
  const samples = [
    s(1, 0, active), s(1, 0, active), s(1, 0, { hud: { health: false } }),
    s(1, 1, { endUI: true }), s(1, 1, { endUI: true }),
    s(2, 1), s(2, 1), // INIT 2: the page never active
    s(2, 2), // END 2: no end screen
    s(3, 2, { game: { localActive: true }, inRound: false, hud: { health: false } }), // a one-sample blip, no round
  ];
  const r = pageRounds(samples);
  assert.deepEqual(r.inits, [
    { init: 1, samples: 5, active: 2, inRound: 0, hud: 0 },
    { init: 2, samples: 3, active: 0, inRound: 0, hud: 0 },
    { init: 3, samples: 1, active: 1, inRound: 0, hud: 0 },
  ]);
  assert.deepEqual(r.ends, [
    { end: 1, samples: 4, endUI: 2 }, // END 1 is the latest end through INIT 2's samples too
    { end: 2, samples: 2, endUI: 0 },
  ]);
});

test("pageRounds: a sample with no bridge count (before the first message) counts toward no round", () => {
  const r = pageRounds([{ game: { localActive: true }, inRound: true }, { bridge: { init: 0, end: 0 }, endUI: true }]);
  assert.deepEqual(r, { inits: [], ends: [] });
  assert.deepEqual(pageRounds(undefined), { inits: [], ends: [] });
});
