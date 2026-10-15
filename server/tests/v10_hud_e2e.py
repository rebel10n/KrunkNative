#!/usr/bin/env python3
"""The in-game HUD on the Krunker v10.0.2 page (ui/, client/src/ui/hud_v10.c), end to end.

  python3 -I server/tests/v10_hud_e2e.py BUILD_DIR      (run from the repository root)

One headless client_bin --offline (Kanji, a sandbox bot) on the v10 page (ui/)
plays a match through the menu, spectating (setSpect: v10 shows the Spectate switch to accounts
only, hideSpectateIfGuestUser), spawn, the scoreboard (Tab), a death, respawn, ESC,
playing again and the end of the match, and checks what the player sees at each step against what
v10.0.2 shows there (doc/ref/krunker-v10/ingame/, the offline v10 sandbox):
  - the chat (#chatUIHolder): hidden on the menu for a guest, up in game, dead, spectating and on
    the end screen (hideChatIfGuestUser);
  - #leaderboardHolder always up in game and while spectating (v10's switchLeaderboard);
  - the player HUD (health bar, weapon slots, ammo, timer) alive, gone on the death screen;
  - the Tab scoreboard in v10's <thead>/<tbody> table;
  - the bots stop once the kill feed is checked (bot_stop): endGame shows the end screen only to an
    active player or a spectator, so a bot killing the player before the END left the death card /
    the menu up instead (endUI=0, the end-screen checks failed now and then);
  - the end screen's v10 Performance tab (Combat Earnings: 2 cells, Combat Record: 5) and a
    guest's signup incentive, whose REGISTER opens v10's login / register modal (loginOrRegister).
Frames of every step go next to the matching v10 reference into WORK/frames/<step>.sbs.png
(reference | KrunkNative; the references are over flat black, the run over the 3D world).
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
"""
import glob
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
REF = os.path.join(REPO, "doc", "ref", "krunker-v10", "ingame")
WORK = tempfile.mkdtemp(prefix="kn-v10-hud-e2e-", dir=os.path.expanduser("~/.cache"))
results = []

# (time, step, element=expected ..., reference to frame next to it)
SCRIPT = """
0 menu
300 con bot_add 1
1000 check @menu chatUIHolder=0 instructionHolder=1 menuHolder=1
1000 frame @menu ingame-pause-esc
1100 call setSpect 1
1500 play
3500 check @spect spectateUI=1 chatUIHolder=1 leaderboardHolder=1 weapHolder=0 bottomLeftHolder=0
3500 frame @spect ingame-spectate
4000 esc
4800 check @spect-esc menuHolder=1 spectateUI=0 chatUIHolder=0
5000 call setSpect 0
5400 play
7500 check @alive chatUIHolder=1 chatHolder=1 leaderboardHolder=1 bottomLeftHealthBar=1 weapHolder=1 ammoHolder=1 timerHolder=1 deathUIHolder=0 spectateUI=0
7500 frame @alive ingame-plain
7600 tab
7900 check @tab centerLeaderDisplay=1 leaderboardHolder=1
7900 nodes @tab-head thead
7900 nodes @tab-rows tbody
7900 frame @tab ingame-scoreboard
8100 up TAB
8400 check @tab-up centerLeaderDisplay=0
8600 con kill
9600 check @dead deathUIHolder=1 weapHolder=0 bottomLeftHolder=0 chatUIHolder=1
9600 frame @dead ingame-death-self
10000 respawn
14000 check @respawn weapHolder=1 deathUIHolder=0 chatUIHolder=1 leaderboardHolder=1
14200 con bot_add 3
15500 con kill @bots
16200 check @kills killFeed=1 leaderboardHolder=1
16200 nodes @feed .killfeedItem
16200 frame @kills ingame-killfeed
16300 con bot_stop on
17000 esc
17800 check @esc menuHolder=1 chatUIHolder=0 instructionHolder=1 inGameUI=0
18000 play
20000 check @back weapHolder=1 menuHolder=0 chatUIHolder=1
20500 con end
25000 check @endanim endUI=1 inGameUI=0 chatUIHolder=1
25000 frame @endanim ingame-endscreen
32000 check @results endUI=1 endTabbedView=1 matchEndSignupIncentives=1 chatUIHolder=1
32000 nodes @perf-stats .crStat
32000 nodes @perf-titles .endTitle
32000 nodes @perf-old #gameFundsLayout
32000 frame @results ingame-endscreen-late
32500 clicksel .signup-rewards-register-button
33500 check @register accName=1 knV10AuthSwitch=1
33500 frame @register -
34000 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def expand(script):
    lines, expects, frames = [], [], []
    for raw in script.strip().splitlines():
        t, cmd, *rest = raw.split()
        if cmd == "check":
            label = rest[0].lstrip("@")
            expects.append((label, dict(kv.split("=", 1) for kv in rest[1:])))
            lines.append(f"{t} vis @{label} " + " ".join(kv.split("=", 1)[0] for kv in rest[1:]))
        elif cmd == "frame":
            label = rest[0].lstrip("@")
            frames.append((label, rest[1]))
            lines.append(f"{t} frames {WORK}/{label} 1")
        elif cmd == "tab":
            lines.append(f"{t} down TAB")
        elif cmd == "esc":
            lines += [f"{t} down ESC", f"{float(t) + 50:g} up ESC"]
        else:
            lines.append(raw)
    return "\n".join(lines) + "\n", expects, frames


def stalled(lines, limit_ms=1500):
    """A check that ran limit_ms later than scheduled: a loaded host stalled a frame, and the
    script's events after it ran bunched together."""
    due = {}
    for raw in SCRIPT.strip().splitlines():
        t, cmd, *rest = raw.split()
        if cmd == "check":
            due[rest[0].lstrip("@")] = int(t)
    for line in lines:
        m = re.search(r"\[input_script\] vis t=(\d+) @(\S+)", line)
        if m and m.group(2) in due and int(m.group(1)) - due[m.group(2)] > limit_ms:
            return True
    return False


