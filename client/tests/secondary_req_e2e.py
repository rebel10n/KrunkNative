#!/usr/bin/env python3
"""The Select Secondary picker (client/src/ui/ui_loadout.c gen_secondary, window 20) applies v10's weapon req
(v10.js:214015-214020, _34256): a customOnly secondary (the Grappler) has no click unless the game is a custom
game that is not comp, or the account is a dev.

  python3 -I client/tests/secondary_req_e2e.py BUILD_DIR      (run from the repository root)

One headless client_bin --render-scenario per state; KRUNKNATIVE_SCENARIO_HTML dumps the captured page, and the
Grappler's card (skinSelector(20, true), its weapon index) is the one with a click:
  - official (state.official, a krunker.io game: never hosted, the scenario drops its offline sandbox host): the
    Grappler's card has no click, the Pistol's still does;
  - KrunkNative's custom game (the offline sandbox, a hosted game): the Grappler's card is clickable.
Exit 0 when both hold, 1 otherwise.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.getcwd()
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
CLIENT = os.path.join(BUILD, "client_bin")
WORK = tempfile.mkdtemp(prefix="kn-secondary-req-", dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
GRAPPLER = 20   # WEAPON_ID_GRAPPLER: its card's index in the picker
PISTOL = 2      # WEAPON_ID_PISTOL: a secondary with no req
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def render(name, official):
    """the picker's clickable card indices of one render (None when the render failed)"""
    state = {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Guest", "classIndex": 0,
             "region": "...", "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji",
             "anim_time": 0.8, "window": 20}
    if official:
        state["official"] = True
    scen = os.path.join(WORK, name + ".json")
    with open(scen, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": state, "frames": 3}, f)
    html = os.path.join(WORK, name + ".html")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"),
        "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
        "KRUNKNATIVE_SCENARIO_HTML": html, "KRUNKNATIVE_ASSETS_DIR": ASSETS,
    })
    proc = subprocess.run([CLIENT, "--render-scenario", scen, os.path.join(WORK, name + ".png")], env=env, cwd=ROOT,
                          capture_output=True, text=True, timeout=300)
    if proc.returncode != 0 or not os.path.exists(html):
        return None
    with open(html, encoding="utf-8", errors="replace") as f:
        page = f.read()
    return {int(m) for m in re.findall(r"skinSelector\((\d+), true\)", page)}


def main():
    if not os.path.exists(CLIENT):
        print(f"no {CLIENT}: build client_bin first")
        return 1
    official = render("official", True)
    check("official: the Select Secondary picker renders", official is not None and PISTOL in official,
          "" if official is not None else "client_bin --render-scenario failed or the picker is missing")
    if official is not None:
        check("official: the Grappler's card has no click (v10 req: customOnly needs a custom game or a dev)",
              GRAPPLER not in official, f"clickable cards {sorted(official)}")
    custom = render("custom", False)
    check("custom: the Select Secondary picker renders", custom is not None and PISTOL in custom,
          "" if custom is not None else "client_bin --render-scenario failed or the picker is missing")
    if custom is not None:
        check("custom (KrunkNative's offline sandbox, a custom game): the Grappler's card is clickable",
              GRAPPLER in custom, f"clickable cards {sorted(custom)}")
    shutil.rmtree(WORK, ignore_errors=True)
    failed = len(results) - sum(results)
    print(f"secondary_req_e2e: {len(results) - failed}/{len(results)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
