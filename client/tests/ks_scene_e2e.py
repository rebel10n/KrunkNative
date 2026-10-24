#!/usr/bin/env python3
"""Client KrunkScript scene objects on a headless client_bin (client/src/ks/ks_scene.c).

  python3 -I client/tests/ks_scene_e2e.py BUILD_DIR      (run from the repository root)

An offline Kanji round whose map carries a client script. The script adds scene objects and logs what the
obj3D members answer; KRUNKNATIVE_KS_LOG_STDOUT=1 echoes the script console ("[KRUNKSCRIPT] ...") on stdout.
Checked: the v10.0.3 light members intensity / distance / color (deob-10.0.3.js:208565-208600): lights only
(0 / "" on a cube), intensity clamped to 0..15, distance to 0..1000 on point / spot lights, color a #rrggbb
string, and writes of the wrong type ignored.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-ks-scene-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

CLIENT_SCRIPT = r'''
public action start() {
    obj l = GAME.SCENE.addSpotLight("#ff0000", 0, 0, 0, 0, -1, 0, 100, 1, 2);
    GAME.log("spot " + toStr(l.intensity) + " " + toStr(l.distance) + " " + l.color);
    l.intensity = 20;
    l.distance = 5000;
    l.color = "#00ff00";
    GAME.log("spot2 " + toStr(l.intensity) + " " + toStr(l.distance) + " " + l.color);
    l.intensity = -3;
    l.distance = -1;
    l.color = "#abc";
    GAME.log("spot3 " + toStr(l.intensity) + " " + toStr(l.distance) + " " + l.color);
    l.intensity = "5";
    l.color = "red";
    GAME.log("spot4 " + toStr(l.intensity) + " " + l.color);
    obj c = GAME.SCENE.addCube("", "#ffffff", 0, 0, 0, 1, 1, 1);
    c.intensity = 4;
    c.color = "#123456";
    GAME.log("cube " + toStr(c.intensity) + " " + toStr(c.distance) + " [" + c.color + "]");
    GAME.log("done");
}
'''


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def main():
    try:
        with open("assets/maps/official/10_kanji.json") as f:
            m = json.load(f)
        m["scripts"] = {"client": CLIENT_SCRIPT}
        map_path = os.path.join(WORK, "ks_scene.json")
        with open(map_path, "w") as f:
            json.dump(m, f)
        script = os.path.join(WORK, "input.script")
        with open(script, "w") as f:
            f.write("0 play\n600 quit\n")
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                    "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                    "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script,
                    "KRUNKNATIVE_FIXED_DELTA": "16.667", "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                    "KRUNKNATIVE_KS_LOG_STDOUT": "1"})
        log = os.path.join(WORK, "client.log")
        with open(log, "w") as out:
            p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", map_path], stdout=out,
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
            try:
                p.wait(timeout=240)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                check("client_bin finished its script", False, f"timeout, log {log}")
        lines = {}
        with open(log, errors="replace") as f:
            for line in f:
                if line.startswith("[KRUNKSCRIPT] "):
                    text = line[len("[KRUNKSCRIPT] "):].strip()
                    lines[text.split(" ")[0]] = text
        check("the client script ran", "done" in lines, f"log {log}")
        check("a spot light: intensity 2, distance 100, colour #ff0000", lines.get("spot") == "spot 2 100 #ff0000", lines.get("spot", ""))
        check("writes clamp: intensity 15, distance 1000, colour #00ff00", lines.get("spot2") == "spot2 15 1000 #00ff00", lines.get("spot2", ""))
        check("writes clamp below: 0, 0; #abc is #aabbcc", lines.get("spot3") == "spot3 0 0 #aabbcc", lines.get("spot3", ""))
        check("a string intensity and a colour name are ignored", lines.get("spot4") == "spot4 0 #aabbcc", lines.get("spot4", ""))
        check("a cube has no light members: 0, 0, \"\"", lines.get("cube") == "cube 0 0 []", lines.get("cube", ""))
    finally:
        if all(results) and results:
            shutil.rmtree(WORK, ignore_errors=True)
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
