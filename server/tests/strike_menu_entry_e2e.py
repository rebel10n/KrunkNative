#!/usr/bin/env python3
"""Krunker Strike's menu entry points end to end (FRVR Krunker Strike 1.37.2's mobile UI): the KR
limit's info bubble on MAIN_MENU, the Settings presets (selectSettingPre: a preset sets its values,
Custom restores the values saved when it was left, a hand change makes the preset Custom), and a
locked class's unlock line on CLASS_SELECTION (the store for bundles and spin tickets, the profile
for the Rewards Track).

  python3 -I server/tests/strike_menu_entry_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin --flavor strike --offline (Kanji), one private config, clicking the page:
  the info icon opens the bubble, a second tap on the info icon closes it, the info icon reopens it, and it
  hides by itself after 5 s (a tap on the bubble itself is not tested: it lands on the Play button under it)
  Settings: Custom is active at first; Pro sets resolution 1 / fov 110; Custom restores 0.4 / 70;
  Performance; a Display checkbox made by hand turns the preset to Custom (highlighted)
  Customize > class: Triggerman (owned) shows Selected; Hunter (bundle) and Run N Gun (track) show their
  unlock lines; the line opens STORE for Hunter and PROFILE for Run N Gun; back returns.
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
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
WORK = tempfile.mkdtemp(prefix="kn-strike-entry-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


def script():
    s = ["0 menu",
         "3000 nodes @main .frvr-kr-limit__info",
         "3100 clicksel .frvr-kr-limit__info",
         "3400 nodes @bubble .speech-bubble-container",
         "3400 text @bubble-text .speech-bubble-kr-limit",
         "3500 clicksel .frvr-kr-limit__info",
         "3800 nodes @bubble-closed .speech-bubble-container",
         "3900 clicksel .frvr-kr-limit__info",
         "4000 nodes @bubble-again .speech-bubble-container",
         "9600 clickid mobMenuSett",
         "10300 nodes @settings .frvr-preset__button--active",
         "10300 text @custom-active .frvr-preset__button--active",
         "10400 clicksel [onclick=\"strikeAction('preset:1')\"]",
         "11000 text @pro-active .frvr-preset__button--active",
         "11100 clicksel [onclick=\"strikeAction('preset:3')\"]",
         "11700 text @custom-back .frvr-preset__button--active",
         "11800 clicksel [onclick=\"strikeAction('preset:2')\"]",
         "12400 text @perf-active .frvr-preset__button--active",
         "12500 clicksel [onclick=\"strikeAction('settings-tab:1')\"]",
         "13100 call strikeSetting \"showFPS\" \"0\"",
         "13300 clicksel [onclick=\"strikeAction('settings-tab:0')\"]",
         "13900 text @manual-active .frvr-preset__button--active",
         "14000 clicksel .frvr-button-back",
         "14600 clickid mobMenuCustomize",
         "15300 clicksel [onclick=\"strikeAction('class-selection')\"]",
         "16000 text @own-box .class-selection__select-class",
         "16100 clicksel [onclick=\"strikeAction('class:1')\"]",
         "16700 nodes @unlock-hunter .class-selection__unlock-info",
         "16700 text @unlock-hunter-text .class-selection__unlock-info span",
         "16800 clicksel .class-selection__unlock-info",
         "17400 nodes @store .frvr-store-screen",
         "17500 clicksel .frvr-store-screen .frvr-button-back",
         "18100 nodes @class-again .class-selection__class-bullet-holder",
         "18200 clicksel [onclick=\"strikeAction('class:2')\"]",
         "18800 text @unlock-track-text .class-selection__unlock-info span",
         "18900 clicksel .class-selection__unlock-info",
         "19500 nodes @profile .frvr-account-progression",
         "19500 nodes @profile-class .class-selection__class-bullet-holder",
         "19600 clicksel .frvr-settings-screen .frvr-button-back",
         "20200 nodes @class-back .class-selection__class-bullet-holder",
         "20900 clicksel .frvr-button-back",
         "21500 clicksel .frvr-button-back",
         "22100 nodes @main-back .frvr-kr-limit",
         "22100 nodes @bubble-gone .speech-bubble-container",
         "22200 quit",
         ]
    return s


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script_lines):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write("\n".join(script_lines) + "\n")
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FIXED_DELTA": "16.667"})
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
    nodes, texts = {}, {}
    for m in re.finditer(r"\[input_script\] nodes t=\d+ @(\S+) elements=\d+ match=(\d+)", text):
        nodes[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"\[input_script\] text t=\d+ @(\S+) n=\d+ \S+ value=\S* text=(.*)", text):
        texts[m.group(1)] = m.group(2).strip()
    return text, nodes, texts


def main():
    text, nodes, texts = run("run1", script())
    check("the KR info icon is on MAIN_MENU", nodes.get("main") == 1, str(nodes.get("main")))
    check("the info icon opens the bubble", nodes.get("bubble") == 1, str(nodes.get("bubble")))
    check("the bubble says the KR limit", "limit to the KRs" in texts.get("bubble-text", ""), texts.get("bubble-text", ""))
    check("the info icon closes the open bubble", nodes.get("bubble-closed") == 0, str(nodes.get("bubble-closed")))
    check("the info icon reopens the bubble", nodes.get("bubble-again") == 1, str(nodes.get("bubble-again")))
    check("the bubble hides by itself after 5 s (on the main menu it was opened on)", nodes.get("bubble-gone") == 0, str(nodes.get("bubble-gone")))

    check("Settings opens with one active preset", nodes.get("settings") == 1, str(nodes.get("settings")))
    check("Custom is the start preset", texts.get("custom-active") == "Custom", texts.get("custom-active"))
    check("Pro sets its values (resolution 1, fov 110)", "[strike] preset 1: resolution 1, fov 110" in text)
    check("Pro is highlighted", texts.get("pro-active") == "Pro", texts.get("pro-active"))
    saved = re.search(r"\[strike\] custom saved: (.*)", text)
    restored = re.search(r"\[strike\] custom restored: (.*)", text)
    check("leaving Custom saves its values, and Custom restores exactly those",
          saved is not None and restored is not None and saved.group(1) == restored.group(1),
          f"saved {saved.group(1) if saved else None}, restored {restored.group(1) if restored else None}")
    check("Custom is highlighted again", texts.get("custom-back") == "Custom", texts.get("custom-back"))
    check("Performance sets its values", "[strike] preset 3: resolution 0.4, fov 70" in text)
    check("Performance is highlighted", texts.get("perf-active") == "Performance", texts.get("perf-active"))
    check("a hand change in Display makes the preset Custom", texts.get("manual-active") == "Custom", texts.get("manual-active"))

    check("Customize's class selection shows Selected for the owned Triggerman",
          "Selected" in texts.get("own-box", ""), texts.get("own-box", ""))
    check("Hunter (a bundle unlock) shows its unlock line", nodes.get("unlock-hunter") == 1, str(nodes.get("unlock-hunter")))
    check("the unlock line names the bundle", "Bundle purchase" in texts.get("unlock-hunter-text", ""), texts.get("unlock-hunter-text", ""))
    check("the bundle's unlock line opens STORE", nodes.get("store") == 1, str(nodes.get("store")))
    check("STORE's back returns to CLASS_SELECTION", nodes.get("class-again", 0) >= 1, str(nodes.get("class-again")))
    check("Run N Gun (a Rewards Track unlock) shows its unlock line",
          "Rewards Track" in texts.get("unlock-track-text", ""), texts.get("unlock-track-text", ""))
    check("the track's unlock line opens the profile", nodes.get("profile") == 1, str(nodes.get("profile")))
    check("the profile's back returns to CLASS_SELECTION", nodes.get("class-back", 0) >= 1, str(nodes.get("class-back")))
    check("the menu is back after CUSTOMIZE and CLASS_SELECTION", nodes.get("main-back") == 1, str(nodes.get("main-back")))
    return all(results)


ok = main()
if ok:
    shutil.rmtree(WORK, ignore_errors=True)
print(f"\n{sum(results)}/{len(results)} passed" + ("" if ok else f"  (logs: {WORK})"))
sys.exit(0 if ok else 1)