def main():
    for attempt in (1, 2):
        del results[:]
        ok, lines = run(attempt)
        if ok or attempt == 2 or not stalled(lines):
            break
        print(f"\n(the host stalled a frame by more than 1.5 s: running the match again)\n", flush=True)
    print(f"\n{sum(results)}/{len(results)} passed  (frames: {os.path.join(WORK, 'frames')})")
    sys.exit(0 if ok else 1)


def run(attempt):
    text, expects, frames = expand(SCRIPT)
    script = os.path.join(WORK, "hud.script")
    with open(script, "w") as f:
        f.write(text)
    log = os.path.join(WORK, f"client{attempt}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()
    check("the v10 page loaded", not any("windows.json missing" in l or "missing window file" in l for l in lines)
          and any("[input_script] vis" in l for l in lines), log)

    got = {}
    for line in lines:
        m = re.search(r"\[input_script\] vis t=\d+ @(\S+)(.*)", line)
        if m:
            got.setdefault(m.group(1), {}).update(kv.split("=", 1) for kv in m.group(2).split())
        m = re.search(r"\[input_script\] nodes t=\d+ @?(\S+) elements=\d+ match=(\d+)", line)
        if m:
            got.setdefault("nodes", {})[m.group(1)] = int(m.group(2))
    for label, want in expects:
        have = got.get(label, {})
        bad = {k: have.get(k, "no probe") for k, v in want.items() if have.get(k) != v}
        check(f"{label}: " + " ".join(f"{k}={v}" for k, v in want.items()), not bad, f"got {bad}" if bad else "")
    nodes = got.get("nodes", {})
    check("kills on bots: rows in #killFeed (under the leaderboard)", nodes.get("feed", 0) >= 1, f"{nodes.get('feed')} .killfeedItem")
    check("Tab scoreboard: v10's <thead> header rows and a <tbody> of players", nodes.get("tab-head") == 1 and nodes.get("tab-rows") == 1,
          f"{nodes.get('tab-head')} thead, {nodes.get('tab-rows')} tbody")
    check("end screen: v10's Performance tab (Combat Earnings 2 cells, Combat Record 5, no v7 funds card)",
          nodes.get("perf-stats") == 7 and nodes.get("perf-titles") == 2 and nodes.get("perf-old") == 0,
          f"{nodes.get('perf-stats')} .crStat, {nodes.get('perf-titles')} .endTitle (Combat Earnings / Combat Record), v7 card {nodes.get('perf-old')}")

    # frames next to the v10 references, for review (not pixel-asserted: the run draws the 3D world)
    out = os.path.join(WORK, "frames")
    os.makedirs(out, exist_ok=True)
    made = 0
    for label, ref in frames:
        shots = sorted(glob.glob(os.path.join(WORK, label + "_*.png")))
        if not shots:
            check(f"{label}: frame written", False)
            continue
        args = [os.path.join(REF, ref + ".png"), shots[-1]] if ref != "-" else [shots[-1]]
        if shutil.which("magick") and subprocess.call(["magick", *args, "+append", os.path.join(out, label + ".sbs.png")]) == 0:
            made += 1
    check("frames next to the v10 references", made == len(frames), out)

    return bool(results) and all(results), lines


if __name__ == "__main__":
    main()
