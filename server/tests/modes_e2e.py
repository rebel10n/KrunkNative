#!/usr/bin/env python3
"""Objective modes end-to-end: the HUD of the public rotation's modes (FFA, Capture the Flag,
Kill Confirmed) and a full CTF flag cycle, on the real client.

  python3 -I server/tests/modes_e2e.py BUILD_DIR [scenario ...]   (from the repository root)

Each scenario is one headless client_bin run (KRUNKNATIVE_HEADLESS=1, EGL; KRUNKNATIVE_EGL_DEVICE
picks the GPU) driven by an input script (client/src/input_script.c): `vis` probes log whether
page elements are shown (`:objectives`: the game-overlay layer of hud_objectives.c drew), `flags`
logs the client's flag mirror with its crystal / stand meshes. Expectations follow Krunker v7.1.4:
  - #teamScores is hidden until a team has scored (index.js f1996 over Game.teamScores = {});
  - #gameMessage carries the gmsg and the CTF notices;
  - the objective canvas items (CTF's flag pings) exist in CTF only (game-overlay.js);
  - CTF (game.js pickupFlag / dropFlag / resetFlag / updateFlags): a pickup gives the flag a
    carrier (hidden on its carrier, "Guest" in #tScoreM), a death drops it where the carrier died
    (y + flagOff, "AWAY"), a touch by its own team returns it home ("AT BASE"), and a dropped flag
    goes home by itself after flagIdle (45 s). After every step the mirror has exactly one flag per
    team, and the crystal stands where the mirror says (x / z; y bobs by +-1).
The flag cycle runs offline and against a local server_bin (online).

Prints PASS/FAIL per check; exit code 0 only when every check passed. Logs are kept in
~/.cache/kn-modes-e2e-* on failure.
"""
import concurrent.futures
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ONLY = set(sys.argv[2:])
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-modes-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
LOCK = threading.Lock()
results = []  # (scenario, check, ok, detail)

FLAG_RE = re.compile(r"\[input_script\] flag t=(\d+) i=(\d+) team=(-?\d+) carrier=(-?\d+) pos=([-\d.]+),([-\d.]+),([-\d.]+) "
                     r"mesh=(\d) crystal=([-\d.]+),([-\d.]+),([-\d.]+) vis=(\d) stand=[-\d.,]+ svis=\d info=(.*)$")
VIS_RE = re.compile(r"\[input_script\] vis t=\d+ @(\S+)(.*)")


