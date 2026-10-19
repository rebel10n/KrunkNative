#!/usr/bin/env python3
"""Typing and HUD input regressions, end to end through real key + char events.

  python3 -I server/tests/input_typing_e2e.py BUILD_DIR      (run from the repository root)

The input script's `down <key>` sends the char callback after the key callback for printable keys,
as GLFW does (client/src/input_script.c), so these runs type the way a keyboard does:

  1. typing: Space types a space in Settings' search box (from the main menu and from the in-game
     menu); B and T typed into the search box, the in-game chat, the login modal's username and the
     Object Manager's search land in the field and fire neither the bhop toggle nor Ctrl TP; the same
     keys with no field focused do (the control: the log lines this test looks for exist). SHIFT+A
     types "A", CTRL+A types nothing.
  2. bhop toast: the bhop mode message slides in from the top right in its own toast (#knBhopToast,
     bhop_toast.c, the v3.1.1 notify.ts look), starts off the right edge and settles in place; it is
     not a notification (no centre or #knNotify text, no chat line, nothing in the middle of the
     screen). Ctrl TP (T with no field focused) shows its message in it too, the one exception; a
     generic notification never does and Ctrl TP is not in the notification path any more.
  3. PRESS START: a controller's Start opens the menu with "PRESS START"; once the pad goes quiet and
     the mouse moves the menu reads "CLICK TO PLAY" again (the text follows Gamepad.active).
  4. the Sniper Rifle's scope (#aimRecticle) is gone the moment we die scoped, through the death
     screen and the menu, and does not come back on the respawn while the aim key is still held.
  5. the end screen: CLICK TO PLAY (#instructions) is never on screen together with the results or the
     title (alive at the END, menu open at the END); a window opened over the results, closed, then
     Click to Play brings the results back (UI.toggleEndScreen) instead of leaving CLICK TO PLAY up.

Each run is one headless offline client_bin (KRUNKNATIVE_HEADLESS=1). Exit code 0 when every check
passed; logs are kept in ~/.cache/kn-input-typing-* on failure.
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
WORK = tempfile.mkdtemp(prefix="kn-input-typing-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def run(name, script, args=("--offline",), storage=None, timeout=150):
    """client_bin with the script on a fresh config; returns the log text."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in (storage or {}).items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
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


def value(log, label):
    """the `text @label` probe's value= (the field's text), None when absent"""
    m = re.search(r"\[input_script\] text t=\d+ @" + re.escape(label) + r" n=(\d+) checked=\d value=(.*?) text=(.*)", log)
    return m.group(2) if m and m.group(1) != "0" else None


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ @" + re.escape(label) + r" n=\d+ checked=\d value=.*? text=(.*)", log)
    return m.group(1).strip() if m else None


def vis(log, label):
    """the `vis @label` probe as {id: 0/1}"""
    m = re.search(r"\[input_script\] vis t=\d+ @" + re.escape(label) + r" (.*)", log)
    return {k: v for k, v in re.findall(r"(\S+?)=(\S+)", m.group(1))} if m else {}


def toggles(log):
    """the bhop toggle and Ctrl TP notices printed so far (ui_bhop.c / ui_mod_keys.c console.log)"""
    return len(re.findall(r"^Bhop - ", log, re.M)), len(re.findall(r"^Ctrl TP - ", log, re.M))


def press(t, key):
    return f"{t} down {key}\n{t + 40} up {key}\n"


# ---------------------------------------------------------------- 1. typing in fields
script = "0 menu\n300 call showWindow 1\n900 clickid settSearch\n"
script += press(1000, "F") + press(1100, "O") + press(1200, "SPACE") + press(1300, "B") + press(1400, "T")
script += press(1500, "SHIFT+A") + press(1600, "CTRL+A")
script += "1800 text @search [id=settSearch]\n1900 call closWind\n"
# the login modal's username (v10_login.c)
script += "2200 clickid knV10LoginBtn\n2700 clickid knV10AuthSwitch\n2900 clickid accName\n"
script += press(3000, "B") + press(3100, "T") + "3300 text @acc [id=accName]\n3400 quit\n"
log1 = run("menu-fields", script)
check(value(log1, "search") == "fo btA", f"Settings search: Space, B, T and Shift+A type, Ctrl+A does not (value {value(log1, 'search')!r})")
check(value(log1, "acc") == "bt", f"login username: B and T type (value {value(log1, 'acc')!r})")
check(toggles(log1) == (0, 0), f"no bhop toggle / Ctrl TP while typing in the menu (seen {toggles(log1)})")

