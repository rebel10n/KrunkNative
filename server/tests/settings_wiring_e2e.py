#!/usr/bin/env python3
"""v10 settings that used to save and change nothing: each one is set through the page's setSetting and its
effect is observed in the running game (headless offline client_bin, input script probes).

  python3 -I server/tests/settings_wiring_e2e.py BUILD_DIR   (from the repository root)

HUD: fpsRate (the #ingameFPS rewrite cadence), nametagScale (the tag's scale), dmgType (the damage number's
text for a 23.4 hit: "23.4" Float, "24" Default), customHitmarker (the #hitmarker image replaces the drawn
marker, 105 px, gone after 350 ms), customDamage (the blood overlay loads the image), endMessage (sent to
the chat once at the match end).

Exit code 0 when every check passed; logs stay in ~/.cache/kn-settings-wiring-* on failure.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-settings-wiring-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []


def check(ok, what, detail=""):
    print(("PASS " if ok else "FAIL ") + what + (f"  -- {detail}" if detail and not ok else ""), flush=True)
    if not ok:
        failures.append(what)


def run(name, script, extra_env=None, timeout=180, args=()):
    """client_bin --offline with the script; returns the log text."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path,
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    env.update(extra_env or {})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", *args], stdout=out, stderr=subprocess.STDOUT,
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


def texts(log, label):
    """every `text @label` line's text, in order"""
    return re.findall(r"\[input_script\] text t=\d+ @" + re.escape(label) + r" n=\d+ checked=\d value=.*? text=(.*)", log)


def styles(log, label):
    """the `style @label id:prop ...` line as {id:prop: value}"""
    m = re.search(r"\[input_script\] style t=\d+ @" + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ @" + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def grep_hits(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ @" + re.escape(label) + r" needle=\S+ n=\d+ hits=(\d+)", log)
    return int(m.group(1)) if m else -1


def scale_of(transform):
    m = re.search(r"scale\(([-\d.]+)\)", transform or "")
    return float(m.group(1)) if m else None


def changes(values):
    return sum(1 for a, b in zip(values, values[1:]) if a != b)


# ---------------------------------------------------------------- HUD
HIT_IMG = "https://assets.krunker.io/textures/hitmarker_3.png"
lines = ["0 menu", "200 con bot_add 1", '300 call setSetting "showFPS" "true"', '300 call setSetting "showMessages" "true"',
         '300 call setSetting "fpsRate" "0"', "600 play"]
# fpsRate 0: #ingameFPS follows the frame counter every frame; 1000: at most once a second. The frame
# cap moves the count (120 -> 30 -> 120 over each second) so the readout has something to follow.
lines += ['2550 call setSetting "updateRate" "30"']
lines += [f"{t} text @fps0 [id=ingameFPS]" for t in range(2600, 3600, 50)]
lines += ['3600 call setSetting "fpsRate" "1000"', '3600 call setSetting "updateRate" "0"']
lines += [f"{t} text @fps1 [id=ingameFPS]" for t in range(3700, 4600, 50)]
lines += [
    "4700 style @nt1 knNameTag_63:transform",
    '4750 call setSetting "nametagScale" "0.5"',
    "4900 style @nt2 knNameTag_63:transform",
    "5000 damage 23.4 0",
    "5100 text @dmgf [id=knDmgNum_0]",
    '5150 call setSetting "dmgType" "0"',
    "5200 damage 23.4 0",
    "5300 text @dmgd [id=knDmgNum_1]",
    f'5400 call setSetting "customHitmarker" "{HIT_IMG}"',
    "5500 nethit 0",
    "5560 vis @chit hitmarker knHitmarker",
    "5560 style @chits hitmarker:width hitmarker:height",
    "6000 vis @chit-gone hitmarker",
    '6100 call setSetting "customHitmarker" ""',
    "6200 nethit 0",
    "6260 vis @hit hitmarker knHitmarker",
    f'6400 call setSetting "customDamage" "{HIT_IMG}"',
    '6600 call setSetting "endMessage" "ggwp_kn_end"',
    "6800 con end",
    "11500 grep @endmsg ggwp_kn_end [id=chatList]",
    "11600 quit",
]
log = run("hud", "\n".join(lines), {"KRUNKNATIVE_NAMETAG_DEMO": "2"})

