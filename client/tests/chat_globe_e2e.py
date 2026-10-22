#!/usr/bin/env python3
"""The chat's globe icon (#chatSwitch, the text & voice channel) end to end (client/src/ui/hud_ui.c
hud_switch_chat, the page markup in ui/index.html, v10.0.3 window.switchChat: doc/v10/chat-globe.md).

  python3 -I client/tests/chat_globe_e2e.py BUILD_DIR      (run from the repository root)

The globe is a toggle, not a picker: a click flips data-tab between "public" (Global) and "groups"
(Team) only in a team mode; elsewhere it stays Global. Each click plays select_0 at 0.1 (v10's
playSelect(0.1)); a team switch adds the system line "Team Text & Voice Chat" /
"Global Text & Voice Chat"; nothing is stored (no setting, no file); the channel is what the next
typed line is sent on ("[chat team]" on the server log, the sandbox's line for a team message).

Headless client_bin runs, offline Kanji, input scripts, the Escape menu open (the cursor is free there;
in game the cursor is locked and a click is the shot, in v10 too). Three runs per mode: a control
without the click and the run with it, so the select_0 count is compared, not guessed.
  A. Free for All: the globe is Global; a click keeps it Global, no system line, one select_0 more
     than the control. Screenshots: the menu open with the globe, and after the click.
  B. Team Deathmatch (KRUNKNATIVE_MODE=tdm): a click shows Team (data-tab groups, the team system line,
     one select_0 more); a typed line is sent on the team channel; a second click shows Global again
     (the global system line); a typed line after it is sent on the global channel. Screenshots:
     open, after the first choice, after the second.
  C. Nothing is stored: no key of the saved settings (knSettings.json) names the channel.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
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
REPO = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-chat-globe-", dir=CACHE if os.path.isdir(CACHE) else None)
SHOTS = os.environ.get("KN_CHAT_GLOBE_SHOTS") or os.path.join(CACHE, "kn-chat-globe", "shots")
os.makedirs(SHOTS, exist_ok=True)
CFG = os.path.join(WORK, "cfg")
STORE = os.path.join(CFG, "krunknative")
KEY = os.path.join(STORE, "knSettings.json")
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script, mode=None, timeout=180):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_MODE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": CFG, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_AUDIO_LOG": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    if mode:
        env["KRUNKNATIVE_MODE"] = mode
    env.setdefault("KRUNKNATIVE_ASSETS_DIR", os.path.join(REPO, "assets"))
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        text = f.read()
    return text


def attrs(text):
    # "[input_script] attr t=1700 @a0 n=1 data-tab=public" -> {label: value}
    return {m.group(1): m.group(2) for m in re.finditer(r"\[input_script\] attr t=\d+ @(\S+) n=\d+ data-tab=(\S+)", text)}


def hits(text, label):
    m = re.search(r"\[input_script\] grep t=\d+ @" + re.escape(label) + r" needle=\S+ n=\d+ hits=(\d+)", text)
    return int(m.group(1)) if m else None


def selects(text):
    # every 2D select_0 the audio layer starts (KRUNKNATIVE_AUDIO_LOG prints "[audio] t=.. play2d select_0 vol=..")
    return len(re.findall(r"\bplay2d select_0\b", text))


def sent_channel(text, word):
    # the sandbox echoes a chat line as "[sandbox] [chat team] Guest_1: team-typed-line" (team) and
    # "[sandbox] [chat] Guest_1: global-typed-line" (global)
    tag = r"\[chat team\]" if word == "team" else r"\[chat\]"
    return bool(re.search(r"\[sandbox\] " + tag + r" \S+: " + re.escape(word) + r"-typed-line", text))


def stored_channel():
    try:
        with open(KEY) as f:
            text = f.read()
    except OSError:
        return None
    return bool(re.search(r"chatSwitch|chatChannel|\"groups\"|chat.*[Cc]hannel", text))


def open_menu(t):
    return f"{t} down ESC\n{t + 50} up ESC\n"


def typed(t, word):
    return f"{t} down ENTER\n{t + 50} up ENTER\n{t + 100} type {word}\n{t + 200} down ENTER\n{t + 250} up ENTER\n"


def ffa_script(click):
    s = "0 play\n" + open_menu(1500)
    s += f"1800 frames {SHOTS}/a-ffa-open\n"
    s += "1850 attr @a0 data-tab [id=chatSwitch]\n"
    if click:
        s += "1900 clickid chatSwitch\n"
    s += "2100 attr @a1 data-tab [id=chatSwitch]\n"
    s += "2150 grep @g1 Team [id=chatList]\n"
    s += f"2200 frames {SHOTS}/a-ffa-after\n"
    s += "2300 quit\n"
    return s


def team_script(click):
    s = "0 play\n" + open_menu(1500)
    s += f"1800 frames {SHOTS}/b-team-open\n"
    s += "1850 attr @b0 data-tab [id=chatSwitch]\n"
    if click:
        s += "1900 clickid chatSwitch\n"
    s += "2100 attr @b1 data-tab [id=chatSwitch]\n"
    s += "2150 grep @b1t Team [id=chatList]\n"
    s += f"2200 frames {SHOTS}/b-team-first\n"
    s += "2250 down ESC\n2300 up ESC\n"
    s += typed(2400, "team-typed-line")
    s += open_menu(2900)
    if click:
        s += "3000 clickid chatSwitch\n"
    s += "3200 attr @b2 data-tab [id=chatSwitch]\n"
    s += "3250 grep @b2g Global [id=chatList]\n"
    s += f"3300 frames {SHOTS}/b-team-second\n"
    s += "3350 down ESC\n3400 up ESC\n"
    s += typed(3500, "global-typed-line")
    s += "4200 quit\n"
    return s


def main():
    os.makedirs(STORE, exist_ok=True)
    with open(os.path.join(STORE, "krk_advanced.json"), "w") as f:
        f.write("1")

    # A. Free for All
    ctrl = run("ffa-control", ffa_script(False))
    text = run("ffa", ffa_script(True))
    a = attrs(text)
    check("A: the globe is Global (public) before a click in FFA", attrs(ctrl).get("a0") == "public", str(attrs(ctrl)))
    check("A: a click in FFA keeps the globe Global", a.get("a1") == "public", str(a))
    check("A: a click in FFA shows no team system line", hits(text, "g1") == 0, f"hits {hits(text, 'g1')}")
    check("A: a click in FFA plays select_0 once (v10 playSelect(0.1))", selects(text) - selects(ctrl) == 1,
          f"clicked {selects(text)} control {selects(ctrl)}")

    # B. Team Deathmatch
    ctrl = run("tdm-control", team_script(False), mode="tdm")
    text = run("tdm", team_script(True), mode="tdm")
    b = attrs(text)
    check("B: the globe is Global before the first click", b.get("b0") == "public", str(b))
    check("B: the first click switches to Team (data-tab groups)", b.get("b1") == "groups", str(b))
    check("B: the team switch says so in the chat", (hits(text, "b1t") or 0) >= 1, f"hits {hits(text, 'b1t')}")
    check("B: each of the two clicks plays select_0 (v10 playSelect(0.1) on every switchChat)",
          selects(text) - selects(ctrl) == 2, f"clicked {selects(text)} control {selects(ctrl)}")
    check("B: a line typed in Team is sent on the team channel", sent_channel(text, "team"), "server log")
    check("B: the second click switches back to Global (data-tab public)", b.get("b2") == "public", str(b))
    check("B: the Global switch says so in the chat", (hits(text, "b2g") or 0) >= 1, f"hits {hits(text, 'b2g')}")
    check("B: a line typed after it is sent on the global channel", sent_channel(text, "global"), "server log")

    # C. nothing stored
    check("C: the channel is not stored (no saved setting names it)", stored_channel() in (None, False),
          f"stored {stored_channel()!r}")

    print(f"\nscreenshots: {SHOTS}")
    ok = bool(results) and all(results)
    print(f"{'ALL PASSED' if ok else 'FAILED'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
