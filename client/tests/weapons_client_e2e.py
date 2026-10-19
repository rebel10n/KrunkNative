#!/usr/bin/env python3
"""The special weapons through the real client, as Krunker v10.0.2 plays them.

  python3 -I client/tests/weapons_client_e2e.py BUILD_DIR      (run from the repository root)

One offline Kanji game (headless client_bin, input script, fixed 60 fps step); the input script's
`state` / `sync` probes log the held weapon, ammo, charge, reload timer, grapple and canThrow.
  1. Slimer (the 7-kill streak picked in slot 1, called with key 1): it comes into hand with 2
     rounds, R does not reload it (v10 noReload, v10.js:101251), each click places a Slime Pad and
     the emptied Slimer leaves the loadout (removeOnEmpty: back to the Assault Rifle).
  2. Charge Rifle (the Object Manager's extra slot): held fire charges it (shootChrg, chrgTime
     700 ms, v10.js:101736), a partial charge reads between 0 and 0.7 s, and the full charge fires
     the blast by itself (fullChrgSht): the round is spent and the reload runs.
  3. Grappler: fired at a wall ~225 units away (low on it: the eye height after a streak differs by
     a fraction of a unit, enough to miss a roof edge) it hooks (grapple=1), reels the rope in at
     0.04 units / ms while the range is over 35 (v10.js:169773) and pulls the player along; a
     jump lets go (endGrapple).
  4. Combat Knife (the melee key): a swing keeps it, the aimed click throws it (canThrow 0).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-weapons-client-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
results = []

SCRIPT = """
0 menu
100 call selectStreak 4 0
300 play
500 con god
600 con bot_add 7
1800 con kill @bots
2300 con bot_kick all
2600 state
2700 down 1
2750 up 1
3200 state
3300 pose -343 -143 -28 0 -0.6
3400 down R
3450 up R
3700 state
3900 mdown left
3950 mup left
4300 state
4500 mdown left
4550 mup left
4900 state
5000 om add 28
5600 pose -343 -143 -28 0 0
5700 mdown left
5900 state
6300 state
6700 state
6800 mup left
6900 om add 28
7500 om add 20
8100 pose -343 -143 -28 -0.7 0.1
8200 mdown left
8300 state
9700 state
9800 mup left
9900 down SPACE
9950 up SPACE
10300 state
10400 om add 20
11000 pose -343 -143 -28 0 0
11100 down Q
11150 up Q
11600 state
11700 mdown left
11750 mup left
12100 sync
12200 mdown right
12700 mdown left
12750 mup left
12800 mup right
13200 sync
13300 quit
"""

VEC = r"(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)"
# the state probes in script order
LABELS = ["before", "slimer", "slimer-r", "slimer-1", "slimer-2", "charge-200", "charge-600", "charge-1000", "grapple-100",
          "grapple-1500", "grapple-jump", "knife"]
STATE_RE = re.compile(r"\[input_script\] state t=\d+ ()slot=(\d+)/(\d+) weapon=(.+?) menu=\d locked=\d pos=" + VEC +
                      r" ammo=(\d+) aim=\S+ ended=\d charge=([\d.]+) full=\d reload=([\d.]+) fire=\S+ grapple=(\d) rope=([\d.]+)")
SYNC_RE = re.compile(r"\[input_script\] sync\+ t=\d+ .*?throw=(-?\d)")


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run():
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(cfg)
    script = os.path.join(WORK, "weapons.script")
    with open(script, "w") as f:
        f.write(SCRIPT.strip() + "\n")
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS),
                "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("the client finishes the script", False, "timeout")
    with open(log, errors="replace") as f:
        return f.read()


def main():
    text = run()
    states = {}
    for label, m in zip(LABELS, STATE_RE.finditer(text)):
        states[label] = {"slot": int(m.group(2)), "size": int(m.group(3)), "weapon": m.group(4),
                              "pos": (float(m.group(5)), float(m.group(6)), float(m.group(7))), "ammo": int(m.group(8)),
                              "charge": float(m.group(9)), "reload": float(m.group(10)), "grapple": int(m.group(11)),
                              "rope": float(m.group(12))}
    syncs = [int(m.group(1)) for m in SYNC_RE.finditer(text)]
    s = states.get

    # 1. Slimer
    before, sl, slr, sl1, sl2 = s("before"), s("slimer"), s("slimer-r"), s("slimer-1"), s("slimer-2")
    check("slimer: the streak puts it in hand with 2 rounds", sl and sl["weapon"] == "Slimer" and sl["ammo"] == 2, str(sl))
    check("slimer: R does not reload it (noReload)", slr and slr["weapon"] == "Slimer" and slr["ammo"] == 2 and slr["reload"] == 0.0, str(slr))
    check("slimer: one click, one pad (1 round left)", sl1 and sl1["weapon"] == "Slimer" and sl1["ammo"] == 1, str(sl1))
    check("slimer: emptied, it leaves the loadout", sl2 and before and sl2["weapon"] == before["weapon"] and sl2["size"] == before["size"], f"{before} -> {sl2}")

    # 2. Charge Rifle
    c200, c600, c1000 = s("charge-200"), s("charge-600"), s("charge-1000")
    check("charge rifle: held fire charges it", c200 and c600 and c200["weapon"] == "Charge Rifle" and 0.0 < c200["charge"] < c600["charge"] <= 0.7,
          f"{c200 and c200['charge']} -> {c600 and c600['charge']}")
    check("charge rifle: the full charge fires the blast (round spent, reloading)", c1000 and c1000["ammo"] == 0 and c1000["reload"] > 0.0, str(c1000))

    # 3. Grappler
    g100, g1500, gj = s("grapple-100"), s("grapple-1500"), s("grapple-jump")
    check("grappler: hooks the wall", g100 and g100["weapon"] == "Grappler" and g100["grapple"] == 1 and g100["rope"] > 100.0, str(g100))
    if g100 and g1500:
        reeled = g100["rope"] - g1500["rope"]
        moved = math.dist(g100["pos"], g1500["pos"])
        check("grappler: reels in at 0.04 units / ms", 45.0 < reeled < 65.0, f"{reeled:.1f} in 1.4 s")
        check("grappler: and pulls the player along", moved > 30.0, f"{moved:.1f} units")
    else:
        check("grappler: reels in", False, "no state")
    check("grappler: a jump lets go", gj and gj["grapple"] == 0, str(gj))

    # 4. Combat Knife
    kn = s("knife")
    check("knife: the melee key takes it out", kn and kn["weapon"] == "Combat Knife", str(kn))
    check("knife: a swing keeps it, the aimed click throws it", syncs[-2:] == [1, 0], str(syncs))

    ok = all(results)
    print(f"{sum(results)}/{len(results)} passed")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