fps0, fps1 = texts(log, "fps0"), texts(log, "fps1")
check(len(fps0) >= 18 and changes(fps0) >= 3, "fpsRate 0: #ingameFPS is rewritten every frame (several changes in 1 s)",
      f"{changes(fps0)} changes in {fps0}")
check(len(fps1) >= 16 and changes(fps1) <= 1, "fpsRate 1000: #ingameFPS changes at most once in 0.9 s",
      f"{changes(fps1)} changes in {fps1}")

s1, s2 = scale_of(styles(log, "nt1").get("knNameTag_63:transform")), scale_of(styles(log, "nt2").get("knNameTag_63:transform"))
check(s1 and s2 and abs(s2 / s1 - 0.5) < 0.02, "nametagScale 0.5 halves the player tag", f"{s1} -> {s2}")

dmg_float, dmg_default = texts(log, "dmgf"), texts(log, "dmgd")
check(dmg_float == ["23.4"], 'dmgType Float (default): a 23.4 hit shows "23.4"', str(dmg_float))
check(dmg_default == ["24"], 'dmgType Default: a 23.4 hit shows Math.ceil, "24"', str(dmg_default))

chit, chits = vis(log, "chit"), styles(log, "chits")
check(chit.get("hitmarker") == "1" and chit.get("knHitmarker") == "0",
      "customHitmarker: the hit shows the #hitmarker image, not the drawn marker", str(chit))
check(chits.get("hitmarker:width") == "105px" and chits.get("hitmarker:height") == "105px",
      "customHitmarker: animateDiv size 105 px", str(chits))
check(vis(log, "chit-gone").get("hitmarker") == "0", "customHitmarker: the image is gone after 350 ms")
hit = vis(log, "hit")
check(hit.get("knHitmarker") == "1" and hit.get("hitmarker") == "0", "customHitmarker cleared: the drawn marker is back", str(hit))

check(re.search(r"\[vfx\] customDamage \S+ -> loaded", log) is not None, "customDamage: the blood overlay loads the image")

check(grep_hits(log, "endmsg") == 1 and log.count("ggwp_kn_end") >= 1,
      "endMessage: sent to the chat once when the match ends", f"hits={grep_hits(log, 'endmsg')}")

# ---------------------------------------------------------------- input
def cam_yaw(log, label):
    m = re.search(r"\[input_script\] cam t=\d+ @" + re.escape(label) + r" .*? dir=([-\d.]+),([-\d.]+)", log)
    return float(m.group(2)) if m else None


# mouseFlick / flickClamp: three 10 px moves then one 500 px flick, with the fix off (530 px of look) and
# on (flickClamp 200: the flick repeats the last 10 px, so 40 px)
mouse = ["0 menu", "600 play", "2000 cursor 640 360", "2100 cam @m0"]
mouse += ["2120 cursor 650 360", "2140 cursor 660 360", "2160 cursor 670 360", "2180 cursor 1170 360", "2300 cam @m1"]
mouse += ['2400 call setSetting "mouseFlick" "true"', '2400 call setSetting "flickClamp" "200"', "2500 cam @m2"]
mouse += ["2520 cursor 1180 360", "2540 cursor 1190 360", "2560 cursor 1200 360", "2580 cursor 1700 360", "2700 cam @m3"]
mouse += ["2800 quit"]
log = run("mouse", "\n".join(mouse))
y0, y1, y2, y3 = (cam_yaw(log, f"m{i}") for i in range(4))
if None in (y0, y1, y2, y3):
    check(False, "mouseFlick: the camera probes logged", f"{y0} {y1} {y2} {y3}")
