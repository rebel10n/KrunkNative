#!/usr/bin/env python3
"""Nametags of bots in an offline FFA match: visible when they should be, never a random flip.

  python3 -I client/tests/nametag_visibility_e2e.py BUILD_DIR      (run from the repository root)

An offline Kanji game (headless client_bin, the sandbox server in-process, fixed 60 fps step) with
eight bots in Free for All. The player dies at 10 s (`killed`: a KILL by a bot, so the death cam
follows that bot and its tag stays up until the next visibility pass) and again at 60 s (`con kill`,
a suicide), each followed by a respawn once the death delay allows (the respawn command stays
pending until then).

KRUNKNATIVE_NAMETAG_LOG writes one line per frame and other player (client/src/nametags.c
nt_trace): the frame's own inputs (alive, active, has a mesh, in view of the camera, line of sight
from the player: Game.canSee), the tag's state (`visible` = the nameVisRate pass's decision,
`shown` = the tag as drawn), and the pass's last line-of-sight result (`pass_los`, with its time).
The checks, on alive frames only:
  1. every drawn tag is what the same frame says: shown == visible && in view && active && mesh
     && not hidden by the menu (no flip without a cause);
  2. the pass's line-of-sight result matches the frame's line of sight once that changed at least
     0.35 s earlier (the pass runs every 0.2 s of wall time, so it may lag by one period, but no
     more); a stale pass result beyond that is a bug;
  3. no tag is drawn for a bot without a sight line after the state has settled (sticky tags);
  4. the check can see tags: bots drawn in view with a sight line, and a killer's tag up while the
     player is dead (the death-cam path that must clear on respawn ran).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it), and in a
worktree KRUNKNATIVE_ASSETS_DIR pointing at the checkout's assets (the font files are untracked).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import collections
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-nametag-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
TOLERANCE_S = 0.35  # the nameVisRate pass (0.2 s) plus a frame of slack

# time from `play`: eight bots join at 0.2 s; the player dies at 10 s (`killed`) and again at 60 s
# (`con kill`, after the first respawn); each respawn is retried by the input script until the
# death delay allows it
SCRIPT = """0 play
200 con bot_add 8
10000 killed
22000 respawn
60000 con kill
84000 respawn
110000 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(timeout=900):
    script = os.path.join(WORK, "nametag.script")
    with open(script, "w") as f:
        f.write(SCRIPT)
    log = os.path.join(WORK, "nametag.log")
    trace = os.path.join(WORK, "nametags.jsonl")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_NAMETAG_LOG": trace,
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    return log, trace


def load(trace):
    rows = []
    if not os.path.exists(trace):
        return rows
    with open(trace) as f:
        for line in f:
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass
    return rows


def analyse(rows):
    # the player's spawns and deaths from the alive flag
    transitions = []
    prev = None
    for r in rows:
        if r["me_active"] != prev:
            transitions.append((r["t"], r["me_active"]))
            prev = r["me_active"]
    alive_spells = sum(1 for _, a in transitions if a == 1)
    dead_spells = sum(1 for _, a in transitions if a == 0)

    # global changes (the player's death / respawn, the menu) that reset every bot at once
    global_change = []
    last_key = None
    for r in rows:
        key = (r["me_active"], r["hidden"])
        if key != last_key:
            global_change.append(r["t"])
            last_key = key

    def near_global(t):
        return any(0.0 <= t - g < TOLERANCE_S for g in global_change)

    # Only alive frames are judged: while the player is dead the killer's tag is up on purpose
    # (v10 B8epi1 on kill), and the pass does not run.
    by_uid = collections.defaultdict(list)
    for r in rows:
        if r["me_active"] == 1:
            by_uid[r["uid"]].append(r)

    identity, lag, sticky = [], [], []
    shown_total = shown_in_view_los = pass_seen = 0
    for uid, seq in by_uid.items():
        last_los, last_change_t, prev_act = None, -1e9, None
        for r in seq:
            los = r["los"] == 1
            act = bool(r["active"] and r["mesh"])
            # the pass only decides for a bot that is active with a model: a bot that is inactive,
            # or has just come back, has no fresh pass result (the clock restarts then)
            if last_los is None or los != last_los or act != prev_act:
                last_change_t = r["t"]
            last_los, prev_act = los, act
            settled = r["t"] - last_change_t >= TOLERANCE_S and not near_global(r["t"])
            shown = r["shown"] == 1
            shown_total += shown
            shown_in_view_los += shown and los and bool(r["in_view"])
            # 1. every drawn tag is what this frame's own pass result and projection say
            expect_shown = bool(r["visible"] and r["in_view"] and r["active"] and r["mesh"] and not r["hidden"])
            if shown != expect_shown:
                identity.append((uid, r["f"], round(r["t"], 3)))
            # 2. the pass's line-of-sight result matches the frame's, once the state has settled
            if act and r["pass_los"] >= 0:
                pass_seen += 1
                if (r["pass_los"] == 1) != los and settled:
                    lag.append((uid, r["f"], round(r["t"], 3), r["pass_los"], r["los"], round(r["t"] - r["pass_t"], 3)))
            # 3. no tag without a sight line after the state has settled
            if shown and not los and settled and act and not r["hidden"]:
                sticky.append((uid, r["f"], round(r["t"], 3)))
    return {"transitions": transitions, "alive_spells": alive_spells, "dead_spells": dead_spells,
            "identity": identity, "lag": lag, "sticky": sticky, "shown_total": shown_total,
            "shown_in_view_los": shown_in_view_los, "pass_seen": pass_seen, "bots": len(by_uid),
            "killer_dead_shown": sum(1 for r in rows if r["me_active"] == 0 and r["shown"] == 1)}


try:
    log, trace = run()
    rows = load(trace)
    check("the trace exists and the player spawned and died", rows and sum(1 for r in rows if r["me_active"] == 1) > 0,
          f"{len(rows)} bot-frames in {trace}")
    if rows:
        a = analyse(rows)
        check("the player spawned, died and spawned again twice", a["alive_spells"] >= 3 and a["dead_spells"] >= 2,
              f"alive spells {a['alive_spells']}, dead spells {a['dead_spells']}")
        check("eight bots were tracked", a["bots"] == 8, f"{a['bots']} other players in the trace")
        check("every drawn tag matches its pass result and projection in the same frame (no random flip)", not a["identity"],
              f"{len(a['identity'])} mismatches, e.g. {a['identity'][:4]}" if a["identity"] else
              f"{a['shown_total']} drawn bot-frames, all explained")
        check("the pass's line-of-sight result matches the frame's once settled (within one pass period)", not a["lag"],
              f"{len(a['lag'])} stale, e.g. {a['lag'][:3]}" if a["lag"] else f"{a['pass_seen']} pass results checked")
        check("no tag outlives its line of sight or the death (sticky)", not a["sticky"],
              f"{len(a['sticky'])} sticky, e.g. {a['sticky'][:4]}" if a["sticky"] else "every drawn tag had a sight line")
        check("the check can see tags (bots drawn in view with line of sight)", a["shown_in_view_los"] > 0,
              f"{a['shown_in_view_los']} bot-frames, {a['shown_total']} drawn in total")
        check("the killer's tag was up while the player was dead (the death-cam path ran)", a["killer_dead_shown"] > 0,
              f"{a['killer_dead_shown']} dead-frames with a tag drawn")
    else:
        check("the trace has rows", False, f"no trace at {trace}")
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("nametag_visibility_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
