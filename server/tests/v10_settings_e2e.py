#!/usr/bin/env python3
"""Krunker v10.0.2 Settings window end-to-end on the v10 page (ui/): open Settings, change a setting in
every tab of both views (basic and advanced) and the Client tab, check the game applied it (the client's
"[ui] setting" / "[render] setting" lines) and that it persisted (knSettings / knClient storage), then read
it back in a second run. Also: no "Game version" row and no "Manage Ads" button, search, the Pro preset
and back to Custom, Export, keybind rebinding (Jump -> K: the window shows it, it is stored, and in game K
jumps while Space no longer does), the v10 bind rows (Ping Position, Stalker taunts, sandbox Smite /
Unlimited Ammo), and Reset (settings and binds back to their defaults).

  python3 -I server/tests/v10_settings_e2e.py BUILD_DIR   (from the repository root)

Each run is one headless offline client_bin (KRUNKNATIVE_HEADLESS=1) driven by
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
UI_DIR = os.environ.get("KN_V10_UI_DIR", "ui")
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


def rect_box(log, label, element):
    """the `rect @label` line's box of an element id: (x, y, w, h, offsetWidth, offsetHeight), None when not rendered (-)"""
    m = re.search(r"\[input_script\] rect t=\d+ @" + re.escape(label) + r" .*?" + re.escape(element) + r"=([^ \n]*)", log)
    if not m or m.group(1) in ("-", ""):
        return None
    return tuple(float(v) for v in m.group(1).split(","))


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
900 nodes @qp [id=setHed_quickplay]
900 nodes @qpmodes [id^=knSeekMode]
900 text @qpnote [id=knSeekModeNote]
900 text @qphed [id=setHed_quickplay]
900 nodes @qpfolded [id=setBod_quickplay][style*="none"]
900 rect @qprow knSeekPreset0 knSeekPreset4 setBod_quickplay
1000 clicksel input[onclick*="defaultRegionOnly"]
1200 text @b0 input[onclick*="defaultRegionOnly"]
1300 call windows.0.changeTab 1
1600 clicksel input[onclick*="showFPS"]
1800 text @b1 input[onclick*="showFPS"]
1900 call windows.0.changeTab 2
2150 setscroll menuWindow 500
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
7200 call windows.0.changeTab 4
7250 setscroll menuWindow 500
7300 clicksel input[onclick*="autoSpec"]
7500 text @a6 input[onclick*="autoSpec"]
7550 setscroll menuWindow 0
7600 clicksel input[onclick*="hidePlayerNames"]
7700 text @hide input[onclick*="hidePlayerNames"]
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
check(nodes(log1, "qp") == 1 and nodes(log1, "qpmodes") >= 30, f"basic General: the Quickplay section with its Seek Modes switches ({nodes(log1, 'qpmodes')})")
check("Krunker's public rotation is on by default: Free for All, Hardpoint, Capture the Flag" in (probe(log1, "qpnote")["text"] or ""),
      "Seek Modes says what the switches start on: Krunker's public rotation")
check((probe(log1, "qphed")["text"] or "").startswith("keyboard_arrow_right") and nodes(log1, "qpfolded") == 1,
      f"Quickplay starts folded: chevron right, body hidden ({probe(log1, 'qphed')['text']!r})")
qprow = rect_box(log1, "qprow", "knSeekPreset0")
check(qprow is not None and qprow[3] > 0 and rect_box(log1, "qprow", "setBod_quickplay") is None,
      f"the presets row shows while the section is folded (Team, Krunker rotation boxes: {qprow})")
check(probe(log1, "b0")["checked"] == 1 and "handler setSetting defaultRegionOnly=true" in log1,
      "basic General: Default Region Only switched on")
check(probe(log1, "b1")["checked"] == 1 and "[ui] setting showFPS=true" in log1, "basic Display: Show FPS on, applied")
fov = probe(log1, "b2")["value"]
check(fov not in (None, "", "100") and f"[ui] setting fov={fov}" in log1, f"basic Game: FOV slider -> {fov}, applied")
check(probe(log1, "b3")["value"] == "0.5" and "[ui] setting sound=0.5" in log1, "basic Sound: Master Volume -> 0.5, applied")
adv = probe(log1, "adv")["text"] or ""
check(adv.startswith("General Controls Display Render Game Sound") and adv.endswith("Client"),
      f"advanced view tabs, the native Client tab last ({adv!r})")