else:
    off, on = abs(y1 - y0), abs(y3 - y2)
    check(off > 0 and abs(on / off - 40 / 530) < 0.01, "mouseFlick + flickClamp 200: a 500 px flick turns like the last 10 px move",
          f"off={off:.4f} on={on:.4f} ratio={on / off if off else 0:.4f}")

# ---------------------------------------------------------------- scope / crosshair
# In game (DOM): the scope settings reach #aimRecticle's image and bars; the Combat Knife fully aimed
# (Q, right button held) swaps the crosshair for the throwing one (crosshairShoM 3: dot + 4 bars).
cross = ["0 menu", "600 play",
         '1500 call setSetting "scopeWidth" "5"', '1500 call setSetting "scopeHeight" "4.1"',
         '1500 call setSetting "scopeBorderCol" "#ff0000"', '1500 call setSetting "scopeBorderOpac" "0.5"',
         '1500 call setSetting "scopeOpac" "0.3"',
         "1700 style @scope recticleImg:width recticleImg:height recticleImg:opacity leftBox:right rightBox:left "
         "topBox:top bottomBox:bottom topBox:background-color leftBox:opacity",
         "1800 nodes @cross0 [id=knCrossCus] > div",
         "1900 down Q", "2000 up Q", "2600 mdown right", "3400 nodes @crossm [id=knCrossCus] > div",
         '3450 call setSetting "crosshairShoM" "0"', "3600 vis @crossoff knCrosshair",
         "3700 mup right", '3700 call setSetting "crosshairShoM" "3"', "4300 nodes @crossup [id=knCrossCus] > div",
         "4400 down E", "4450 up E",
         '4500 call setSetting "crosshairSho" "4"', '4500 call setSetting "crosshairImage" "https://krunker.io/pro_crosshair.png"',
         "4800 rects @img0 [id=knCrossCus] > img",
         '4900 call setSetting "crosshairImageW" "200"', '4900 call setSetting "crosshairImageH" "100"',
         "5200 rects @img1 [id=knCrossCus] > img", "5300 quit"]
log = run("cross", "\n".join(cross))
sc = styles(log, "scope")
check(sc.get("recticleImg:width") == "500px" and sc.get("recticleImg:height") == "410px" and sc.get("recticleImg:opacity") == "0.3",
      "scopeWidth 5 / scopeHeight 4.1 / scopeOpac 0.3: the scope image 500 x 410 px at 0.3", str(sc))
check(sc.get("leftBox:right") == "calc(50%_+_250px)" and sc.get("rightBox:left") == "calc(50%_+_250px)" and
      sc.get("topBox:top") == "calc(50%_+_204px)" and sc.get("bottomBox:bottom") == "calc(50%_+_204px)",
      "scopeWidth / Height: the bars meet the image (even half sizes 250 / 204)", str(sc))
check(sc.get("topBox:background-color") == "#ff0000" and sc.get("leftBox:opacity") == "0.5",
      "scopeBorderCol / scopeBorderOpac: the bars red at 0.5", str(sc))


