#!/usr/bin/env node
// The playability assertions (a)-(h) over one report of `krunker-local.mjs playable`. Each assertion reads only the report
// (its samples, input window, frame window, end and next-round marks, and exceptions), never the page or the bridge.
// Prints one PASS or FAIL line per assertion, then the report path; exit status 0 only when every assertion passes.
// Under the PASS/FAIL lines it prints the bridge's counts beside the page's (pageRounds, bridge_counts.mjs): what the bridge
// sent and, per INIT and END, what the page did with it. Those lines are information, not an assertion.
//   node tools/krunker-local/playable-check.mjs <report.json> [max-exceptions]
import fs from "node:fs";
import { pageRounds } from "./bridge_counts.mjs";

const [file, maxArg] = process.argv.slice(2);
if (!file) {
  console.error("usage: playable-check.mjs <report.json> [max-exceptions]");
  process.exit(2);
}
const r = JSON.parse(fs.readFileSync(file, "utf8"));
const max = Number(maxArg ?? process.env.KN_PLAYABLE_MAX_EXC ?? 0);
const S = r.samples || [];
const sec = (ms) => `${(ms / 1000).toFixed(1)}s`;
const rows = [];
const add = (id, name, ok, detail) => rows.push({ id, name, ok: !!ok, detail });

// (a) init completes: a sample where the game is past its init, with its map loaded and a mode set
const aS = S.find((s) => s.game && !s.game.initInProgress && s.game.mapLoaded && s.game.modeSet && s.game.modeIndex !== null);
add("a", "init completes (initInProgress false, map loaded, mode set)", aS,
  aS ? `at ${sec(aS.t)}, mode ${aS.game.modeIndex}` : `no sample past init (joined: ${r.joined === true})`);

// (b) the HUD renders in the round: the health and ammo boxes are laid out, and a game canvas is visible
const bS = S.find((s) => s.inRound && s.hud && s.hud.health && s.hud.ammo && s.canvasVisible);
add("b", "HUD renders in the round (DOM: health, ammo, canvas)", bS,
  bS ? `at ${sec(bS.t)}` : "no in-round sample with the HUD and a visible canvas");

// (c) window.__kn_game is readable during the round
const cS = S.find((s) => s.inRound && s.gameExposed && s.game && s.game.players >= 1);
add("c", "window.__kn_game readable during the round", cS,
  cS ? `at ${sec(cS.t)}, ${cS.game.players} players listed` : "no in-round sample with the game object");

// (d) the local player's input reaches the server and produces state: W moves the player, the mouse turns it, a click fires
// (the local shot: the game's physObj call with us as the shooter; a trail is drawn only for weapons with one, so it is not asserted)
// (the bridge's "<- q" input frames and the server's "-> l" position samples)
let dOk = false;
let dDetail = "no in-round input window (the join did not reach a round)";
if (r.input && r.input.before && r.input.after) {
  const b = r.input.before;
  const a = r.input.after;
  const q = (a.bridge.q || 0) - (b.bridge.q || 0);
  const l = (a.bridge.l || 0) - (b.bridge.l || 0);
  const lb = b.game && b.game.local;
  const la = a.game && a.game.local;
  const move = lb && la ? Math.hypot(la.x - lb.x, la.z - lb.z) : 0;
  const look = lb && la && typeof lb.yaw === "number" && typeof la.yaw === "number" ? Math.abs(la.yaw - lb.yaw) : 0;
  const shots = (a.shots || 0) - (b.shots || 0);
  const ammo = lb && la ? lb.ammo - la.ammo : 0;
  dOk = q > 0 && l > 0 && move > 0.5 && look > 0.001 && shots > 0;
  dDetail = `input frames +${q}, position samples +${l}, move ${move.toFixed(2)}, look ${look.toFixed(3)}, ` +
    `own shots +${shots}, ammo ${ammo >= 0 ? "-" : "+"}${Math.abs(ammo)} (physCalls +${(a.physCalls || 0) - (b.physCalls || 0)})`;
}
add("d", "input reaches the server and makes state (move, look, fire)", dOk, dDetail);

