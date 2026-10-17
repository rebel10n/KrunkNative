#!/usr/bin/env python3
"""Krunker Strike's menu screens end to end: Store (Extras), Spin, Customize Controls and the
currency "+" buttons of FRVR Krunker Strike 1.37.2's mobile UI (ui/strike/,
client/src/ui/strike_menu.c, strike_touch.c) open from the Strike menu the way FRVR's page reaches
them, and go back.

  python3 -I server/tests/strike_menu_screens_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin --flavor strike --offline (Kanji) runs, one private config, clicking the page:
  run 1  MAIN_MENU Extras (#mobMenuStoreH) -> STORE with its Spins section's wheel blocks (OC); a
         wheel -> SPIN (title, odds, the canvas); back -> STORE (keepAlive); a wheel -> SPIN -> home
         -> MAIN_MENU (kt() twice); Extras -> the KR "+" -> STORE on its Currency section; a wheel ->
         SPIN's ticket "+" -> STORE on Spins, SPIN gone (ly()); back -> MAIN_MENU.
         The gear -> SETTINGS -> Game tab (scrolled) -> Customize Controls -> CUSTOMIZE_CONTROLS;
         the shoot button dragged 100 px left / 50 px down moves with the finger and the release
         saves the layout (mobile_layouts); back -> SETTINGS; Customize again: still moved; home ->
         MAIN_MENU (Settings goes too); play: the in-game controls are up, no shoot button with Auto Fire on
         (FRVR's default); Auto Fire off: the shoot button sits at the saved place.
  run 2  (a restart) Customize Controls: the shoot button is still moved; Reset puts it back.
  run 3  (a restart) Customize Controls: back at the default (Reset was saved); play, Auto Fire off: the in-game
         shoot button at its default place.
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-menu-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


def shop_wheels():
    """the Store's wheel blocks in OC's order: kn_brain's Strike catalogue (tools/strike/gen_economy.py),
    the paid KR / spin-ticket wheels, KR ones first by price (highest first), then the ticket ones"""
    r = subprocess.run([sys.executable, "-I", os.path.join(os.getcwd(), "tools", "strike", "gen_economy.py"), "--json"], capture_output=True)
    wheels = [w for w in json.loads(r.stdout)["wheels"] if w.get("currency") in ("kr", "spin_ticket")]
    wheels.sort(key=lambda w: (w["currency"] == "spin_ticket", -w["amount"]))
    return wheels

CUSTOMIZE = "[onclick=\"strikeAction('customize-controls')\"]"
WHEEL_BLOCK = ".sectioned-scroll:first-child .store-wheel"  # the capture renders the sections twice


def to_controls(t):
    """Settings -> Game tab -> (scrolled) Customize: the commands from time t, and the time after."""
    lines = [f"{t} clickid mobMenuSett", f"{t + 700} clicksel [onclick=\"strikeAction('settings-tab:2')\"]",
             f"{t + 1100} cursor 640 500"]
    lines += [f"{t + 1150 + 25 * i} wheel -1" for i in range(8)]
    lines += [f"{t + 1600} clicksel {CUSTOMIZE}"]
    return lines, t + 2300


def run1():
    s = ["0 menu",
         "3000 nodes @main .mobMenuAcc--main-menu",
         "3100 clickid mobMenuStoreH",
         "3800 nodes @store .frvr-store-screen",
         "3800 nodes @store-main .mobMenuAcc--main-menu",
         f"3800 nodes @wheels {WHEEL_BLOCK}",
         "3800 text @store-section .sectioned-scroll__title--active",
         f"3900 clicksel {WHEEL_BLOCK}",
         "4600 nodes @spin .frvr-spin-screen",
         "4600 nodes @spin-store .frvr-store-screen",
         "4600 nodes @spin-odds .frvr-spin-screen .odd",
         "4600 nodes @spin-canvas .spin-wheel__container canvas[width]",
         "4600 text @spin-title .spinner-title",
         "4700 clicksel .frvr-spin-screen .frvr-button-back",
         "5400 nodes @spinback .frvr-spin-screen",
         "5400 nodes @spinback-store .frvr-store-screen",
         f"5500 clicksel {WHEEL_BLOCK}",
         "6200 nodes @spin2 .frvr-spin-screen",
         "6300 clicksel .frvr-spin-screen .frvr-button-home",
         "7000 nodes @spinhome .mobMenuAcc--main-menu",
         "7000 nodes @spinhome-store .frvr-store-screen",
         "7000 nodes @spinhome-spin .frvr-spin-screen",
         "7100 clickid mobMenuStoreH",
         "7800 clicksel .currency-header .krunkies .purchase-more-btn",
         "8500 nodes @buykr-store .frvr-store-screen",
         "8500 text @buykr-section .sectioned-scroll__title--active",
         f"8600 clicksel {WHEEL_BLOCK}",
         "9300 clicksel .frvr-spin-screen .currency-header .spin-tickets .purchase-more-btn",
         "10000 nodes @buyticket-spin .frvr-spin-screen",
         "10000 nodes @buyticket-store .frvr-store-screen",
         "10000 text @buyticket-section .sectioned-scroll__title--active",
         "10100 clicksel .frvr-store-screen .frvr-button-back",
         "10800 nodes @storeback .mobMenuAcc--main-menu",
         "10800 nodes @storeback-store .frvr-store-screen"]
    lines, t = to_controls(10900)
    s += lines
    s += [f"{t} nodes @controls .control-buttons--customize",
          f"{t} nodes @controls-joysticks .control-buttons--customize .joystick",
          f"{t} rects @before .control-buttons--customize .control-button",
          # the shoot button: right 190 / top 140 / 46 units in a 568 px tall layer at (32, 120)
          f"{t + 100} cursor 914 377", f"{t + 150} mdown left", f"{t + 250} cursor 864 402",
          f"{t + 350} cursor 814 427", f"{t + 450} mup left",
          f"{t + 900} rects @after .control-buttons--customize .control-button",
          f"{t + 1000} clicksel .frvr-button-back",
          f"{t + 1700} nodes @controlsback .control-buttons--customize",
          f"{t + 1700} nodes @controlsback-settings .frvr-settings-screen",
          f"{t + 1800} clicksel {CUSTOMIZE}",
          f"{t + 2500} nodes @controls2 .control-buttons--customize",
          f"{t + 2500} rects @again .control-buttons--customize .control-button",
          f"{t + 2600} clicksel .frvr-button-home",
          f"{t + 3300} nodes @controlshome .mobMenuAcc--main-menu",
          f"{t + 3300} nodes @controlshome-settings .frvr-settings-screen",
          f"{t + 3300} nodes @controlshome-controls .control-buttons--customize",
          f"{t + 3400} play",
          f"{t + 6400} rects @ingame-auto .control-buttons .control-button",
          # Auto Fire is on by default, as in FRVR: no shoot button. Off (window.setSetting), it shows
          f"{t + 6500} call setSetting \"autoFireMbl\" \"false\"",
          f"{t + 7200} rects @ingame .control-buttons .control-button",
          f"{t + 7300} quit"]
    return s


def run2():
    lines, t = to_controls(3000)
    return ["0 menu"] + lines + [
        f"{t} rects @restart .control-buttons--customize .control-button",
        f"{t + 100} clicksel [onclick=\"strikeAction('controls-reset')\"]",
        f"{t + 800} nodes @reset .control-buttons--customize",
        f"{t + 800} rects @reset .control-buttons--customize .control-button",
        f"{t + 900} quit"]


def run3():
    lines, t = to_controls(3000)
    return ["0 menu"] + lines + [
        f"{t} rects @restart2 .control-buttons--customize .control-button",
        f"{t + 100} clicksel .frvr-button-home",
        f"{t + 800} play",
        f"{t + 3700} call setSetting \"autoFireMbl\" \"false\"",
        f"{t + 4400} rects @ingame2 .control-buttons .control-button",
        f"{t + 4500} quit"]


def seed_played(cfg):
    """A returning player (the FTUE gate, client/src/ui/strike_ftue.c): the boot goes to the menu, not quick connect.
    The file is in the Strike flavor's own config directory (flavor_storage_subdir: "krunknative-strike", not Box
    Office's "krunknative"), where a --flavor strike client reads its html storage keys."""
    path = os.path.join(cfg, "krunknative-strike", "knFtue.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write('{"guest":{"played":true}}')


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script_lines):
    seed_played(os.path.join(WORK, "cfg"))
    script = os.path.join(WORK, f"{name}.script")
    with open(script, "w") as f:
        f.write("\n".join(script_lines) + "\n")
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    # KRUNKNATIVE_FIXED_DELTA: the script clock advances one 60 fps frame per frame. Without it a page
    # hitch (the user-asset fetches stall a frame for seconds) moves the clock past several lines at once,
    # and they all run in one frame: the SPIN canvas check and the Customize Controls rect reads then
    # see the screen before its first layout and canvas draw (the canvas gets its width attribute and the
    # shoot button its saved place only in a frame that renders them). strike_screens_e2e.py pins it too.
    # a phone (ui_mobile.c detect_mobile): the touch layer is the in-game input and Customize Controls edits it
    # (strike_touch_layer); a desktop has no shoot button or menu button in game
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_MOBILE": "1"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--flavor", "strike", "--offline", "--map", "kanji"],
                             stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        text = f.read()
    check(f"{name}: the Strike page loaded", "[strike] Krunker Strike page ready" in text, log)
    nodes, texts, rects = {}, {}, {}
    for m in re.finditer(r"\[input_script\] nodes t=\d+ @(\S+) elements=\d+ match=(\d+)", text):
        nodes[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"\[input_script\] text t=\d+ @(\S+) n=\d+ \S+ value=\S* text=(.*)", text):
        texts[m.group(1)] = m.group(2).strip()
    for m in re.finditer(r"\[input_script\] rects t=\d+ @(\S+) n=\d+(.*)", text):
        rects[m.group(1)] = [tuple(float(v) for v in r.split("@")[1].split(",")) for r in m.group(2).split()]
    return text, nodes, texts, rects