def nodes_of(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @" + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


check(nodes_of(log, "cross0") == 0, "default crosshair: no custom layer", str(nodes_of(log, "cross0")))
check(nodes_of(log, "crossm") == 5, "Combat Knife aimed: the throwing crosshair (precision, dot + 4 bars)", str(nodes_of(log, "crossm")))
check(vis(log, "crossoff").get("knCrosshair") == "0", "crosshairShoM 0: no crosshair at all while the knife is aimed")
check(nodes_of(log, "crossup") == 0, "knife not aimed: the normal crosshair is back", str(nodes_of(log, "crossup")))


def rect_of(log, label):
    m = re.search(r"\[input_script\] rects t=\d+ @" + re.escape(label) + r" n=1 \S+@[-\d.]+,[-\d.]+,([\d.]+),([\d.]+)", log)
    return (float(m.group(1)), float(m.group(2))) if m else None


r0, r1 = rect_of(log, "img0"), rect_of(log, "img1")
check(r0 is not None and r0[1] > 0 and abs(r0[0] / r0[1] - 1.0) < 0.02, "image crosshair, W/H 0: its own 636 x 636 size", str(r0))
check(r0 is not None and r1 is not None and r1[1] > 0 and abs(r1[0] / r1[1] - 2.0) < 0.05 and abs(r1[0] / r0[0] - 200 / 636) < 0.02,
      "crosshairImageW 200 / H 100: the image crosshair drawn 200 x 100", f"{r0} -> {r1}")

# Screenshots (--render-scenario): the scoped view with red bars at 0.5, the throwing crosshair in green
SHOTS = os.environ.get("KN_SETTINGS_SHOTS") or os.path.join(WORK, "shots") # KN_SETTINGS_SHOTS: keep them
os.makedirs(SHOTS, exist_ok=True)


def render(name, state_extra):
    import json
    with open("tools/uidiff/scenarios/ingame-scoped.json") as f:
        sc = json.load(f)
    sc.pop("ref", None)
    sc["state"].update(state_extra.pop("state", {}))
    sc["state"]["hud"].update(state_extra.pop("hud", {}))
    path = os.path.join(SHOTS, name + ".json")
    with open(path, "w") as f:
        json.dump(sc, f)
    png = os.path.join(SHOTS, name + ".png")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "scn-cfg")})
    subprocess.run([f"{BUILD}/client_bin", "--render-scenario", path, png], env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=120)
    return png if os.path.exists(png) else None


def pixel(png, x, y):
    try:
        from PIL import Image
    except ImportError:
        return None
    with Image.open(png) as im:
        return im.convert("RGB").getpixel((x, y))


# the scope's DOM settings apply in game (render scenarios skip the settings' DOM pass): Hunter, aimed
scope = ["0 menu", '400 call setSetting "scopeBorderCol" "#ff0000"', '400 call setSetting "scopeBorderOpac" "0.5"',
         '400 call setSetting "scopeWidth" "5"', "600 play", "1500 con class Hunter",
         "2800 mdown right", f"3900 frames {SHOTS}/scoped 1", "4100 quit"]
run("scope", "\n".join(scope))
scoped = os.path.join(SHOTS, "scoped_0000.png")
px = pixel(scoped, 20, 360) if os.path.exists(scoped) else None
check(px is not None and px[0] > px[1] + 60 and px[0] > px[2] + 60, "screenshot: Hunter scoped, the side bar is red at half opacity", str(px))
thrown = render("throw-green", {"state": {"settings": {"crosshairShoM": 3, "crosshairColorM": "#00ff00"}},
                                "hud": {"scoped": False, "aim": 1, "throwCross": True}})
px = pixel(thrown, 640, 360) if thrown else None
check(px is not None and px[1] > 200 and px[0] < 80, "screenshot: the throwing crosshair's dot is crosshairColorM (green)", str(px))
print(f"screenshots: {SHOTS}")

# ---------------------------------------------------------------- viewmodel / camera
# wallLean: a wall slide (Run N Gun airborne along a wall, crouch held) rolls the camera only with the setting on.
import json as _json
WALL_MAP = os.path.join(WORK, "wall.json")
with open(WALL_MAP, "w") as f:
    _json.dump({"name": "Wall", "objects": [{"p": [0, 0, 0], "s": [400, 2, 400]}, {"p": [0, 2, -30], "s": [400, 200, 4]}],
                "spawns": [[0, 2, 0, 0, 0, 0]], "modes": [0]}, f)


def cam_rolls(log, prefix):
    return [abs(float(m)) for m in re.findall(r"\[input_script\] cam t=\d+ @" + prefix + r"\d+ .*?rot=[-\d.]+,[-\d.]+,([-\d.]+)", log)]


