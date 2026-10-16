#!/usr/bin/env python3
"""KrunkScript v10.0.2 client API on a headless offline client_bin (doc/v10/map-format.md section 6).

  python3 -I client/tests/ks_v10_client_e2e.py BUILD_DIR      (run from the repository root)

sandstorm with a client script that exercises the client members added by this package:
  1. SCENE.addParticles, URLS.openDiscord, CONFIG.getSettings answer undefined (v10 leaves them empty)
  2. ENTITIES.list answers an empty array (no entity manager on this build yet)
  3. COOKIES.removeByKey runs (v10: client only, removes the stored key)
  4. onPlayerSpawn: PLAYERS.getSelf().x / .y are numbers (the player x / y / z members, v10 L182010)
  5. no runtime errors
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1; KRUNKNATIVE_EGL_DEVICE picks it).
Assets come from KRUNKNATIVE_ASSETS_DIR (default: the repository's assets/; a git worktree needs the
ignored asset folders: KRUNKNATIVE_ASSETS_DIR=<main checkout>/assets).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import base64
import json
import os
import re
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
WORK = tempfile.mkdtemp(prefix="kn-ks-v10-client-")
results = []
KS_RE = re.compile(r"\[KRUNKSCRIPT\] (.*)")

CLIENT_SCRIPT = """
public action start() {
    GAME.log("particles", toStr GAME.SCENE.addParticles(0, 5, 0, {texture: "glow", count: 10}));
    GAME.log("urls", toStr GAME.URLS.openDiscord());
    GAME.log("settings", toStr GAME.CONFIG.getSettings());
    GAME.log("entities", toStr lengthOf GAME.ENTITIES.list());
    GAME.COOKIES.removeByKey("best");
    GAME.log("cookie removed");
}
public action onPlayerSpawn(str id) {
    obj me = GAME.PLAYERS.getSelf();
    GAME.log("self", toStr (me.x == me.x), toStr (me.y == me.y), toStr (me.z == me.z));
}
"""


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def assets_dir():
    """The repository's assets/, or the main checkout's when this is a git worktree (ignored folders)."""
    here = os.path.abspath("assets")
    if os.path.isdir(os.path.join(here, "css")):
        return here
    try:
        common = subprocess.run(["git", "rev-parse", "--git-common-dir"], capture_output=True, text=True, check=True).stdout.strip()
        main = os.path.join(os.path.dirname(os.path.abspath(common)), "assets")
        if os.path.isdir(os.path.join(main, "css")):
            return main
    except (subprocess.CalledProcessError, OSError):
        pass
    return here


def main():
    with open("assets/maps/311_sandstorm.json") as f:
        m = json.load(f)
    m["scripts"] = {"client": base64.b64encode(CLIENT_SCRIPT.encode()).decode(), "server": ""}
    map_path = os.path.join(WORK, "ks_v10_client.json")
    with open(map_path, "w") as f:
        json.dump(m, f)
    script = os.path.join(WORK, "input.txt")
    with open(script, "w") as f:
        f.write("0 play\n2500 quit\n")
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(cfg, exist_ok=True)
    env = dict(os.environ, KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_AUDIO_BACKEND="null", KRUNKNATIVE_FIXED_DELTA="16",
               KRUNKNATIVE_INPUT_SCRIPT=script, XDG_CONFIG_HOME=cfg)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env.setdefault("KRUNKNATIVE_ASSETS_DIR", assets_dir())
    p = subprocess.run([f"{BUILD}/client_bin", "--offline", map_path], env=env, capture_output=True, text=True, timeout=300)
    text = p.stdout + p.stderr
    lines = [m.group(1).strip() for m in KS_RE.finditer(text)]
    check("client script ran (start and onPlayerSpawn)", "particles undefined" in lines and any(x.startswith("self ") for x in lines),
          str(lines[:6]))
    check("SCENE.addParticles, URLS.openDiscord, CONFIG.getSettings answer undefined",
          "urls undefined" in lines and "settings undefined" in lines and "particles undefined" in lines)
    check("ENTITIES.list answers an empty array", "entities 0" in lines)
    check("COOKIES.removeByKey runs", "cookie removed" in lines)
    check("PLAYERS.getSelf x / y / z are numbers", any(x.startswith("self true true true") for x in lines), str(lines))
    check("no runtime errors", "RUNTIME ERROR" not in text and "error:" not in " ".join(lines), " | ".join(lines[-4:]))
    check("client exits cleanly", p.returncode == 0, str(p.returncode))
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
