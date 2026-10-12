#!/usr/bin/env python3
"""Map feature smoke (headless, scripted): one short client run per map feature, checked from
the client's packet log (KRUNKNATIVE_PACKET_LOG=1) and `state` lines.

  python3 -I tools/kref/map-features-smoke.py BUILD_DIR [offline|online] [case ...]

offline: client_bin --offline (the in-process sandbox server, client/src/net/offline.c).
online:  a server_bin on the case's map + client_bin connected to it (KRUNKNATIVE_SERVER_PORT).
The player is placed with the input script's `pose` (offline; the sandbox follows with the dev
"tp") or `tp` (online), then each case waits for the packet / position its feature produces.
Run from the repository root. Prints one PASS/FAIL line per case; exit code 0 when all passed.
Env: KN_TEST_EGL (default mesa), KN_TEST_KEEP=1 keeps the logs.
"""
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

# name: (map, script lines after `0 play`, check(log) -> (ok, detail))
def has(pattern):
    return lambda log: (re.search(pattern, log) is not None, pattern)

def state_near(x, y, z, tol=6.0):
    def check(log):
        states = re.findall(r"state t=\d+ .*?pos=([-\d.]+),([-\d.]+),([-\d.]+)", log)
        if not states:
            return False, "no state line"
        px, py, pz = map(float, states[0]) # the first state after the placement
        ok = abs(px - x) <= tol and abs(py - y) <= tol and abs(pz - z) <= tol
        return ok, "pos %.1f %.1f %.1f, want %.1f %.1f %.1f" % (px, py, pz, x, y, z)
    return check

def state_rose(min_rise):
    def check(log):
        states = re.findall(r"state t=\d+ .*?pos=([-\d.]+),([-\d.]+),([-\d.]+)", log)
        if len(states) < 2:
            return False, "need two state lines"
        rise = float(states[1][1]) - float(states[0][1])
        return rise >= min_rise, "rose %.1f" % rise
    return check

PLACE = "{t} {verb} {x} {y} {z} 0 0"

CASES = {
    # map triggers (shared/src/triggers.c) on Soul Sanctum, the v3.1.1 map that uses them
    "trigger-jumpscare": ("assets/maps/soul_sanctum.json", [(1500, (454, 220, -105))], has(r'"type":"jump_scare"')),
    "trigger-teleport-interface": ("assets/maps/soul_sanctum.json", [(1500, (358, 220, -102))], has(r'"type":"position"')),
    "trigger-toggle-gate": ("assets/maps/soul_sanctum.json", [(1500, (89, 220, 82))], has(r'"type":"object_state","id":100,"uid":\d+,"kind":"gate"')),
    "trigger-refill-knife": ("assets/maps/soul_sanctum.json", [(1500, (-117, 220, -118))], has(r'"type":"unknown","id":53')),
    "trigger-collision-damage": ("assets/maps/soul_sanctum.json", [(1500, (-97, 203, 0))], has(r'"type":"(health|kill)"')),
    "trigger-end-round": ("assets/maps/soul_sanctum.json", [(1500, (172, -251, -272))], has(r'"type":"end"')),
    # interaction (shared/src/interact.c): a score gate bought with the interact key
    "interact-gate-purchase": ("assets/maps/soul_sanctum.json",
                               [(1200, "con score 300"), (2500, (700, 314, -239)), (3200, "down G"), (3400, "up G")],
                               has(r'"type":"object_state","id":100,"uid":\d+,"kind":"gate","value":0')),
    # map zones / pads (shared/src/player.c, triggers.c)
    "teleporter": ("assets/maps/custom/ESP_Parkour.json", [(1500, (241, -33, -2)), (1700, "state")], state_near(140, 0, 0, 8)),
    "score-zone": ("assets/maps/custom/Bhop_VIP.json", [(1500, (16745, 12, 0))], has(r'"type":"score"')),
    "checkpoint": ("assets/maps/custom/Bhop_VIP.json", [(1500, (16745, 12, 0))], has(r'"type":"checkpoint"')),
    "death-zone": ("assets/maps/kanji.json", [(1500, (-130, -87, -92))], has(r'"type":"kill"')),
    "boost-pad": ("assets/maps/kanji.json", [(1500, (71, -154, -269)), (1550, "state"), (1900, "state")], state_rose(4.0)),
    # AI (shared/src/ai.c): the map's BOTs reach the client
    "map-ai": ("assets/maps/soul_sanctum.json", [(2500, "state")], has(r'"type":"ai_add"')),
    # v7.1.4 trigger runtime (WP-C, shared/src/triggers_v7.c) on the v7-format custom maps
    "v7-enter-kill": ("assets/maps/custom/ESP_Parkour.json", [(1500, (122, 5, 45))], has(r'"type":"kill"')),
    "v7-player-spawn": ("assets/maps/custom/zombie_high.json", [(2500, "state")], has(r'"type":"unknown","id":48')), # onPlayerSpawn -> Change Class / Clear Loadout ("inat")
    "v7-enter-message": ("assets/maps/custom/Slide_Neon_Stage.json", [(6000, (4861, 1058, -235))], has(r'"type":"chat".*"text":"Time logged"')),
    "v7-enter-delayed": ("assets/maps/custom/eterno_jump.json", [(1500, (-3515.75, 46, 8))], has(r'"type":"chat".*"text":"Time logged"')),
    "v7-condition-score": ("assets/maps/custom/Bhop_VIP.json", [(1200, "con score 300"), (1500, (16745, -100, 1008))],
                           has(r'"type":"chat".*"text":"Successfully logged time"')), # cd 1: score > 60
    "v7-condition-false": ("assets/maps/custom/slide_moonlight.json", [(1500, (-8634, -715, -6218))],
                           lambda log: (re.search(r'Successfully logged time', log) is None, "cd 1 score == 21 must stay false")),
    "v7-interact-key-node": ("assets/maps/custom/Krunk_Plaza.json", [(1500, (561, 12, 363)), (2200, "down G"), (2300, "up G")],
                             has(r'"type":"change_position"')), # onInteractKey -> Teleport Player to Path Node 1 ("chgp")
    "v7-join-globals": ("assets/maps/custom/zombie_high.json", [(2000, "state")], has(r'"type":"global_value".*"reset":1')), # the "gbl" join list
}

