#!/usr/bin/env python3
"""Krunker v10.0.2 Settings window end-to-end on the v10 page (ui/v10): open Settings, change a setting in
every tab of both views (basic and advanced) and the Client tab, check the game applied it (the client's
"[ui] setting" / "[render] setting" lines) and that it persisted (knSettings / knClient storage), then read
it back in a second run. Also: no "Game version" row and no "Manage Ads" button, search, the Pro preset
and back to Custom, Export, keybind rebinding (Jump -> K: the window shows it, it is stored, and in game K
jumps while Space no longer does), the v10 bind rows (Ping Position, Stalker taunts, sandbox Smite /
Unlimited Ammo), and Reset (settings and binds back to their defaults).

  python3 -I server/tests/v10_settings_e2e.py BUILD_DIR   (from the repository root)

Each run is one headless offline client_bin (KRUNKNATIVE_HEADLESS=1, KRUNKNATIVE_UI_DIR=ui/v10) driven by
an input script (client/src/input_script.c: call / clickid / clicksel / type / down / up and the text /
nodes / state probes). No server, no ports. Exit code 0 when every check passed; logs are kept in
~/.cache/kn-v10-settings-* on failure.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-v10-settings-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
DOWNLOADS = os.path.join(WORK, "downloads")
UI_DIR = os.environ.get("KN_V10_UI_DIR", "ui/v10" if os.path.isfile("ui/v10/index.html") else "ui")
failures = []
os.makedirs(DOWNLOADS, exist_ok=True)


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def run(name, script, timeout=150):
    """client_bin --offline with the script; returns the log text."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": CFG,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "XDG_DOWNLOAD_DIR": DOWNLOADS,
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_UI_DIR": UI_DIR,
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(False, f"{name}: client_bin finished within {timeout} s")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def probe(log, label):
    """the `text @label` line: {n, checked, value, text}"""
    m = re.search(r"\[input_script\] text t=\d+ @" + re.escape(label) + r" n=(\d+) checked=(\d) value=(.*?) text=(.*)", log)
    if not m:
        return {"n": -1, "checked": -1, "value": None, "text": None}
    return {"n": int(m.group(1)), "checked": int(m.group(2)), "value": m.group(3), "text": m.group(4)}


