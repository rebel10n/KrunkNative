#!/usr/bin/env python3
"""The HUD weapon rack's name tag (v10.0.7 .weapNametag, deob-10.0.7.js:176440).

A slot whose worn copy has m.name draws <span class='weapNametag'>"name"</span> in place of the
skin or weapon name. A slot without one keeps .weapSkin. The name is escaped.

  python3 -I client/tests/weap_nametag_e2e.py BUILD_DIR [SRC]

Headless client_bin --render-scenario (KRUNKNATIVE_HEADLESS=1). Exit 77 without client_bin.
"""
import json
import os
import subprocess
import sys
import tempfile

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.getcwd())
BIN = os.path.join(BUILD, "client_bin")
if not os.access(BIN, os.X_OK):
    print("weap_nametag_e2e: no client_bin: skipped")
    sys.exit(77)

FAILS = []


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % detail[:300]), flush=True)
    if not ok:
        FAILS.append(what)


def main():
    work = tempfile.mkdtemp(prefix="kn-weap-nametag.", dir=os.path.expanduser("~/.cache"))
    state = {
        "menu": False, "window": None, "connected": True, "ready": True, "inGame": True,
        "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "modeName": "Free for All",
        "name": "Local User", "region": "...", "anim_time": 0.8,
        "hud": {
            "health": 100, "maxHealth": 100, "ammo": 30, "ammoMax": 30, "timerText": "03:45",
            "weaponName": "Assault Rifle", "classIndex": 0, "loadoutIndex": 0,
            "loadout": [
                {"name": "Assault Rifle", "icon": "icon_2", "melee": False},
                {"name": "Pistol", "icon": "icon_3", "melee": False, "nametag": "A<B\"C"},
                {"name": "Combat Knife", "icon": "icon_0", "melee": True},
            ],
        },
    }
    path = os.path.join(work, "nametag.json")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": state, "frames": 2}, f)
    html = os.path.join(work, "nametag.html")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_EGL_DEVICE=os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"),
               LIBGL_ALWAYS_SOFTWARE="1", KRUNKNATIVE_SCENARIO_HTML=html,
               XDG_CONFIG_HOME=os.path.join(work, "cfg"))
    r = subprocess.run([BIN, "--render-scenario", path, os.path.join(work, "nametag.png")],
                       cwd=SRC, env=env, capture_output=True, text=True, timeout=180)
    check("client_bin exits 0", r.returncode == 0, (r.stdout + r.stderr)[-500:])
    doc = ""
    if os.path.isfile(html):
        with open(html, encoding="utf-8", errors="replace") as f:
            doc = f.read()
    check("the document was written", bool(doc), work)
    # The document is the DOM written back out: attribute quotes are double, and a quote in
    # text is a raw " (only < and & stay escaped). Slot 1 is the copy's name in place of Pistol.
    tag = '<span class="weapNametag">"A&lt;B"C"</span>'
    check("the named slot is .weapNametag with the escaped name in quotes", tag in doc, "missing weapNametag")
    check("that slot does not also show the weapon name", tag in doc and doc.count("Pistol") == 0)
    check("a slot without m.name keeps .weapSkin", 'class="weapSkin"' in doc and "Assault Rifle" in doc)
    if FAILS:
        print("%d failure(s), files in %s" % (len(FAILS), work))
        return 1
    print("weap_nametag_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