def record(scenario, check, ok, detail=""):
    with LOCK:
        results.append((scenario, check, bool(ok), detail))
    print(("PASS  " if ok else "FAIL  ") + f"{scenario}: {check}" + (f" ({detail})" if detail and not ok else ""), flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def run_client(name, args, script, env_extra=None, timeout=240):
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write("\n".join(line.strip() for line in script.strip().splitlines()) + "\n")
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_AUDIO_BACKEND": "null"})
    env.update(env_extra or {})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        record(name, "client finished", False, "timeout")
    return read(log).splitlines()


def vis(lines):
    got = {}
    for line in lines:
        m = VIS_RE.search(line)
        if m:
            got[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split())
    return got


def flags(lines):
    """{t: [flag dicts]} per `flags` probe."""
    out = {}
    for line in lines:
        m = FLAG_RE.search(line)
        if not m:
            continue
        g = m.groups()
        out.setdefault(int(g[0]), []).append({
            "i": int(g[1]), "team": int(g[2]), "carrier": int(g[3]), "pos": tuple(map(float, g[4:7])), "mesh": int(g[7]),
            "crystal": tuple(map(float, g[8:11])), "vis": int(g[11]), "info": g[12].strip()})
    return [out[t] for t in sorted(out)]


def expect_vis(name, got, label, want):
    have = got.get(label)
    for elem, value in want.items():
        ok = have is not None and have.get(elem) == value
        record(name, f"@{label} {elem}={value}", ok, f"got {have.get(elem) if have else 'no probe'}")


# ------------------------------------------------------------------------- mode HUDs

PROBE = "inGameUI timerVal matchInfo teamScores gameMessage :objectives"


def sc_mode_hud(name, mode, mapname, objective, scores_after=None, message=None):
    """At spawn: the HUD is up, no team scores yet, the objective layer as the mode wants it; then
    standing on the objective (`scores_after`: x y z) for a while gives the team a score row."""
    script = f"""
        0 play
        300 con god
        2500 vis @spawn {PROBE}
    """
    if scores_after:
        script += f"""
        2600 pose {scores_after} 0 -0.3
        16000 vis @held {PROBE}
        16500 quit
        """
    else:
        script += "3000 quit\n"
    lines = run_client(name, ["--offline", "--map", mapname], script, {"KRUNKNATIVE_MODE": mode})
    got = vis(lines)
    want = {"inGameUI": "1", "timerVal": "1", "matchInfo": "1", "teamScores": "0", ":objectives": "1" if objective else "0"}
    if message is not None:
        want["gameMessage"] = message
    expect_vis(name, got, "spawn", want)
    if scores_after:
        expect_vis(name, got, "held", {"teamScores": "1", ":objectives": "1"})


def sc_ffa():
    sc_mode_hud("ffa", "ffa", "kanji", False)


def sc_ctf_hud():
    sc_mode_hud("ctf-hud", "ctf", "kanji", True)


def sc_kc():
    sc_mode_hud("kc", "kc", "kanji", False)


# ------------------------------------------------------------------------- CTF flag cycle

# Kanji: team 1's flag at (179, -139, -468), team 2's at (-317, -143, -74). The player joins team 2
# on an empty server; `/team red` makes it team 1 (a suicide: `respawn` brings it back).
ENEMY = (-317.0, -143.0, -74.0)
AWAY_SPOT = (-280.0, -143.0, -40.0)


def flag_cycle_script(pose):
    ex, ey, ez = ENEMY
    ax, ay, az = AWAY_SPOT
    return f"""
        0 play
        200 con team red
        4000 respawn
        8000 {pose} {ex} {ey} {ez} 0 -0.25
        9000 flags
        9100 {pose} {ax} {ay} {az} 0 -0.25
        10000 con kill
        10600 flags
        11000 respawn
        16000 con team blue
        20000 respawn
        25000 {pose} {ax} {ay} {az} 0 -0.25
        26500 flags
        27000 con team red
        31000 respawn
        36000 {pose} {ex} {ey} {ez} 0 -0.25
        37000 flags
        37100 {pose} {ax} {ay} {az} 0 -0.25
        38000 con kill
        38600 flags
        39000 respawn
        65000 flags
        86000 flags
        86500 quit
    """


def check_flag_cycle(name, lines):
    probes = flags(lines)
    steps = ["picked up", "dropped", "returned by touch", "picked up again", "dropped again", "still away (idle timer)",
             "home after flagIdle"]
    if len(probes) != len(steps):
        record(name, f"{len(steps)} flag probes", False, f"got {len(probes)}")
        return
    for step, probe in zip(steps, probes):
        active = [f for f in probe if f["mesh"]]
        per_team = sorted(f["team"] for f in active)
        record(name, f"{step}: one flag per team", per_team == [1, 2], f"teams {per_team}")
        for f in active:  # the crystal is where the mirror says (x / z), or riding its carrier
            if f["carrier"] < 0:
                dx, dz = abs(f["crystal"][0] - f["pos"][0]), abs(f["crystal"][2] - f["pos"][2])
                record(name, f"{step}: flag {f['team']} crystal at its position", dx < 0.5 and dz < 0.5 and f["vis"] == 1,
                       f"crystal {f['crystal']} pos {f['pos']} vis {f['vis']}")
        enemy = next((f for f in active if f["team"] == 2), None)
        if not enemy:
            continue
        home = abs(enemy["pos"][0] - ENEMY[0]) < 0.5 and abs(enemy["pos"][2] - ENEMY[2]) < 0.5
        if step.startswith("picked"):
            record(name, f"{step}: carried, hidden on its carrier", enemy["carrier"] >= 0 and enemy["vis"] == 0,
                   f"carrier {enemy['carrier']} vis {enemy['vis']}")
            record(name, f"{step}: #tScoreM is the carrier", enemy["info"] not in ("AT BASE", "AWAY"), enemy["info"])
        elif step.startswith("dropped") or step.startswith("still"):
            near = abs(enemy["pos"][0] - AWAY_SPOT[0]) < 6 and abs(enemy["pos"][2] - AWAY_SPOT[2]) < 6
            record(name, f"{step}: on the ground where the carrier died, AWAY",
                   enemy["carrier"] < 0 and near and enemy["info"] == "AWAY", f"{enemy}")
        else:
            record(name, f"{step}: at base, AT BASE", enemy["carrier"] < 0 and home and enemy["info"] == "AT BASE", f"{enemy}")


def sc_ctf_cycle():
    check_flag_cycle("ctf-cycle", run_client("ctf-cycle", ["--offline", "--map", "kanji"], flag_cycle_script("pose"),
                                              {"KRUNKNATIVE_MODE": "ctf"}))


def sc_ctf_cycle_online():
    (port,) = free_ports(1)
    log = os.path.join(WORK, "ctf-cycle-online-server.log")
    with open(log, "w") as out:
        server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port), "--mode", "ctf",
                                   "--open-cheats", "assets/maps/kanji.json"], stdout=out, stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL, start_new_session=True)
    try:
        time.sleep(1.5)
        lines = run_client("ctf-cycle-online", ["--connect", f"127.0.0.1:{port}"], flag_cycle_script("tp"))
        check_flag_cycle("ctf-cycle-online", lines)
    finally:
        if server.poll() is None:
            os.killpg(server.pid, signal.SIGKILL)
            server.wait()


SCENARIOS = {"ffa": sc_ffa, "ctf-hud": sc_ctf_hud, "kc": sc_kc,
             "ctf-cycle": sc_ctf_cycle, "ctf-cycle-online": sc_ctf_cycle_online}


def main():
    names = [n for n in SCENARIOS if not ONLY or n in ONLY]
    unknown = ONLY - set(SCENARIOS)
    if unknown:
        print("unknown scenarios: " + ", ".join(sorted(unknown)))
        return 2
    with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get("KN_E2E_JOBS", "3"))) as pool:
        for future in [pool.submit(SCENARIOS[n]) for n in names]:
            future.result()
    failed = [r for r in results if not r[2]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    if failed:
        print(f"logs: {WORK}")
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
