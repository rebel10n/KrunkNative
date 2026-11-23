#!/usr/bin/env python3
"""The Battle Pass window on its --render-scenario fixture (client/src/ui/battlepass_ui.c).

The fixture (tools/uidiff/scenarios/rewards-bp-*.json: state.account.battlepass) must reach the
window: an active-account change used to wipe it, leaving "Loading Battle Pass". These checks read
the scenario's state line and require the rendered PNG, for the free track, the VIP track with
claimed cards, and the Challenges tab.

  python3 -I client/tests/bp_scenario_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ROOT
BIN = os.path.join(BUILD, "client_bin")
SCRATCH = os.path.expanduser("~/.cache/krunknative/bp/e2e-%d" % os.getpid())
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def scenario(name, expect, more=()):
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_DEBUG="1", XDG_CONFIG_HOME=os.path.join(SCRATCH, "cfg"))
    png = os.path.join(SCRATCH, name + ".png")
    os.makedirs(SCRATCH, exist_ok=True)
    r = subprocess.run([BIN, "--render-scenario", os.path.join(SRC, "tools/uidiff/scenarios", name + ".json"), png],
                       cwd=SRC, env=env, capture_output=True, text=True, timeout=180)
    log = r.stdout + r.stderr
    check(name + ": client_bin exits 0", r.returncode == 0, log[-800:] if r.returncode else "")
    check(name + ": the pass reaches the window", expect in log,
          next((l for l in log.splitlines() if "battlepass" in l), "no battlepass log line"))
    check(name + ": a frame was written", os.path.isfile(png) and os.path.getsize(png) > 50000)
    for want in more:
        check(name + ": " + want, want in log, next((l for l in log.splitlines() if "[battlepass] home" in l), "no home log line"))


def main():
    if not os.path.isfile(BIN):
        print("bp_scenario_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scenario("rewards-bp-home", "battlepass: items=100 tier=0 bpp=1000 claimed=20001 tab=0",
             ["[battlepass] home: level 4/50 complete=0 skip=open"])
    scenario("rewards-bp-vip", "battlepass: items=100 tier=1 bpp=1000 claimed=20001,21001,21004 tab=0")
    # v10.0.7's Skip button stays open on the last level: blockClick only once every level's BPP is paid
    # (deob-10.0.7.js:219256-219259, :219375); 300 * 49 + 10 BPP is level 50 of 50
    scenario("rewards-bp-lastlevel", "battlepass: items=100 tier=1 bpp=14710 claimed=20001,21001,21004 tab=0",
             ["[battlepass] home: level 50/50 complete=0 skip=open"])
    scenario("rewards-bp-chal", "battlepass: items=100 tier=0 bpp=1000 claimed=20001 tab=1")
    failed = sum(1 for _, ok, _ in results if not ok)
    print("bp_scenario_e2e: %d checks, %d failed" % (len(results), failed))
    subprocess.run(["rm", "-rf", SCRATCH], check=False)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