script = "0 play\n" + press(300, "ESC") + "900 call showWindow 1\n1400 clickid settSearch\n"
script += press(1500, "F") + press(1600, "SPACE") + press(1700, "V") + "1900 text @search [id=settSearch]\n2000 call closWind\n"
script += press(2100, "ESC") + "2400 clickid instructionHolder\n"
# the in-game chat (Enter focuses #chatInput), then the Object Manager's search (F1)
script += press(3000, "ENTER") + "3200 vis @chat chatInput\n" + press(3300, "B") + press(3400, "SPACE") + press(3500, "T")
script += "3700 text @chat [id=chatInput]\n" + press(3800, "ESC") + "4100 clickid instructionHolder\n"
script += press(4500, "F1") + "5000 clickid knOmSearchBox\n" + press(5100, "B") + press(5200, "T")
script += "5400 text @om [id=knOmSearchBox]\n" + press(5500, "F1") + "5700 clickid instructionHolder\n"
script += "6000 state\n" + press(6200, "B") + press(6400, "T") + "6800 quit\n"
log2 = run("game-fields", script)
before_free = log2.split("[input_script] state t=")[0]
check(value(log2, "search") == "f v", f"in-game menu: Space types in Settings' search (value {value(log2, 'search')!r})")
check(value(log2, "chat") == "b t", f"in-game chat: B, Space and T type (value {value(log2, 'chat')!r})")
check(value(log2, "om") == "bt", f"Object Manager search: B and T type (value {value(log2, 'om')!r})")
check(toggles(before_free) == (0, 0), f"no bhop toggle / Ctrl TP while typing in game (seen {toggles(before_free)})")
check(toggles(log2) == (1, 1), f"control: B and T with no field focused toggle bhop and Ctrl TP (seen {toggles(log2)})")

# ---------------------------------------------------------------- 2. the bhop toast
def style_value(log, label):
    """the `style @label id:prop` probe's inline value for the bhop toast's transform"""
    m = re.search(r"\[input_script\] style t=\d+ @" + re.escape(label) + r" knBhopToast:transform=(\S+)", log)
    return m.group(1) if m else None


def rect(log, label):
    """the `rect @label id` probe as (x, y, w, h), None when absent"""
    m = re.search(r"\[input_script\] rect t=\d+ @" + re.escape(label) + r" knBhopToast=([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+)", log)
    return tuple(float(v) for v in m.groups()) if m else None


# T (Ctrl TP) shows "Ctrl TP - Enabled" in the bhop toast, and not in the notification path. Then B.
script = "0 play\n" + press(1000, "T") + "1300 vis @ctp knBhopToast\n1300 vis @ctpn knNotify\n"
script += "1300 text @ctptoast [id=knBhopToast]\n1300 text @ctpnotice [id=knNotify]\n1300 text @ctpcenter .notification-text\n"
script += press(3500, "B") + "3560 style @slide knBhopToast:transform\n3560 vis @shown knBhopToast\n"
script += "3800 style @plat knBhopToast:transform\n3800 vis @toast knBhopToast\n3800 text @toast [id=knBhopToast]\n"
script += "3800 rect @rect knBhopToast\n3800 grep @chat bhop [id=chatList]\n3800 vis @mid gameMessage roundMessage\n4000 quit\n"
log3 = run("bhop-toast", script)
check(vis(log3, "ctp").get("knBhopToast") == "1" and (text(log3, "ctptoast") or "").startswith("Ctrl TP - Enabled"),
      f"Ctrl TP: its message shows in the bhop toast ({vis(log3, 'ctp')} {text(log3, 'ctptoast')!r})")
ctp_notice = (text(log3, "ctpnotice") or "").startswith("Ctrl TP -") or (text(log3, "ctpcenter") or "").startswith("Ctrl TP -")
check(not ctp_notice, f"Ctrl TP: not in the notification path (toast {text(log3, 'ctpnotice')!r}, "
                      f"centre {text(log3, 'ctpcenter')!r})")
check(vis(log3, "toast").get("knBhopToast") == "1" and (text(log3, "toast") or "").startswith("Bhop -"),
      f"bhop toggle: the bhop toast shows it (toast {vis(log3, 'toast')} {text(log3, 'toast')!r})")
slide = style_value(log3, "slide")
check(slide is not None and slide.startswith("translateX(") and float(slide[len("translateX("):-len("px)")]) > 0,
      f"bhop toast slides in: off the right edge at the start (transform {slide!r})")
check(style_value(log3, "plat") == "translateX(0.00px)", f"bhop toast settles in place (transform {style_value(log3, 'plat')!r})")
r = rect(log3, "rect")
check(r is not None and r[0] + r[2] >= 1200 and r[0] > 640 and r[1] <= 40,
      f"bhop toast sits in the top right corner (x,y,w,h {r})")
m = re.search(r"\[input_script\] grep t=\d+ @chat needle=\S+ n=(\d+) hits=(\d+)", log3)
check(m is not None and m.group(1) == "1" and m.group(2) == "0", "bhop toggle: no chat line")
check(set(vis(log3, "mid").values()) <= {"0", "-"}, f"bhop toggle: nothing in the middle of the screen ({vis(log3, 'mid')})")