def menu_button(rects, label):
    """the in-game layer's backToMenu button (Gw's last .control-button), shown whatever Auto Fire is"""
    boxes = rects.get(label, [])
    return boxes[4] if len(boxes) > 4 and boxes[4][2] > 0 else None


def shoot(rects, label):
    """The shoot button's box: .control-button matches come in Gw's order (jump, reload, shoot,
    shootWhenAim, backToMenu) on both the customize layer and the in-game one."""
    boxes = rects.get(label, [])
    return boxes[2] if len(boxes) > 2 and boxes[2][2] > 0 else None


def near(a, b, tol=4.0):
    return a is not None and b is not None and abs(a[0] - b[0]) <= tol and abs(a[1] - b[1]) <= tol


EXPECT1 = [
    ("main", 1, "the Strike main menu is up"),
    ("store", 1, "Extras opens STORE"),
    ("store-main", 0, "STORE replaces the main menu"),
    ("wheels", lambda n: n >= 3, "STORE's Spins section shows the shop's wheel blocks (OC)"),
    ("spin", 1, "a wheel block opens SPIN (eb -> Pt(SA))"),
    ("spin-store", 1, "STORE stays rendered under SPIN (keepAlive)"),
    ("spin-odds", "ODDS", "SPIN lists the clicked wheel's odds (oU)"),
    ("spin-canvas", 1, "SPIN's wheel canvas is sized and drawn (aue.resize)"),
    ("spinback", 0, "SPIN's back closes it"),
    ("spinback-store", 1, "SPIN's back returns to STORE"),
    ("spin2", 1, "a wheel opens SPIN again"),
    ("spinhome", 1, "SPIN's home returns to the main menu"),
    ("spinhome-store", 0, "SPIN's home closes STORE too (kt() twice)"),
    ("spinhome-spin", 0, "SPIN's home closes SPIN"),
    ("buykr-store", 1, "the KR \"+\" keeps STORE up"),
    ("buyticket-spin", 0, "SPIN's ticket \"+\" closes SPIN (ly())"),
    ("buyticket-store", 1, "SPIN's ticket \"+\" opens STORE"),
    ("storeback", 1, "STORE's back returns to the main menu"),
    ("storeback-store", 0, "STORE's back closes it"),
    ("controls", 1, "Settings > Game > Customize opens CUSTOMIZE_CONTROLS"),
    ("controls-joysticks", 2, "CUSTOMIZE_CONTROLS shows the two joysticks"),
    ("controlsback", 0, "CUSTOMIZE_CONTROLS' back closes it"),
    ("controlsback-settings", 1, "CUSTOMIZE_CONTROLS' back returns to SETTINGS"),
    ("controls2", 1, "Customize opens CUSTOMIZE_CONTROLS again"),
    ("controlshome", 1, "CUSTOMIZE_CONTROLS' home returns to the main menu"),
    ("controlshome-settings", 0, "CUSTOMIZE_CONTROLS' home closes SETTINGS too (onHide + kt)"),
    ("controlshome-controls", 0, "CUSTOMIZE_CONTROLS' home closes it"),
]