for lean in ("false", "true"):
    wall = ["0 menu", f'300 call setSetting "wallLean" "{lean}"', '300 call setSetting "wallLeanMlt" "1"', "600 play",
            '1500 con class "Run N Gun"',  # the class that wall jumps (player.wallJump)
            "2000 pose 0 2 -24 0.6 0", "2100 down W", "2150 down SPACE", "2250 up SPACE", "2250 down SHIFT"]
    wall += [f"{t} cam @wl{t}" for t in range(2300, 3300, 50)]
    wall += ["3300 up W", "3300 up SHIFT", "3400 quit"]
    log = run("wall-" + lean, "\n".join(wall), args=(WALL_MAP,))
    rolls = cam_rolls(log, "wl")
    if lean == "false":
        check(len(rolls) >= 15 and max(rolls) == 0.0, "wallLean off: no camera roll against the wall", str(rolls))
    else:
        check(len(rolls) >= 15 and 0.01 < max(rolls) <= 0.15 + 1e-3, "wallLean on (x1): the wall slide rolls the camera, at most 0.15",
              str(rolls))

# weapReloadY / weapSwapY: the upper body's pitch (the input log's ubr_x) dips -2.8 x the reload fold and
# -PI/4 x the swap; weapReloadY 0 keeps it level, weapSwapY -1 brings the weapon in from above.
import csv as _csv
VM_LOG = os.path.join(WORK, "viewmodel.csv")
vm = ["0 menu", '300 call setSetting "weapReloadY" "0"', "600 play", "1500 mdown left", "1800 mup left",
      "2000 down R", "2050 up R", '3600 call setSetting "weapReloadY" "1"', "3700 mdown left", "3900 mup left",
      "4000 down R", "4050 up R", '5600 call setSetting "weapSwapY" "-1"', "5700 down E", "5750 up E",
      '6400 call setSetting "weapSwapY" "1"', "6500 down E", "6550 up E", "7200 quit"]
run("viewmodel", "\n".join(vm), {"KRUNKNATIVE_INPUT_LOG": VM_LOG})
rows = []
if os.path.exists(VM_LOG):
    with open(VM_LOG) as f:
        rows = [(float(r["t"]), float(r["ubr_x"]) - float(r["cam_pitch"])) for r in _csv.DictReader(f)]


def span(t0, t1):
    return [v for t, v in rows if t0 <= t < t1]


r0, r1, s_up, s_down = span(2000, 3500), span(4000, 5500), span(5700, 6200), span(6500, 7000)
check(r0 and max(abs(v) for v in r0) < 0.1, "weapReloadY 0: no reload dip", f"{min(r0, default=None)}..{max(r0, default=None)}")
check(r1 and min(r1) < -0.5, "weapReloadY 1: the reload dips the weapon", f"min {min(r1, default=None)}")
check(s_up and max(s_up) > 0.3, "weapSwapY -1: the swap comes in from above", f"max {max(s_up, default=None)}")
check(s_down and min(s_down) < -0.3, "weapSwapY 1: the swap comes in from below", f"min {min(s_down, default=None)}")

# prog*: the interact hold bar (an onInteract trigger held with the shoot button for 30 s) placed, sized,
# turned and coloured by the Editing tab's settings
INTERACT_MAP = os.path.join(WORK, "interact.json")
with open(INTERACT_MAP, "w") as f:
    _json.dump({"name": "InteractTest", "objects": [{"p": [0, -10, 0], "s": [200, 10, 200], "t": 3},
                                                    {"p": [0, 0, 0], "s": [60, 30, 60], "i": 29, "te": 10, "tce": 30, "tc": 1}],
                "spawns": [[-40, 0, 0, 0, 1, 0], [40, 0, 0, 0, 3, 0]]}, f)