def run_case(build, mode, name, work, egl):
    map_path, steps, check = CASES[name]
    script = ["0 play"]
    for t, what in steps:
        if isinstance(what, tuple):
            script.append(PLACE.format(t=t, verb="pose" if mode == "offline" else "tp", x=what[0], y=what[1], z=what[2]))
        else:
            script.append("%d %s" % (t, what))
    end = max(t for t, _ in steps) + 2000
    script += ["%d state" % (end - 200), "%d quit" % end]
    path = os.path.join(work, name + ".script")
    with open(path, "w") as f:
        f.write("\n".join(script) + "\n")
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(k, None)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_EGL_DEVICE": egl, "XDG_CONFIG_HOME": os.path.join(work, "cfg-" + name),
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_PACKET_LOG": "1", "KRUNKNATIVE_FRAME_CAP": "60"})
    server = None
    args = [os.path.join(build, "client_bin")]
    if mode == "offline":
        args += ["--offline", map_path]
    else:
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        server_log = open(os.path.join(work, name + ".server.log"), "w")
        server = subprocess.Popen([os.path.join(build, "server_bin"), "--headless", "--cfg", "none", "--port", str(port), map_path],
                                  stdout=server_log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        time.sleep(1.0)
        env["KRUNKNATIVE_SERVER_PORT"] = str(port)
    log_path = os.path.join(work, name + "." + mode + ".log")
    with open(log_path, "w") as log_file:
        try:
            subprocess.run(args, env=env, stdout=log_file, stderr=subprocess.STDOUT, timeout=120)
        except subprocess.TimeoutExpired:
            pass
    if server:
        server.terminate()
        server.wait()
    with open(log_path, errors="replace") as f:
        log = f.read()
    ok, detail = check(log)
    return ok, detail, log_path

def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
    mode = sys.argv[2] if len(sys.argv) > 2 else "offline"
    names = sys.argv[3:] or list(CASES)
    egl = os.environ.get("KN_TEST_EGL", "mesa")
    work = tempfile.mkdtemp(prefix="kn-map-features-")
    failed = 0
    for name in names:
        ok, detail, log_path = run_case(build, mode, name, work, egl)
        print("%s  %-28s %s  (%s)" % ("PASS" if ok else "FAIL", name, mode, detail if not ok else os.path.basename(log_path)))
        failed += not ok
    print("%d/%d passed (%s); logs in %s" % (len(names) - failed, len(names), mode, work))
    if not failed and os.environ.get("KN_TEST_KEEP") != "1":
        subprocess.run(["rm", "-rf", work])
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
