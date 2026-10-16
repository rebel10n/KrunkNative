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


def run(name, script, extra_env=None, timeout=180):
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

if failures:
    print(f"FAILED {len(failures)}: logs in {WORK}")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("ALL PASS")
