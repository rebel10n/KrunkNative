#!/usr/bin/env python3
"""The HUD weapon rack's name tag (v10.0.7 .weapNametag, deob-10.0.7.js:176440).

A slot whose worn copy has m.name draws <span class='weapNametag'>"name"</span> in place of the
skin or weapon name. A slot without one keeps .weapSkin. The name is escaped. state.skinMeta is
the match's NET_EXT_KN_SKIN_META, read by store index when the slot does not name one itself.

  python3 -I client/tests/weap_nametag_e2e.py BUILD_DIR [SRC]

Headless client_bin --render-scenario (KRUNKNATIVE_HEADLESS=1). Exit 77 without client_bin.
"""
import json
import os
import signal
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


def env_for(work, html):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_EGL_DEVICE=os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"),
               LIBGL_ALWAYS_SOFTWARE="1", KRUNKNATIVE_SCENARIO_HTML=html,
               KRUNKNATIVE_AUDIO_BACKEND="null", KRUNKNATIVE_NO_BROWSER="1",
               KRUNKNATIVE_UPDATES_URL="off", XDG_CONFIG_HOME=os.path.join(work, "cfg"))
    return env


def render(work, scenario, html, png):
    path = os.path.join(work, "scenario.json")
    with open(path, "w") as f:
        json.dump(scenario, f)
    log = os.path.join(work, "client.log")
    with open(log, "w") as out:
        p = subprocess.Popen([BIN, "--render-scenario", path, png], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env_for(work, html), cwd=SRC, start_new_session=True)
        try:
            rc = p.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            return 1, ""
    if rc != 0 or not os.path.isfile(html):
        return rc, ""
    with open(html, encoding="utf-8", errors="replace") as f:
        return 0, f.read()


def explicit_name():
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
    html = os.path.join(work, "nametag.html")
    rc, doc = render(work, {"size": [1280, 720], "background": "#000000", "state": state, "frames": 2},
                     html, os.path.join(work, "nametag.png"))
    check("explicit name: client_bin exits 0", rc == 0, work)
    check("explicit name: the document was written", bool(doc), work)
    # The document is the DOM written back out: attribute quotes are double, and a quote in
    # text is a raw " (only < and & stay escaped). Slot 1 is the copy's name in place of Pistol.
    tag = '<span class="weapNametag">"A&lt;B"C"</span>'
    check("explicit name: .weapNametag has the escaped name in quotes", tag in doc, "missing weapNametag")
    check("explicit name: that slot does not also show the weapon name", tag in doc and doc.count("Pistol") == 0)
    check("explicit name: a slot without m.name keeps .weapSkin", 'class="weapSkin"' in doc and "Assault Rifle" in doc)


def skin_meta_name():
    base = os.path.join(SRC, "tools/uidiff/scenarios/ingame-plain.json")
    if not os.path.isfile(base):
        check("skin meta: ingame-plain.json exists", False, base)
        return
    work = tempfile.mkdtemp(prefix="kn-weap-nametag-meta.", dir=os.path.expanduser("~/.cache"))
    with open(base) as f:
        scenario = json.load(f)
    scenario["state"]["skinMeta"] = [{"skin": 7582, "wear": 0.2, "serial": 4, "name": "Lucky"}]
    scenario["state"]["hud"]["loadout"][0]["skin"] = 7582
    html = os.path.join(work, "page.html")
    rc, doc = render(work, scenario, html, os.path.join(work, "frame.png"))
    check("skin meta: client_bin exits 0", rc == 0, work)

    def slot(n, nxt):
        for q in ("'", '"'):
            key = "id=%sweapItem_%d%s" % (q, n, q)
            end = "id=%sweapItem_%d%s" % (q, nxt, q)
            if key in doc:
                return doc.split(key, 1)[1].split(end, 1)[0]
        return ""

    slot0, slot1 = slot(0, 1), slot(1, 2)
    check("skin meta: the tagged slot is the copy's name", "weapNametag" in slot0 and "Lucky" in slot0 and "weapSkin" not in slot0, slot0[:300])
    check("skin meta: the next slot keeps its weapon name", "Pistol" in slot1 and "weapNametag" not in slot1, slot1[:300])


def main():
    explicit_name()
    skin_meta_name()
    if FAILS:
        print("%d failure(s)" % len(FAILS))
        return 1
    print("weap_nametag_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