def nodes(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @" + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


def storage(key):
    try:
        with open(os.path.join(CFG, "krunknative", key + ".json")) as f:
            return f.read()
    except OSError:
        return None


def positions(log):
    return [tuple(map(float, m.group(1).split(","))) for m in
            re.finditer(r"\[input_script\] state t=\d+ .*?pos=([-\d.]+,[-\d.]+,[-\d.]+)", log)]


# ---------------------------------------------------------------- run 1: change one setting per tab
script1 = """
0 menu
300 call showWindow 1
900 text @hdr [id=windowHeader]
900 text @tabs [id=settingsTabLayout]
900 nodes @gv [id=knGameVersionRow]
900 nodes @gvsel [id=knGameVersion]
900 nodes @ads [onclick="openOneTrust()"]
900 nodes @presets .setSugBox2
1000 clicksel input[onclick*="defaultRegionOnly"]
1200 text @b0 input[onclick*="defaultRegionOnly"]
1300 call windows.0.changeTab 1
1600 clicksel input[onclick*="showFPS"]
1800 text @b1 input[onclick*="showFPS"]
1900 call windows.0.changeTab 2
2200 clicksel [id=slid_fov]
2400 text @b2 [id=slid_input_fov]
2500 call windows.0.changeTab 3
2800 clicksel [id=slid_sound]
3000 text @b3 [id=slid_input_sound]
3100 clickid typeBtn
3500 text @adv [id=settingsTabLayout]
3600 call windows.0.changeTab 0
3900 clicksel input[onclick*="showNetStat"]
4100 text @a0 input[onclick*="showNetStat"]
4200 call windows.0.changeTab 1
4500 clicksel input[onclick='window.setSetting("invertY", this.checked)']
4700 text @a1 input[onclick='window.setSetting("invertY", this.checked)']
4800 call windows.0.changeTab 2
5100 clicksel input[onclick*="dynamicHP"]
5300 text @a2 input[onclick*="dynamicHP"]
5400 call windows.0.changeTab 3
5700 nodes @webgpu [id=webgpu_div][style*="none"]
5700 clicksel input[onclick*="lowSpec"]
5900 text @a3 input[onclick*="lowSpec"]
6000 call windows.0.changeTab 4
6250 setscroll menuWindow 100000
6300 clicksel input[onclick*="killFx"]
6500 text @a4 input[onclick*="killFx"]
6550 setscroll menuWindow 0
6600 call windows.0.changeTab 5
6900 clicksel [id=slid_gunsVolume]
7100 text @a5 [id=slid_input_gunsVolume]
7200 call windows.0.changeTab 6
7450 setscroll menuWindow 100000
7500 clicksel input[onclick*="autoSpec"]
7700 text @a6 input[onclick*="autoSpec"]
7750 setscroll menuWindow 0
7800 clickid settSearch
8000 type fps
8400 text @search [id=settHolder]
8400 text @searchtabs [id=settingsTabLayout]
8500 call windows.0.changeTab 0
8800 call selectSettingPre 1
9300 text @pro [id=settingsPreset]
9400 call selectSettingPre 2
9900 text @custom [id=settingsPreset]
10000 call exportSettings
10300 call showWindow 7
10800 text @ctrlhdr [id=windowHeader]
10800 text @ping [id=contmarkPosition]
10900 clickid contjump
11100 text @waiting [id=contjump]
11200 down K
11250 up K
11500 text @jump [id=contjump]
11600 call windows.6.switchTab 4
12000 text @stalker [id=contstalkerTaunt_0]
12000 nodes @smite [id=contsmiteSB]
12000 nodes @unlimited [id=contunlimitedSB]
12100 quit
"""
log1 = run("run1", script1)

hdr = probe(log1, "hdr")
check(hdr["text"] == "Game Settings", f"Settings opens on the v10 page (header {hdr['text']!r})")
tabs = probe(log1, "tabs")["text"] or ""
check(tabs.startswith("General Display Game Sound"), f"basic view tabs General / Display / Game / Sound ({tabs!r})")
check(nodes(log1, "gv") == 0 and nodes(log1, "gvsel") == 0, "no Game version row (v10 is the only version)")
check(nodes(log1, "ads") == 0, "no Manage Ads button (KrunkNative shows no ads)")
check(nodes(log1, "presets") >= 4, f"General's preset boxes ({nodes(log1, 'presets')})")
check(probe(log1, "b0")["checked"] == 1 and "handler setSetting defaultRegionOnly=true" in log1,
      "basic General: Default Region Only switched on")
check(probe(log1, "b1")["checked"] == 1 and "[ui] setting showFPS=true" in log1, "basic Display: Show FPS on, applied")
fov = probe(log1, "b2")["value"]
check(fov not in (None, "", "100") and f"[ui] setting fov={fov}" in log1, f"basic Game: FOV slider -> {fov}, applied")
check(probe(log1, "b3")["value"] == "0.5" and "[ui] setting sound=0.5" in log1, "basic Sound: Master Volume -> 0.5, applied")
adv = probe(log1, "adv")["text"] or ""
check(adv.startswith("General Controls Display Render Game Sound"), f"advanced view tabs ({adv!r})")
check(probe(log1, "a0")["checked"] == 1 and "[render] setting showNetStat=true" in log1, "advanced General: Show Network Stats on, applied")
check(probe(log1, "a1")["checked"] == 1 and "[ui] setting invertY=true" in log1, "advanced Controls: Invert Y on, applied")
check(probe(log1, "a2")["checked"] == 0 and "[render] setting dynamicHP=false" in log1, "advanced Display: Dynamic HP Bars off, applied")
check(nodes(log1, "webgpu") == 1, "advanced Render: the WebGPU row is hidden (GL only)")
check(probe(log1, "a3")["checked"] == 1 and "[render] setting lowSpec=true" in log1, "advanced Render: Low Spec on, applied")
check(probe(log1, "a4")["checked"] == 0 and "handler setSetting killFx=false" in log1, "advanced Game: Particle Effect on Kill off")
check(probe(log1, "a5")["value"] == "0.5" and "[ui] setting gunsVolume=0.5" in log1, "advanced Sound: Weapon Volume -> 0.5, applied")
check(probe(log1, "a6")["checked"] == 1, "Client tab: Auto Spectate on")
search = probe(log1, "search")["text"] or ""
check("Show FPS" in search and "Default Region" not in search, f"search 'fps' lists Show FPS only ({search[:80]!r})")
check("[ui] setting fov=110" in log1, "Pro preset applies its values (fov 110)")
check(probe(log1, "pro")["value"] == "1" and probe(log1, "custom")["value"] == "2", "the preset select follows Pro, then Custom")
check(re.search(rf"\[ui\] setting fov={re.escape(fov or 'x')}\b", log1[log1.find("setting fov=110"):]) is not None,
      "back to Custom restores the custom values")
exported = ""
try:
    with open(os.path.join(DOWNLOADS, "settings.txt")) as f:
        exported = f.read()
except OSError:
    pass
check('"showFPS":true' in exported and '"invertY":true' in exported, "Export writes the changed settings")
check(probe(log1, "ctrlhdr")["text"] == "Change Controls", "Keybinds window opens")
check(probe(log1, "ping")["text"] == "M2", "General: Ping Position (Ranked Only) bound to M2")
check(probe(log1, "waiting")["text"] not in ("SPACE", None), f"clicking Jump waits for a key ({probe(log1, 'waiting')['text']!r})")
check(probe(log1, "jump")["text"] == "K", f"Jump rebound to K ({probe(log1, 'jump')['text']!r})")
check(probe(log1, "stalker")["text"] == "1" and nodes(log1, "smite") == 1 and nodes(log1, "unlimited") == 1,
      "Modes: Stalker taunts, sandbox Smite and Unlimited Ammo rows")

saved = json.loads(storage("knSettings") or "{}")
want = {"defaultRegionOnly": True, "showFPS": True, "sound": 0.5, "showNetStat": True, "invertY": True,
        "dynamicHP": False, "lowSpec": True, "killFx": False, "gunsVolume": 0.5}
check(all(saved.get(k) == v for k, v in want.items()), f"persisted in knSettings ({ {k: saved.get(k) for k in want} })")
check(str(saved.get("fov")) == fov, f"FOV persisted ({saved.get('fov')})")
client = json.loads(storage("knClient") or "{}")
check(client.get("autoSpec") in (True, "true"), f"Auto Spectate persisted in knClient ({client.get('autoSpec')})")
check((storage("cont_jumpKey") or "").strip().strip('"') == "75", f"Jump bind persisted (cont_jumpKey {storage('cont_jumpKey')!r})")

# ---------------------------------------------------------------- run 2: read back, the bind in game
script2 = """
0 menu
300 call showWindow 1
800 text @view [id=settingsTabLayout]
900 call windows.0.changeTab 0
1100 text @b0 input[onclick*="defaultRegionOnly"]
1200 call windows.0.changeTab 4
1450 setscroll menuWindow 100000
1500 text @a4 input[onclick*="killFx"]
1600 call windows.0.changeTab 6
1900 setscroll menuWindow 100000
2000 text @a6 input[onclick*="autoSpec"]
2100 clicksel input[onclick*="autoSpec"]
2300 text @a6off input[onclick*="autoSpec"]
2400 quit
"""
# the bind in game: the clock starts when the player is in (`play` first)
script2b = """
0 play
1000 state
1100 down SPACE
1400 up SPACE
1400 state
1450 state
1500 state
2500 state
2600 down K
2900 up K
2900 state
2950 state
3000 state
3200 down J
3250 up J
4000 quit
"""
log2 = run("run2", script2)
check("[ui] setting invertY=true" in log2 and f"[ui] setting fov={fov}" in log2, "saved settings are applied at startup")
check(probe(log2, "b0")["n"] == 1 and probe(log2, "b0")["checked"] == 1, "the window shows the saved Default Region Only")
check((probe(log2, "view")["text"] or "").startswith("General Controls"), "the advanced view is remembered (krk_advanced)")
check(probe(log2, "a4")["n"] == 1 and probe(log2, "a4")["checked"] == 0, "the window shows the saved Particle Effect on Kill (off)")
check(probe(log2, "a6")["checked"] == 1 and probe(log2, "a6off")["checked"] == 0 and
      json.loads(storage("knClient") or "{}").get("autoSpec") in (False, "false"),
      "the Client tab shows the saved Auto Spectate and switches it off again")
# Godmode (Sandbox) bound to J as if picked in the Modes tab: in the offline sandbox it toggles god mode
with open(os.path.join(CFG, "krunknative", "cont_sandboxGodMode.json"), "w") as f:
    f.write("74")
log2b = run("run2b", script2b)
check("Server: God mode ON" in log2b, "sandbox Godmode bind toggles god mode in the offline sandbox")
pos = positions(log2b)
if len(pos) >= 8:
    rest, space, k = pos[0][1], max(p[1] for p in pos[1:4]), max(p[1] for p in pos[5:8])
    check(space - rest < 0.5, f"Space no longer jumps (y {rest:.2f} -> {space:.2f})")
    check(k - rest > 1.0, f"K jumps (y {rest:.2f} -> {k:.2f})")
else:
    check(False, f"in-game state probes ({len(pos)} of 8)")

# ---------------------------------------------------------------- run 3: Reset
script3 = """
0 menu
300 call showWindow 1
800 clicksel [onclick="resetSettings()"]
1200 clickid confirmBtn
1800 text @b0 input[onclick*="defaultRegionOnly"]
1900 quit
"""
log3 = run("run3", script3)
check(probe(log3, "b0")["checked"] == 0, "Reset: Default Region Only back off in the window")
saved = json.loads(storage("knSettings") or "{}")
reset_ok = all(saved.get(k, d) == d for k, d in (("showFPS", False), ("invertY", False), ("defaultRegionOnly", False), ("killFx", True), ("lowSpec", False)))
check(reset_ok, f"Reset: knSettings back to the defaults ({ {k: saved.get(k) for k in ('showFPS', 'invertY', 'defaultRegionOnly', 'killFx', 'lowSpec')} })")
check(storage("cont_jumpKey") in (None, "", '""', "null") or (storage("cont_jumpKey") or "").strip().strip('"') == "32",
      f"Reset: Jump back to Space (cont_jumpKey {storage('cont_jumpKey')!r})")

if failures:
    print(f"{len(failures)} check(s) failed; logs in {WORK}")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("v10_settings_e2e: all checks passed")