prog = ["0 menu", '300 call setSetting "progOffX" "3"', '300 call setSetting "progOffY" "4"', '300 call setSetting "progScale" "0.5"',
        '300 call setSetting "progRot" "90"', '300 call setSetting "progOpac" "0.6"', '300 call setSetting "progColor" "#00ff00"',
        "600 play", "2000 pose 0 0 0 0 0", "2400 mdown left", "3600 vis @pv interactProg",
        "3600 style @ps interactProg:left interactProg:top interactProg:width interactProg:height interactProg:transform "
        "interactProg:opacity interProgB:background-color", f"3650 frames {SHOTS}/interact 1", "3700 mup left", "3900 quit"]
log = run("prog", "\n".join(prog), args=(INTERACT_MAP,))
ps = styles(log, "ps")
check(vis(log, "pv").get("interactProg") == "1", "an interact hold shows the progress bar", str(vis(log, "pv")))
check(ps.get("interactProg:left") == "30%" and ps.get("interactProg:top") == "40%" and ps.get("interactProg:width") == "200px" and
      ps.get("interactProg:height") == "15px", "progOffX 3 / progOffY 4 / progScale 0.5: the bar at 30 % / 40 %, 200 x 15 px", str(ps))
check("rotate(1.5708rad)" in (ps.get("interactProg:transform") or "") and ps.get("interactProg:opacity") == "0.6" and
      ps.get("interProgB:background-color") == "#00ff00", "progRot 90 / progOpac 0.6 / progColor: turned, faded, green", str(ps))

# tracerOffset: the local tracers leave the barrel (0) or a point up to 20 units off it (1); trailCol /
# ropeCol reach the renderer's colours (the vfx log)
tr = ["0 menu", "600 play", "2000 mdown left", "2700 mup left", '2800 call setSetting "tracerOffset" "1"', "3600 mdown left",
      "4300 mup left", '4400 call setSetting "trailCol" "#ff0000"', '4400 call setSetting "ropeCol" "#00ff00"', "4700 quit"]
log = run("tracer", "\n".join(tr), {"KRUNKNATIVE_VFX_LOG": "1"})
cut = log.find("call t=28")
pat = r"\[vfx\] tracer start ([-\d.]+) ([-\d.]+) ([-\d.]+) dst [-\d.]+ \(camera ([-\d.]+) ([-\d.]+) ([-\d.]+)"


def rel(text):
    return [(float(a) - float(d), float(b) - float(e), float(c) - float(f_)) for a, b, c, d, e, f_ in re.findall(pat, text)]


v0, v1 = rel(log[:cut]) if cut > 0 else [], rel(log[cut:]) if cut > 0 else []
mean = [sum(v[i] for v in v0) / len(v0) for i in range(3)] if v0 else [0, 0, 0]
off = lambda vs: [sum((v[i] - mean[i]) ** 2 for i in range(3)) ** 0.5 for v in vs]
o0, o1 = off(v0), off(v1)
check(len(o0) >= 3 and max(o0) < 1.0, "tracerOffset 0: every tracer leaves the barrel", str([round(v, 2) for v in o0]))
check(len(o1) >= 3 and max(o1) > 5.0 and max(o1) < 21.0, "tracerOffset 1: the tracers start up to 20 units off the barrel",
      str([round(v, 2) for v in o1]))
check("[vfx] trailCol 1.000 0.000 0.000 ropeCol 0.000 1.000 0.000" in log, "trailCol / ropeCol: the trail red, the rope green")

# scoreBoardTrigger: Continuous (Hold, the default) shows the board only while Tab is held; Press (Toggle)
# keeps it after the release and the next press hides it
sb = ["0 menu", "600 play", "2000 down TAB", "2200 vis @h-down centerLeaderDisplay", "2300 up TAB", "2500 vis @h-up centerLeaderDisplay",
      '2600 call setSetting "scoreBoardTrigger" "0"', "2700 down TAB", "2800 up TAB", "3000 vis @t-up centerLeaderDisplay",
      "3100 down TAB", "3200 up TAB", "3400 vis @t-again centerLeaderDisplay", "3500 quit"]
