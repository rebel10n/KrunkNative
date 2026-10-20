#!/usr/bin/env python3
"""The Physics Gun keeps picking up and dragging after a drop, and a death never leaves a toast.

  python3 -I client/tests/physgun_cycle_e2e.py BUILD_DIR      (run from the repository root)

One offline Kanji game (headless client_bin, input script, fixed 60 fps step; the local player is a dev).
  1. Grab a spawned crate, carry it, drop it, pick it up again and drag it (the probe `pg` logs the gun's
     grab, the object under the crosshair, and the carried object's feet; `aimlast` turns the look onto the
     newest object, the crate, before each grab).
  2. Hold the grab, die (`con kill` with god off), and get the edit answer "Spawn in first" (the server's
     status 7 for an edit that was on its way: `omres 7`). The grab ends, and no toast is issued (`toast`
     counts ui_notify's toasts).
  3. Respawn, equip the Physics Gun again, spawn a crate and grab, drop and pick it up again.
The setup (a crate under the crosshair after each spawn) depends on the random spawn point, so a run whose
spawn does not put a takeable crate under the crosshair is retried (up to six times); a gun failure is not.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-physgun-cycle-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
results = []

SCRIPT = """
0 menu
300 play
500 con god
600 om physgun 0
650 look 0 0.5
900 om object 10
1000 pg @spawned
1050 aimlast
1060 pg @aimed
1100 mdown left
1200 pg @grab1
1300 look 0.03 0
1400 pg @carry
1450 look -0.03 0
1500 mup left
1600 pg @drop
1700 aimlast
1800 mdown left
1900 pg @pick2
1950 look 0.05 0
2000 pg @pick2drag
2050 look -0.05 0
2100 mup left
2200 pg @cycle-end
2300 aimlast
2350 mdown left
2400 pg @death-grab
2500 con ungod
2600 con kill
2800 pg @dead
2850 toast @before-answer
2900 omres 7
2950 toast @after-answer
3050 mup left
3100 respawn
6000 state
6050 om physgun 0
6070 look 0 0.5
6100 om object 10
6150 aimlast
6200 pg @respawn-spawned
6300 mdown left
6400 pg @respawn-grab
6450 mup left
6500 pg @respawn-drop
6550 aimlast
6600 mdown left
6700 pg @respawn-pick
6800 mup left
6900 quit
"""
PG_RE = re.compile(r"\[input_script\] pg t=\d+ @(\S+) grab=(\d) uid=(-?\d+) frozen=\d releasing=(\d) drag=\d sel=-?\d+ aim=(-?\d+) "
                   r"aimfeet=(\S+) hold=(\S+) edit=(\d) takes=(\d) gun=(-?\d) caps=\d+ shoot=(-?\d)")
TOAST_RE = re.compile(r"\[input_script\] toast t=\d+ @(\S+) n=(\d+)")


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run_once(attempt):
    cfg = os.path.join(WORK, f"cfg{attempt}")
    os.makedirs(cfg, exist_ok=True)
    script = os.path.join(WORK, f"physgun-cycle-{attempt}.script")
    with open(script, "w") as f:
        f.write(SCRIPT.strip() + "\n")
    log = os.path.join(WORK, f"client-{attempt}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, f"state{attempt}"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS),
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def parse(text):
    pg = {}
    for m in PG_RE.finditer(text):
        pg[m.group(1)] = {"grab": int(m.group(2)), "uid": int(m.group(3)), "releasing": int(m.group(4)), "aim": int(m.group(5)),
                          "aimfeet": m.group(6), "hold": m.group(7), "edit": int(m.group(8)), "takes": int(m.group(9)),
                          "gun": int(m.group(10)), "active": int(m.group(11))}
    toasts = {m.group(1): int(m.group(2)) for m in TOAST_RE.finditer(text)}
    return pg, toasts


def main():
    def setup_ok(pg):
        # both crates (the first life's and the next life's) must be takeable at the crosshair's aim
        return all(pg.get(k) and pg[k]["takes"] == 1 and pg[k]["edit"] == 1 for k in ("aimed", "respawn-spawned"))

    pg = {}
    toasts = {}
    for attempt in range(1, 7):
        pg, toasts = parse(run_once(attempt))
        if setup_ok(pg):
            break
        print(f"setup attempt {attempt}: a spawned crate is not takeable at the crosshair "
              f"(first {pg.get('aimed')}, next life {pg.get('respawn-spawned')}), retrying", flush=True)
    else:
        print(f"logs kept in {WORK}")
        return 1
    s = pg.get
    check("setup: the spawned crate is takeable", s("aimed")["takes"] == 1 and s("aimed")["edit"] == 1, str(s("aimed")))

    # 1. one grab, carry, drop, and the next grab picks it up again and drags it
    grab1, carry, drop = s("grab1"), s("carry"), s("drop")
    check("cycle: the first grab takes the crate", grab1 and grab1["grab"] == 1, str(grab1))
    check("cycle: the carry holds it (the crate is the one grabbed)", carry and carry["grab"] == 1 and carry["uid"] == grab1["uid"], str(carry))
    check("cycle: the drop releases it", drop and drop["grab"] == 0 and drop["releasing"] == 0, str(drop))
    pick2, pick2drag = s("pick2"), s("pick2drag")
    check("cycle: after the drop, the crosshair picks the crate up again", pick2 and pick2["grab"] == 1 and pick2["uid"] == grab1["uid"],
          str(pick2))
    check("cycle: the second grab drags it (its hold follows the look)",
          pick2 and pick2drag and pick2drag["grab"] == 1 and pick2drag["hold"] != pick2["hold"], f"{pick2 and pick2['hold']} -> "
          f"{pick2drag and pick2drag['hold']}")
    end = s("cycle-end")
    check("cycle: released again", end and end["grab"] == 0, str(end))

    # 2. death while holding the crate: the grab ends, and the answer does not toast
    dg, dead = s("death-grab"), s("dead")
    check("death: the grab is on when we die", dg and dg["grab"] == 1, str(dg))
    check("death: we are dead, and the grab ended with the life", dead and dead["active"] == 0 and dead["grab"] == 0, str(dead))
    before, after = toasts.get("before-answer"), toasts.get("after-answer")
    check("death: 'Spawn in first' toasts nothing (the toast count holds)", before is not None and before == after, f"{before} -> {after}")

    # 3. the next life: the gun works again
    rs, rg, rd, rp = s("respawn-spawned"), s("respawn-grab"), s("respawn-drop"), s("respawn-pick")
    check("respawn: a crate spawns and the gun takes it", rs and rs["aim"] >= 0 and rs["takes"] == 1, str(rs))
    check("respawn: the grab works after a respawn", rg and rg["grab"] == 1, str(rg))
    check("respawn: the drop releases it", rd and rd["grab"] == 0, str(rd))
    check("respawn: the next grab picks it up again", rp and rp["grab"] == 1, str(rp))
    return 0 if all(results) else 1


if __name__ == "__main__":
    code = main()
    print(("all passed" if code == 0 else "FAILED") + f" ({sum(results)}/{len(results)})")
    if code == 0:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(code)