# ---------------------------------------------------------------- 3. PRESS START
script = "0 play\n500 gpad start\n600 state\n700 text @pad [id=instructions]\n"
script += "1000 cursor 400 300\n1050 cursor 420 320\n1300 text @mouse [id=instructions]\n1400 quit\n"
log4 = run("press-start", script)
check(" menu=1 " in (re.search(r"\[input_script\] state t=\d+ .*", log4) or [""])[0], "Start opens the menu")
check(text(log4, "pad") == "CLICK TO PLAY",
      f"the pad went quiet at the next frame: the menu reads CLICK TO PLAY (was {text(log4, 'pad')!r})")
check(text(log4, "mouse") == "CLICK TO PLAY", f"after mouse movement: CLICK TO PLAY (was {text(log4, 'mouse')!r})")

# ---------------------------------------------------------------- 4. the scope after death
script = "0 menu\n100 call selectClass 1\n500 play\n1600 mdown right\n3000 vis @aim aimRecticle\n"
script += "3100 con kill\n3120 vis @d0 aimRecticle\n3500 vis @d1 aimRecticle\n6500 vis @d2 aimRecticle menuHolder\n"
script += "7000 respawn\n7300 state\n7300 vis @r0 aimRecticle\n7600 mup right\n7900 vis @r1 aimRecticle\n8000 quit\n"
log5 = run("scope-death", script)
check(vis(log5, "aim").get("aimRecticle") == "1", "control: the Sniper Rifle's scope shows when aimed")
for label in ("d0", "d1", "d2"):
    check(vis(log5, label).get("aimRecticle") == "0", f"scope gone after death ({label}: {vis(log5, label)})")
check(vis(log5, "r0").get("aimRecticle") == "0", f"no scope on the respawn with the aim key still held ({vis(log5, 'r0')})")

# ---------------------------------------------------------------- 5. CLICK TO PLAY vs the end screen
END_IDS = "endUI instructions knVicTitle"


def overlaps(log):
    """vis probes where #instructions shows together with the results or the title"""
    bad = []
    for m in re.finditer(r"\[input_script\] vis t=(\d+) @(\S+) (.*)", log):
        v = dict(re.findall(r"(\S+?)=(\S+)", m.group(3)))
        if v.get("instructions") == "1" and (v.get("endUI") == "1" or v.get("knVicTitle") == "1"):
            bad.append(m.group(2))
    return bad


script = "0 play\n300 con end\n"
script += "".join(f"{t} vis @a{t} {END_IDS}\n" for t in (1000, 4000, 9000, 14000))
script += press(14100, "ESC") + f"14500 vis @esc {END_IDS}\n14600 clickid instructionHolder\n15000 vis @back {END_IDS}\n15100 quit\n"
log6 = run("end-alive", script, args=("--offline", "assets/maps/official/10_kanji.json"))
check(vis(log6, "a4000").get("endUI") == "1", "control: the end screen shows")
check(vis(log6, "esc").get("instructions") == "1" and vis(log6, "esc").get("endUI") == "0", "ESC: the menu over the results (toggleEndScreen)")
check(vis(log6, "back").get("endUI") == "1", "Click to Play over the results brings them back")
check(not overlaps(log6), f"CLICK TO PLAY never on the results or the title (alive at the END: {overlaps(log6)})")

script = "0 play\n" + press(300, "ESC") + "700 con end\n"
script += "".join(f"{t} vis @m{t} {END_IDS}\n" for t in (2000, 5000, 9000))
# a window over the results (v10 showWindow -> toggleMenu(true)), closed, then Click to Play: toggleEndScreen
script += f"9100 call showWindow 1\n9300 vis @w9300 {END_IDS}\n9500 call closWind\n9700 vis @w9700 {END_IDS}\n"
script += f"9800 clickid instructionHolder\n10200 vis @w10200 {END_IDS}\n13000 vis @w13000 {END_IDS}\n13100 quit\n"
log7 = run("end-menu", script, args=("--offline", "assets/maps/official/10_kanji.json"))
check(vis(log7, "m5000").get("endUI") == "1", "the END over an open menu shows the end screen")
check(vis(log7, "w9300").get("endUI") == "0" and vis(log7, "w9700").get("instructions") == "1",
      "a window over the results: the menu (CLICK TO PLAY) takes their place, as v10's toggleMenu(true)")
check(vis(log7, "w10200").get("endUI") == "1" and vis(log7, "w13000").get("endUI") == "1",
      f"then Click to Play brings the results back and they stay ({vis(log7, 'w10200')}, {vis(log7, 'w13000')})")
check(not overlaps(log7), f"CLICK TO PLAY never on the results or the title (menu open at the END, window: {overlaps(log7)})")

if failures:
    print(f"{len(failures)} check(s) failed; logs in {WORK}")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("input_typing_e2e: all checks passed")