check(probe(log1, "a0")["checked"] == 1 and "[render] setting showNetStat=true" in log1, "advanced General: Show Network Stats on, applied")
check(probe(log1, "a1")["checked"] == 1 and "[ui] setting invertY=true" in log1, "advanced Controls: Invert Y on, applied")
check(probe(log1, "a2")["checked"] == 0 and "[render] setting dynamicHP=false" in log1, "advanced Display: Dynamic HP Bars off, applied")
check(nodes(log1, "webgpu") == 1, "advanced Render: the WebGPU row is hidden (GL only)")
check(probe(log1, "a3")["checked"] == 1 and "[render] setting lowSpec=true" in log1, "advanced Render: Low Spec on, applied")
check(probe(log1, "a4")["checked"] == 0 and "handler setSetting killFx=false" in log1, "advanced Game: Particle Effect on Kill off")
check(probe(log1, "a5")["value"] == "0.5" and "[ui] setting gunsVolume=0.5" in log1, "advanced Sound: Weapon Volume -> 0.5, applied")
check(probe(log1, "a6")["checked"] == 1, "Game > Gameplay: Auto Spectate on")
check(probe(log1, "hide")["checked"] == 1 and "client setting hidePlayerNames=true" in log1,
      "Game > Gameplay Privacy: Hide player names on, applied")
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
check(client.get("hidePlayerNames") in (True, "true"), f"Hide player names persisted in knClient ({client.get('hidePlayerNames')})")
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
1550 setscroll menuWindow 500
1600 text @a6 input[onclick*="autoSpec"]
2100 clicksel input[onclick*="autoSpec"]
2300 text @a6off input[onclick*="autoSpec"]
2350 setscroll menuWindow 0
2400 text @hide input[onclick*="hidePlayerNames"]
2500 quit
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
      "the Game tab shows the saved Auto Spectate and switches it off again")
check(probe(log2, "hide")["checked"] == 1, "the Game tab shows the saved Hide player names")
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

# ---------------------------------------------------------------- run 2c: Settings > Quickplay's switches
# (seek_prefs.c: one switch per mode, in a folder; seek_modes stored as v10 reads it, a comma list.
# The rotation (0, 2, 3, 19, 25) is on at first; a switch saves the list of the switches on, in order.
# The Quickplay section starts folded: the click at its header opens it, the Seek Modes folder is open.)
script2c = """
0 menu
300 call showWindow 1
800 call windows.0.changeTab 0
900 setscroll menuWindow 100000
950 clickid setHed_quickplay
1000 setscroll menuWindow 1470
1500 text @trade0 [id=knSeekMode18]
1550 text @ffa0 [id=knSeekMode0]
1600 call knSeekSwitch "seek_modes" 18 1
1800 text @trade [id=knSeekMode18]
1850 call knSeekSwitch "seek_modes" 4 1
2000 text @parkour [id=knSeekMode4]
2100 clicksel [id=knSeekMode0] + .slider
2300 text @ffa [id=knSeekMode0]
2500 quit
"""
log2c = run("run2c", script2c)
check(probe(log2c, "trade0")["checked"] == 0 and probe(log2c, "ffa0")["checked"] == 1, "Quickplay: Free for All starts on (the rotation), Trade off")
# (Trade and Parkour go through the handler: a `call` saves the list but does not flip the checkbox,
# a click does; the saved lists below are what the switches send)
check(probe(log2c, "ffa")["checked"] == 0, "Quickplay: switching Free for All off turns it off")
check("[ui] Quickplay seek_modes=0,2,3,18,19,25\n" in log2c and "[ui] Quickplay seek_modes=0,2,3,4,18,19,25\n" in log2c
      and "[ui] Quickplay seek_modes=2,3,4,18,19,25\n" in log2c,
      "Quickplay: each switch saves the list of the switches on (the rotation plus Trade, then Parkour, then no Free for All)")
saved_modes = json.loads(storage("knSettings") or "{}").get("seek_modes")
check(saved_modes == "2,3,4,18,19,25", f"Quickplay: seek_modes persisted as v10's comma list ({saved_modes!r})")

