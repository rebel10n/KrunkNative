#!/usr/bin/env python3
"""The players' shadow casters under shadowsDynamic and highResShad, end to end (client/src/shadow.c).

  python3 -I client/tests/shadow_casters_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7 (deob-10.0.7.js line numbers): head, body and legs cast with dynamicShadows (:206746, :206770, :206791);
genArms gives the arms `castShadow = !firstPerson && highResShad` (:206820, the third argument is !fullModel,
:174006 / :174018) and the gun's loadMesh takes `shadows: highResShad` (:174143), which loadMesh makes castShadow
(:207421). The first-person viewmodel is on layer 1 (:174260-174262), which the sun's shadow camera does not see
(:69767-69768): our own arms and gun never cast in first person. Without shadowsDynamic the map renders once after a
change (toggleShadowAuto, :205966-205971) and the players are no per-frame casters.

Headless offline Kanji runs (client_bin, the sandbox server in-process, a fixed 60 fps step, a private
XDG_CONFIG_HOME) with three bots that stand still (bot_stop); the input script's `shadows` probe logs the last frame's drawable casters per
player (body / arms / guns), first in first person, then after `con thirdperson`:
  1. shadows + shadowsDynamic + highResShad: every bot casts its body and arms, and some bot its gun; in first
     person our arms and gun do not cast, in third person they do.
  2. highResShad off: the bots' and our third-person body still casts, no arms and no gun.
  3. shadowsDynamic off: no player is a caster.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it), and the checkout's
assets (in a worktree the untracked font / model folders, or KRUNKNATIVE_ASSETS_DIR).
Prints PASS/FAIL lines; exit code 0 only when everything passed, 77 = skipped (no client_bin).
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CLIENT = os.path.join(BUILD, "client_bin")
if not os.path.exists(CLIENT):
    print(f"needs client_bin in {BUILD}: skip")
    sys.exit(77)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-shadow-casters-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

# bot_stop: the bots stand still and never shoot, so we stay alive (an inactive player is no caster)
SCRIPT = """0 play
100 con bot_stop on
200 con bot_add 3
4000 shadows @fp
4100 con thirdperson
6000 shadows @tp
6100 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, settings, timeout=300):
    root = os.path.join(WORK, name)
    store = os.path.join(root, "cfg", "krunknative", "knSettings.json")
    os.makedirs(os.path.dirname(store), exist_ok=True)
    with open(store, "w") as f:
        json.dump(settings, f)
    script = os.path.join(root, "input.script")
    with open(script, "w") as f:
        f.write(SCRIPT)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(root, "cfg"),
                "XDG_STATE_HOME": os.path.join(root, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    log = os.path.join(root, "client.log")
    with open(log, "w") as out:
        p = subprocess.Popen([CLIENT, "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        text = f.read()
    probes = {}
    for m in re.finditer(r"\[input_script\] shadows t=\d+ (@\S+) me=(-?\d+)(.*)", text):
        players = {int(u): {"body": int(b), "arms": int(a), "guns": int(g)}
                   for u, b, a, g in re.findall(r" (\d+):body=(\d+),arms=(\d+),guns=(\d+)", m.group(3))}
        probes[m.group(1)] = (int(m.group(2)), players)
    return probes, text


def split(probe):
    me, players = probe
    return players.get(me), {u: c for u, c in players.items() if u != me}


def main():
    on = {"shadows": True, "shadowsDynamic": True, "highResShad": True}

    # 1. highResShad: the full models' arms and guns cast, our first-person viewmodel does not
    probes, text = run("high-res", on)
    check("high-res: both probes logged", "@fp" in probes and "@tp" in probes, str(sorted(probes)))
    if "@fp" in probes and "@tp" in probes:
        me_fp, bots = split(probes["@fp"])
        check("high-res: three bots are casters", len(bots) == 3, str(bots))
        check("high-res: every bot's head, body and legs cast", bots and all(c["body"] > 0 for c in bots.values()), str(bots))
        check("high-res: every bot's arms cast (genArms castShadow, :206820)", bots and all(c["arms"] > 0 for c in bots.values()),
              str(bots))
        check("high-res: a bot's gun casts (loadMesh shadows: highResShad, :174143 / :207421)",
              any(c["guns"] > 0 for c in bots.values()), str(bots))
        check("high-res: in first person our arms and gun do not cast (layer 1, :174260)",
              me_fp is not None and me_fp["arms"] == 0 and me_fp["guns"] == 0, str(me_fp))
        me_tp, _ = split(probes["@tp"])
        check("high-res: in third person our arms and gun cast (a full model)",
              me_tp is not None and me_tp["arms"] > 0 and me_tp["guns"] > 0 and me_tp["body"] > 0, str(me_tp))

    # 2. without highResShad only the default body casts
    probes, text = run("low-res", dict(on, highResShad=False))
    check("low-res: both probes logged", "@fp" in probes and "@tp" in probes, str(sorted(probes)))
    if "@fp" in probes and "@tp" in probes:
        _, bots = split(probes["@fp"])
        check("low-res: every bot's body casts, no arms and no gun",
              len(bots) == 3 and all(c["body"] > 0 and c["arms"] == 0 and c["guns"] == 0 for c in bots.values()), str(bots))
        me_tp, _ = split(probes["@tp"])
        check("low-res: our third-person body casts, no arms and no gun",
              me_tp is not None and me_tp["body"] > 0 and me_tp["arms"] == 0 and me_tp["guns"] == 0, str(me_tp))

    # 3. without shadowsDynamic no player is a per-frame caster
    probes, text = run("static", dict(on, shadowsDynamic=False))
    check("static: both probes logged", "@fp" in probes and "@tp" in probes, str(sorted(probes)))
    check("static: no player casters", all(not players for _, players in probes.values()), str(probes))

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
