#!/usr/bin/env python3
"""Offline persistence of KrunkScript data across client sessions (doc/v7/krunkscript.md 2.6, WP6).

  python3 -I server/tests/ks_offline_persist_e2e.py BUILD_DIR     (run from the repository root)

client_bin --offline (headless, KRUNKNATIVE_HEADLESS=1) runs twice on one private
XDG_CONFIG_HOME with a map whose
  * client script counts its runs in GAME.COOKIES (localStorage "krkcst_<map>_<key>": natively
    $XDG_CONFIG_HOME/krunknative/<key>.json) and checks v7's value rules (booleans refused,
    arrays as "1,2", a missing key reads "null");
  * server script (the offline sandbox) adds 1 to GAME.STORAGE "visits" on every spawn: the
    offline file store ks-storage-<hash>.json, written through the client's html_storage.
The second run must see runs 2 and visits 2.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
WORK = tempfile.mkdtemp(prefix="kn-ks-persist-")
results = []

SERVER = """
action saved(obj data, bool ok, str name) { GAME.log("visits", toStr ok, name, (str) data.visits); }
public action onPlayerSpawn(str id) { obj p = GAME.PLAYERS.findByID(id); GAME.STORAGE.update((str) p.accountName, {visits: 1}, "private", saved); }
"""
CLIENT = """
public action start() {
    num runs = 0;
    if (GAME.COOKIES.has("runs")) { runs = (num) GAME.COOKIES.load("runs"); }
    runs += 1;
    GAME.COOKIES.save("runs", runs);
    GAME.COOKIES.save("flag", true);
    GAME.COOKIES.save("list", num[1, 2]);
    GAME.log("cookies", toStr runs, GAME.COOKIES.load("flag"), GAME.COOKIES.load("list"), GAME.COOKIES.load("missing"));
}
"""


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(cfg, map_path, inputs):
    env = dict(os.environ, KRUNKNATIVE_HEADLESS="1", XDG_CONFIG_HOME=cfg, KRUNKNATIVE_INPUT_SCRIPT=inputs, KRUNKNATIVE_FRAME_CAP="60")
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    r = subprocess.run([f"{BUILD}/client_bin", "--offline", map_path], env=env, capture_output=True, timeout=180)
    return [l for l in r.stdout.decode(errors="replace").splitlines() + r.stderr.decode(errors="replace").splitlines()
            if "[console] [KRUNKSCRIPT]" in l]


def main():
    with open("server/tests/arena.json") as f:
        m = json.load(f)
    m["name"] = "KsPersist"
    m["scripts"] = {"server": SERVER, "client": CLIENT}
    map_path = os.path.join(WORK, "persist.json")
    with open(map_path, "w") as f:
        json.dump(m, f)
    inputs = os.path.join(WORK, "input.txt")
    with open(inputs, "w") as f:
        f.write("0 play\n4000 quit\n")
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(cfg)
    first = run(cfg, map_path, inputs)
    check("first run", any("cookies 1 null 1,2 null" in l for l in first) and any("visits true local 1" in l for l in first), str(first))
    second = run(cfg, map_path, inputs)
    check("COOKIES persist across sessions", any("cookies 2 null 1,2 null" in l for l in second), str(second))
    check("offline STORAGE persists across sessions", any("visits true local 2" in l for l in second), str(second))
    files = os.listdir(os.path.join(cfg, "krunknative"))
    check("stored under the config dir", "krkcst_KsPersist_runs.json" in files and any(n.startswith("ks-storage-") for n in files),
          str(files))
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("kept " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