// (e) the frame counter keeps advancing for 60 s: each 5 s slice of the frame window has frames in it
const fr = S.filter((s) => s.phase === "frames");
let eOk = false;
let eDetail = "no frame window";
if (fr.length > 1) {
  const start = fr[0].t;
  const slices = [];
  for (let k = 0; k < 12; k++) {
    const lo = fr.find((s) => s.t >= start + 5000 * k);
    const hi = fr.find((s) => s.t >= start + 5000 * (k + 1)) || fr[fr.length - 1];
    slices.push(lo && hi ? hi.frames - lo.frames : null);
  }
  const span = fr[fr.length - 1].t - start;
  eOk = r.joined === true && span >= 55000 && slices.every((x) => typeof x === "number" && x > 0);
  eDetail = `${sec(span)} sampled; frames per 5 s: ${slices.join(", ")}` + (r.joined === true ? "" : " (not in a round: menu frames only)");
}
add("e", "frame counter keeps advancing for 60 s", eOk, eDetail);

// (f) the round ends and the end screen is shown
// (f) must follow a round the local player was in: an end screen while still on the menu is not a round's end
const firstIn = S.findIndex((s) => s.inRound);
// (the end screen can come up in the frame window, after the round started: any phase counts)
const fS = firstIn < 0 ? null : S.find((s, i) => i > firstIn && s.endUI);
add("f", "round ends and the end screen is shown", fS,
  fS ? `end screen at ${sec(fS.t)}, bridge end labels ${fS.bridge.end}`
    : (firstIn < 0 ? "not reached (no in-round sample, so no round to end)" : "no end screen in the round window"));

// (g) the next round starts: a new init on the socket after the end screen, the local player active, the end screen gone
const base = fS ? fS.bridge.init : null;
const gS = base === null ? null : S.find((s) => s.phase === "next" && s.inRound && !s.endUI && s.bridge.init > base);
add("g", "next round starts with init, local player active", gS,
  gS ? `init ${base} -> ${gS.bridge.init} at ${sec(gS.t)}, ${gS.game.players} players`
    : (fS ? "no init after the end screen with an active local player" : "not reached (no end screen)"));

// (h) the page's exception count stays under the threshold; the first exception is listed with its location
const ex = r.exceptions || [];
const exCount = r.exceptionCount ?? ex.length;
const first = ex[0];
add("h", `exceptions at or under ${max}`, exCount <= max,
  `${exCount} exceptions` + (first ? `; first (${first.phase}, ${sec(first.t)}): ${first.text} at ${first.where}` +
    (first.frames && first.frames.length ? ` [${first.frames.join(" < ")}]` : "") : ""));

for (const x of rows) console.log(`${x.ok ? "PASS" : "FAIL"} (${x.id}) ${x.name}: ${x.detail}`);
// the bridge's counts beside the page's: messages sent, and for each INIT / END the samples in which the page used it
const sent = r.bridgeCounts || {};
console.log(`bridge sent (its trace, this run): init ${sent.init || 0}, start ${sent.start || 0}, end ${sent.end || 0}`);
const rounds = pageRounds(S);
for (const x of rounds.inits) {
  console.log(`  INIT ${x.init}: page player active in ${x.active}/${x.samples} samples, in round ${x.inRound}, HUD ${x.hud}`);
}
for (const x of rounds.ends) console.log(`  END ${x.end}: page end screen in ${x.endUI}/${x.samples} samples`);
const pageIn = rounds.inits.filter((x) => x.inRound > 0).length;
console.log(`rounds: the bridge sent ${sent.init || 0} INIT(s); the page was in a round for ${pageIn} of them`);
console.log(`report: ${file}`);
console.log(rows.every((x) => x.ok) ? "playable: PASS" : `playable: FAIL (${rows.filter((x) => !x.ok).map((x) => x.id).join(", ")})`);
process.exit(rows.every((x) => x.ok) ? 0 : 1);