# ---------------------------------------------------------------- run 2d: Quickplay's folded section and presets
# (seek_prefs.c presets_html / apply_preset). A new client on the same settings: the section is folded again
# (its state is not saved), the presets work while it is folded, the header opens and folds it, and a preset
# shows its switches. Team = modes 1, 2, 3, 19, 25, 26 on the rotation maps; Competitive = Hardpoint on the
# Ranked pool (2, 4, 11, 14, 17, 42: 10.0.3's, no Eterno Simulator); Every mode and map = all 37 modes and 46 maps; Krunker rotation = both empty.
script2d = """
0 menu
300 call showWindow 1
800 call windows.0.changeTab 0
900 text @hed0 [id=setHed_quickplay]
900 nodes @folded0 [id=setBod_quickplay][style*="none"]
900 rect @rect0 knSeekPreset0 setBod_quickplay
950 setscroll menuWindow 100000
1000 clickid knSeekPreset1
1200 text @team1 [id=knSeekMode1]
1250 text @team0 [id=knSeekMode0]
1300 text @team19 [id=knSeekMode19]
1350 text @team4 [id=knSeekMode4]
1400 text @teammap39 [id=knSeekMap39]
1420 text @teammap5 [id=knSeekMap5]
1500 clickid knSeekPreset3
1700 text @ranked39 [id=knSeekMap39]
1720 text @ranked42 [id=knSeekMap42]
1750 text @ranked0 [id=knSeekMap0]
1760 text @ranked17 [id=knSeekMap17]
1800 text @ranked1 [id=knSeekMode1]
1800 text @ranked2 [id=knSeekMode2]
1900 clickid knSeekPreset4
2100 text @any36 [id=knSeekMode36]
2150 text @any0 [id=knSeekMode0]
2200 text @any45 [id=knSeekMap45]
2300 clickid knSeekPreset0
2500 text @rot0 [id=knSeekMode0]
2550 text @rot4 [id=knSeekMode4]
2600 text @rotmap2 [id=knSeekMap2]
2650 text @rotmap5 [id=knSeekMap5]
2700 clickid knSeekPreset1
2900 call knSeekSwitch "seek_modes" 4 1
3050 text @custom1 [id=knSeekMode1]
3100 setscroll menuWindow 100000
3150 clickid setHed_quickplay
3300 text @hed1 [id=setHed_quickplay]
3300 nodes @folded1 [id=setBod_quickplay][style*="none"]
3300 rect @rect1 setBod_quickplay knSeekMode4
3350 setscroll menuWindow 200
3400 clickid setHed_quickplay
3600 text @hed2 [id=setHed_quickplay]
3600 nodes @folded2 [id=setBod_quickplay][style*="none"]
3700 quit
"""
log2d = run("run2d", script2d)
check((probe(log2d, "hed0")["text"] or "").startswith("keyboard_arrow_right") and nodes(log2d, "folded0") == 1,
      "a new client on the same settings: Quickplay starts folded again (not saved)")
check(rect_box(log2d, "rect0", "knSeekPreset0") is not None and rect_box(log2d, "rect0", "setBod_quickplay") is None,
      "folded: the presets row is shown, the switches are not")
check(probe(log2d, "team1")["checked"] == 1 and probe(log2d, "team0")["checked"] == 0 and probe(log2d, "team19")["checked"] == 1
      and probe(log2d, "team4")["checked"] == 0, "Team preset: Team Deathmatch, Hardpoint, Kill Confirmed on; Free for All and Parkour off")
check(probe(log2d, "teammap5")["checked"] == 0 and probe(log2d, "teammap39")["checked"] == 1,
      "Team preset: the maps stay the rotation (Eterno Simulator, in it, on; Shipment, not in it, off)")
check("[ui] Quickplay seek_modes=1,2,3,19,25,26\n" in log2d and "[ui] Quickplay seek_maps=(none: the public rotation)\n" in log2d,
      "Team preset saves seek_modes 1,2,3,19,25,26 and the rotation maps, as the switches would")
check(probe(log2d, "ranked42")["checked"] == 1 and probe(log2d, "ranked17")["checked"] == 1 and probe(log2d, "ranked0")["checked"] == 0
      and probe(log2d, "ranked39")["checked"] == 0 and probe(log2d, "ranked1")["checked"] == 0 and probe(log2d, "ranked2")["checked"] == 1,
      "Competitive: Hardpoint and the Ranked pool (Bureau, Frontier) on; Burg (the rotation), Eterno Simulator (left the pool in 10.0.3) and Team Deathmatch off")
check("[ui] Quickplay seek_maps=2,4,11,14,17,42\n" in log2d and "[ui] Quickplay seek_modes=2\n" in log2d,
      "Competitive saves seek_modes 2 and the Ranked pool 2,4,11,14,17,42")
check(probe(log2d, "any36")["checked"] == 1 and probe(log2d, "any0")["checked"] == 1 and probe(log2d, "any45")["checked"] == 1,
      "Every mode and map: the last mode, the first mode and the last map are on")
check(probe(log2d, "rot0")["checked"] == 1 and probe(log2d, "rot4")["checked"] == 0 and probe(log2d, "rotmap2")["checked"] == 1
      and probe(log2d, "rotmap5")["checked"] == 0, "Krunker rotation: the rotation switches on again, Parkour and Shipment off")
check("[ui] Quickplay seek_modes=(none: the public rotation)\n" in log2d, "Krunker rotation saves the empty list (the rotation)")
check(probe(log2d, "custom1")["checked"] == 1, "after the Team preset, its switches still show it (Hardpoint on)")
saved_d = json.loads(storage("knSettings") or "{}")
check(saved_d.get("seek_modes") == "1,2,3,4,19,25,26" and saved_d.get("seek_maps") in (None, ""),
      f"a switch after the Team preset saves the list with Parkour (seek_modes {saved_d.get('seek_modes')!r})")
check((probe(log2d, "hed1")["text"] or "").startswith("keyboard_arrow_down") and nodes(log2d, "folded1") == 0,
      "the header opens the section: chevron down, body shown")
check((rect_box(log2d, "rect1", "setBod_quickplay") or (0, 0, 0, 0, 0, 0))[5] > 0, "opened: the body is rendered (its switches with it)")
check((probe(log2d, "hed2")["text"] or "").startswith("keyboard_arrow_right") and nodes(log2d, "folded2") == 1,
      "the header folds it again")

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