def main():
    wheels = shop_wheels()
    last = wheels[-1] if wheels else {"slices": [], "id": "?"}
    names = {"starter": "Starter", "elite": "Elite", "lucky_draw": "Lucky Draw"}  # strike_economy_data.c k_wheels
    text, nodes, texts, rects = run("run1", run1())
    for label, want, name in EXPECT1:
        if want == "ODDS":
            want = len(last["slices"])
        n = nodes.get(label)
        ok = n is not None and (want(n) if callable(want) else n == want)
        check(name, ok, f"@{label} match={n}" + ("" if callable(want) else f" (want {want})"))
    check("STORE opens on its first section (Spins)", texts.get("store-section") == "Spins", texts.get("store-section"))
    check("SPIN's title is the clicked wheel's (the last block's) name", texts.get("spin-title") == names.get(last["id"]),
          f"{texts.get('spin-title')} vs {names.get(last['id'])}")
    check("the KR \"+\" selects STORE's Currency section", texts.get("buykr-section") == "Currency", texts.get("buykr-section"))
    check("the ticket \"+\" selects STORE's Spins section", texts.get("buyticket-section") == "Spins", texts.get("buyticket-section"))
    before, after, again = shoot(rects, "before"), shoot(rects, "after"), shoot(rects, "again")
    moved = before and after and abs((after[0] - before[0]) + 100) <= 3 and abs((after[1] - before[1]) - 50) <= 3
    check("dragging the shoot button moves it with the finger (-100, +50 px)", moved, f"{before} -> {after}")
    check("the release saves the layout", "[strike] controls layout saved" in text)
    check("Customize Controls opened again keeps the moved button", near(after, again), f"{again}")
    check("the in-game controls exist after play (the menu button)", menu_button(rects, "ingame-auto") is not None, str(rects.get("ingame-auto")))
    check("Auto Fire on (FRVR's default): no shoot button", shoot(rects, "ingame-auto") is None, str(rects.get("ingame-auto")))
    ingame1 = shoot(rects, "ingame")
    check("Auto Fire off: the shoot button shows", ingame1 is not None, str(rects.get("ingame")))

    text, nodes, texts, rects = run("run2", run2())
    restart = shoot(rects, "restart")
    check("after a restart Customize Controls shows the saved place", near(after, restart), f"{restart}")
    check("Reset ran Ahe()", "[strike] controls reset" in text)
    check("Reset keeps CUSTOMIZE_CONTROLS up", nodes.get("reset") == 1, str(nodes.get("reset")))
    reset = shoot(rects, "reset")
    check("Reset puts the shoot button back at its default place", near(before, reset), f"{reset}")

    text, nodes, texts, rects = run("run3", run3())
    check("Reset was saved: after a restart the default place", near(before, shoot(rects, "restart2")), f"{shoot(rects, 'restart2')}")
    ingame2 = shoot(rects, "ingame2")
    check("the in-game shoot button follows the saved layout (moved left of its default)",
          ingame1 is not None and ingame2 is not None and ingame2[0] - ingame1[0] > 50, f"moved {ingame1} default {ingame2}")
    return all(results)


ok = main()
if ok:
    shutil.rmtree(WORK, ignore_errors=True)
print(f"\n{sum(results)}/{len(results)} passed" + ("" if ok else f"  (logs: {WORK})"))
sys.exit(0 if ok else 1)