log = run("board", "\n".join(sb))
seen = [vis(log, k).get("centerLeaderDisplay") for k in ("h-down", "h-up", "t-up", "t-again")]
check(seen == ["1", "0", "1", "0"], "scoreBoardTrigger: Hold shows the board while held; Toggle keeps it up until the next press", str(seen))

# ---------------------------------------------------------------- per weapon (vmSetts / gpSetts)
# The Game tab with vmSetts on Per Weapon: "Select Weapon" follows the row and the viewmodel rows edit the
# picked weapon's clones; Apply To All copies them to every weapon. In game the held weapon reads its
# clone (weaponOffX 3 moves the viewmodel), and gpSetts' sensitivityX 0.1 (its minimum) slows the mouse to a tenth.
PW_LOG = os.path.join(WORK, "perwep.csv")
pw = ["0 menu", "300 call showWindow 1", "600 clickid typeBtn", "900 call windows.0.changeTab 4",
      '1200 call setSetting "vmSetts" "1"', "1500 nodes @pw [data-kn-perwep=vmSetts]", "1500 nodes @bob0 [id=slid_weaponBob_0]",
      "1500 nodes @bobh input[oninput*='\"weaponBob_0\"']", "1600 call windows.0.changeWep 2", "1900 nodes @bob2 [id=slid_weaponBob_2]", f"1950 frames {SHOTS}/perweapon 1",
      '2000 call setSetting "weaponOffX_2" "3"', '2100 call windows.0.applyAllWeps "vmSetts"',
      "2400 call windows.0.changeTab 1", '2700 call setSetting "gpSetts" "1"', "3000 nodes @gp [data-kn-perwep=gpSetts]",
      '3100 call setSetting "sensitivityX_2" "0.1"', '3200 call windows.0.applyAllWeps "gpSetts"',
      "3500 play", "5000 cursor 640 360", "5100 cam @g0", "5150 cursor 700 360", "5200 cursor 760 360", "5300 cam @g1",
      '5400 call setSetting "gpSetts" "0"', '5400 call setSetting "vmSetts" "0"', "5500 cam @g2", "5550 cursor 820 360",
      "5600 cursor 880 360", "5700 cam @g3", "5800 quit"]
log = run("perwep", "\n".join(pw), {"KRUNKNATIVE_INPUT_LOG": PW_LOG})
check(nodes_of(log, "pw") == 1 and nodes_of(log, "bob0") == 1 and nodes_of(log, "bobh") == 1,
      "vmSetts Per Weapon: Select Weapon shows and the viewmodel rows edit weapon 0's clones",
      f"{nodes_of(log, 'pw')} {nodes_of(log, 'bob0')} {nodes_of(log, 'bobh')}")
check(nodes_of(log, "bob2") == 1, "Select Weapon: the rows follow the picked weapon (2)", str(nodes_of(log, "bob2")))
check(nodes_of(log, "gp") == 1, "gpSetts Per Weapon: Select Weapon on the Controls tab", str(nodes_of(log, "gp")))
wp = []
if os.path.exists(PW_LOG):
    with open(PW_LOG) as f:
        wp = [(float(r["t"]), abs(float(r["wp_x"]))) for r in _csv.DictReader(f)]
on = [v for t, v in wp if 4500 <= t < 5300]
off = [v for t, v in wp if 5600 <= t < 5800]
check(on and off and min(on) > 2.5 * max(off), "vmSetts Per Weapon: the held weapon's weaponOffX 3 (applied to all) moves the viewmodel",
      f"on {min(on, default=None)} off {max(off, default=None)}")
g = [cam_yaw(log, f"g{i}") for i in range(4)]
check(None not in g and abs(g[3] - g[2]) > 0.01 and abs(abs(g[1] - g[0]) / abs(g[3] - g[2]) - 0.1) < 0.02,
      "gpSetts Per Weapon: sensitivityX 0.1 on every weapon turns a tenth of the global setting's 1", str(g))

if failures:
    print(f"FAILED {len(failures)}: logs in {WORK}")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("ALL PASS")
