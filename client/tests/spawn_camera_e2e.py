#!/usr/bin/env python3
"""Every spawn of ours resets the camera the way Krunker does (client/src/main.c client_player_spawned).

  python3 -I client/tests/spawn_camera_e2e.py BUILD_DIR      (run from the repository root)

Krunker v7.1.4 / v10.0.2 on our spawn: "start" runs resetGameSessionState -> CONTROLS.reset(true)
(the look and RENDER.camera.rotation to 0, zoom 1, the death chase target and the spectate target
cleared), then the "0" player sync runs players.add -> player.init (xDire = yDire = 0, recoilAnim(Y),
landBobY, leanAnim*, aimVal, crouchVal, camChase from rest) and io_addPlayers sets xDire from the
spawn data and the camera from it (object.rotation.y = xDire, pchObjc.rotation.x = yDire = 0).
So the first frame after a spawn looks along the spawn's yaw, level, with no recoil, landing bob,
lean, ADS zoom or leftover chase / third-person easing, and it stays there while the mouse rests.

Offline Kanji runs (headless client_bin, input script, fixed 60 fps step) look somewhere odd (pitch
near +-90 degrees, yaw turned), fire and aim, then die and respawn:
  1. `con kill` while looking straight down, firing and aiming, then Play again (`respawn`);
  2. the same looking straight up with the third-person view on (its camera pulled in by the floor
     until the death: it has to start at its full distance after the spawn);
  3. a new round (`con end`): the next round's INIT and SPAWN.
The first spawn (Play from the menu) is checked in every run. `cam` lines (input_script.c) log the
camera on the first 5 frames after each spawn; KRUNKNATIVE_PACKET_LOG=1 gives the SPAWN's direction.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
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
WORK = tempfile.mkdtemp(prefix="kn-spawn-camera-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

CAM_RE = re.compile(r"\[input_script\] cam t=(\d+) (\S+) spawns=(\d+) active=(\d) rot=(\S+) pos=(\S+) eye=(\S+) dir=(\S+) "
                    r"zoom=(\S+) recoil=(\S+) land=(\S+) lean=(\S+) aim=(\S+) crouch=(\S+) shake=(\S+) chase=(\d) spect=(\d) "
                    r"tp=(\d) keep=(\S+)")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def vec(s):
    return tuple(float(x) for x in s.split(","))


def wrap(a):
    return math.atan2(math.sin(a), math.cos(a))


def run(name, script, timeout=240):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_PACKET_LOG": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    return log


def parse(log):
    """[(spawn packet, [cam frames after it])] per own SPAWN, and the probes logged by label."""
    spawns, probes, last_spawn = [], {}, None
    with open(log, errors="replace") as f:
        for line in f:
            line = ANSI_RE.sub("", line)
            if '"type":"spawn"' in line and "<=" in line:
                try:
                    pkt = json.loads(line[line.index("{"):])
                except ValueError:
                    continue
                if pkt.get("is_you") and pkt.get("active"):
                    last_spawn = pkt
                continue
            m = CAM_RE.search(line)
            if not m:
                continue
            cam = {"t": int(m.group(1)), "label": m.group(2), "spawns": int(m.group(3)), "active": int(m.group(4)),
                   "rot": vec(m.group(5)), "pos": vec(m.group(6)), "eye": vec(m.group(7)), "dir": vec(m.group(8)),
                   "zoom": float(m.group(9)), "recoil": vec(m.group(10)), "land": float(m.group(11)),
                   "lean": vec(m.group(12)), "aim": float(m.group(13)), "crouch": float(m.group(14)),
                   "shake": vec(m.group(15)), "chase": int(m.group(16)), "spect": int(m.group(17)),
                   "tp": int(m.group(18)), "keep": float(m.group(19))}
            if cam["label"].startswith("@spawn"):
                if cam["label"] == "@spawn0":
                    spawns.append((last_spawn, []))
                if spawns:
                    spawns[-1][1].append(cam)
            else:
                probes[cam["label"]] = cam
    return spawns, probes


def check_spawn(name, pkt, frames, third_person=False):
    if not pkt or len(frames) < 5:
        check(f"{name}: SPAWN packet and 5 logged frames", False, f"packet {pkt is not None}, {len(frames)} frames")
        return
    yaw = pkt["direction"][1]
    # player.init: yDire = 0; the spawn data carries only xDire (index 18)
    check(f"{name}: the SPAWN looks level (yDire 0)", abs(pkt["direction"][0]) < 1e-6, f"pitch {pkt['direction'][0]}")
    bad = []
    for c in frames:
        pitch = c["rot"][0] - c["shake"][0]
        dyaw = wrap(c["rot"][1] - c["shake"][1] - yaw)
        if abs(pitch) > 2e-3 or abs(dyaw) > 2e-3 or abs(c["rot"][2]) > 1e-6:
            bad.append(f"{c['label']} rot={c['rot']} (spawn yaw {yaw:.5f})")
    check(f"{name}: the camera looks along the spawn's yaw, pitch 0 (CONTROLS.reset + io_addPlayers)", not bad, "; ".join(bad))
    # (third person: the input look is aimed from the eye at what the crosshair ray hits, a KrunkNative
    # aim correction, so our xDire / yDire differ from the view's by design and the weapon lean
    # follows that first small turn)
    if not third_person:
        bad = []
        for c in frames:
            if abs(c["dir"][0]) > 2e-3 or abs(wrap(c["dir"][1] - yaw)) > 2e-3:
                bad.append(f"{c['label']} dir={c['dir']}")
        check(f"{name}: our look (xDire / yDire) is the spawn's", not bad, "; ".join(bad))
    first = frames[0]
    lean_max = 5e-3 if third_person else 1e-3
    rest = (abs(first["recoil"][0]) < 1e-4 and abs(first["recoil"][1]) < 1e-4 and abs(first["land"]) < 1e-4 and
            max(abs(v) for v in first["lean"]) < lean_max and first["aim"] == 0.0 and first["crouch"] == 0.0)
    check(f"{name}: no leftover recoil, landing bob, lean, aim or crouch (player.init)", rest,
          f"recoil={first['recoil']} land={first['land']} lean={first['lean']} aim={first['aim']} crouch={first['crouch']}")
    check(f"{name}: no death chase or spectate camera", all(not c["chase"] and not c["spect"] for c in frames))
    if third_person:
        keeps = [c["keep"] for c in frames]
        check(f"{name}: third person on", all(c["tp"] for c in frames))
        check(f"{name}: the third-person camera starts at its distance (no ease out from the death's)",
              keeps[0] > 0.0 and abs(keeps[0] - keeps[-1]) < 0.05, f"keep {keeps}")
    else:
        check(f"{name}: zoom 1 (RENDER.zoom(1))", all(abs(c["zoom"] - 1.0) < 1e-4 for c in frames), f"zoom {[c['zoom'] for c in frames]}")
        off = max(math.dist(c["pos"], c["eye"]) for c in frames)
        check(f"{name}: the camera sits at the eye (moveCam), no offset", off < 1e-3, f"max offset {off:.4f}")


def odd_before(name, probes, pitch_sign):
    c = probes.get("@before")
    ok = c and c["active"] and pitch_sign * c["dir"][0] > 1.4 and c["recoil"][0] > 1e-3
    check(f"{name}: before the death the look was odd and the recoil up", ok,
          f"dir={c['dir'] if c else None} recoil={c['recoil'] if c else None}")


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin", file=sys.stderr)
        return 2
    try:
        # 1. looking straight down (pitch clamps at -90), yaw turned, firing + aiming, `con kill`, Play again
        log = run("respawn", "0 play\n500 look 1.3 -1.6\n700 mdown left\n900 mdown right\n1100 cam @before\n1100 con kill\n"
                             "1150 mup left\n1150 mup right\n1400 respawn\n45000 quit\n")
        spawns, probes = parse(log)
        check("respawn: two spawns (Play, then Play again)", len(spawns) == 2, f"{len(spawns)}, log {log}")
        odd_before("respawn", probes, -1)
        for i, (pkt, frames) in enumerate(spawns[:2]):
            check_spawn(f"respawn spawn {i + 1}", pkt, frames)

        # 2. third person, looking straight up: the camera behind the shoulder sinks into the floor and is
        #    pulled in (third_person.c keep); the next life's camera starts at its full distance
        log = run("third", "0 play\n800 con thirdperson\n1200 look -2.2 1.6\n1500 mdown left\n"
                           "2300 cam @before\n2300 con kill\n2350 mup left\n2600 respawn\n45000 quit\n")
        spawns, probes = parse(log)
        check("third: two spawns", len(spawns) == 2, f"{len(spawns)}, log {log}")
        odd_before("third", probes, 1)
        if len(spawns) == 2:
            check_spawn("third spawn 1", *spawns[0])
            check_spawn("third spawn 2", *spawns[1], third_person=True)

        # 3. a new round: `con end`, the end screen, the next round's INIT + SPAWN
        log = run("round", "0 play\n500 look 2.5 1.6\n700 mdown left\n900 mdown right\n1100 cam @before\n1100 con end\n"
                           "1300 mup left\n1300 mup right\n60000 quit\n", timeout=300)
        spawns, probes = parse(log)
        check("round: a spawn in the next round", len(spawns) == 2, f"{len(spawns)}, log {log}")
        odd_before("round", probes, 1)
        for i, (pkt, frames) in enumerate(spawns[:2]):
            check_spawn(f"round spawn {i + 1}", pkt, frames)
    finally:
        if all(results) and not os.environ.get("KN_TEST_KEEP"):
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
