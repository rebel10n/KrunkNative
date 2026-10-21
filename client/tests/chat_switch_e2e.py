#!/usr/bin/env python3
"""Settings > Chat "Show Player Messages" (showMessages), default ON, end to end (client/src/ui/hud_ui.c,
ui_controls.c, ui_render_settings.c, client/src/net/game_events.c, ui/windows/settings-advanced-2.html).

  python3 -I client/tests/chat_switch_e2e.py BUILD_DIR      (run from the repository root)

The switch gates human players only. A bot's line (the server's bot chat: gg, kill comments) and system
lines (kill feed, notices, errors) are always shown: the server decides whether a bot line exists at all.

Headless client_bin runs (offline Kanji, input scripts, a private XDG_CONFIG_HOME kept across the runs).
The `netchat` input-script command injects a server chat line: 0 a human's line (uid 2, a guest: no
PLAYER_INFO), 3 a bot's line (uid 1, after the bot PLAYER_INFO the server sends), 1 game, 2 server.
Rows in #chatList carry data-kind: "human", "bot", "line". Every typed line goes through the same chat-box
steps (down ENTER opens it, type, down ENTER sends), started a moment after `play` so the game accepts chat.
  A. the default (nothing stored): the Settings switch is there and on; a human line is shown, a bot line
     and a system line are; plain text typed into the chat box and sent with Enter is shown (echo) and is
     in the server log (the positive control for the log probe); "/clear" still works from the chat box.
  B. the switch clicked off in the Settings window (from the default): a human line shown before is removed,
     the bot rows stay; stored "false"; a later human line is not shown, and plain text typed and sent with
     Enter is neither shown nor sent (no echo, nothing in the server log). The same steps as A's send, so
     the off checks are not vacuous: A's echo is the positive control.
  C. the stored "false" survives a restart (no human line shown at once); then the switch is clicked on in
     the Settings window: stored "true", a human line is shown, and plain text typed and sent is shown and
     reaches the server again.
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
WORK = tempfile.mkdtemp(prefix="kn-chat-switch-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
STORE = os.path.join(CFG, "krunknative")
KEY = os.path.join(STORE, "knSettings.json")  # every Settings value: UI_SETTINGS_STORAGE_KEY (ui_settings.c)
SWITCH_ROW = "[data-show-messages-setting]"
SWITCH = SWITCH_ROW + " .switch"
CHECKED = SWITCH_ROW + " input:checked"
HUMAN = '[data-kind="human"]'
BOT = '[data-kind="bot"]'
LINE = '[data-kind="line"]'
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def stored():
    # the saved showMessages value as the store holds it: "true" / "false", None when nothing is saved
    try:
        with open(KEY) as f:
            v = json.load(f).get("showMessages")
    except (OSError, ValueError, AttributeError):
        return None
    if v is None:
        return None
    return "true" if v is True else "false" if v is False else str(v)


def run(name, script, timeout=180):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": CFG, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
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
    counts = {m.group(1): int(m.group(2)) for m in re.finditer(r"\[input_script\] nodes t=\d+ (@\S+) elements=\d+ match=(\d+)", text)}
    return counts, text


def open_settings(t, label):
    # the Settings window (windows[0]) in the advanced view (the store's krk_advanced), on the Display tab
    # (advanced tab 2, settings-advanced-2.html: the Chat section). The scroll units are not window pixels
    # (the page is scaled ~0.58): 3200 puts the Chat switch at y~305 in the window (menuWindow 61..659)
    return (f"{t} call showWindow 1\n{t + 200} call windows.0.changeTab 2\n{t + 600} setscroll menuWindow 3200\n"
            f"{t + 800} nodes {label}-exists {SWITCH_ROW}\n{t + 850} nodes {label} {CHECKED}\n")


def main():
    os.makedirs(STORE, exist_ok=True)
    with open(os.path.join(STORE, "krk_advanced.json"), "w") as f:
        f.write("1")

    # A. the default
    n, log = run("default", "0 menu\n" + open_settings(500, "@a-switch") + f"""1600 call closWind
2000 play
2100 nodes @a-p0 {HUMAN}
2100 nodes @a-b0 {BOT}
2100 nodes @a-l0 {LINE}
2200 netchat 0 hidden-human-line
2300 nodes @a-p1 {HUMAN}
2400 netchat 3 shown-bot-line
2500 nodes @a-b1 {BOT}
2600 netchat 2 shown-server-line
2700 nodes @a-l1 {LINE}
2800 down ENTER
2850 up ENTER
2900 type typed-plain-default
3000 down ENTER
3050 up ENTER
3200 nodes @a-p2 {HUMAN}
3300 netchat 2 before-clear
3400 nodes @a-l2 {LINE}
3500 down ENTER
3550 up ENTER
3600 type /clear
3700 down ENTER
3750 up ENTER
3900 nodes @a-l3 {LINE}
4000 quit
""")
    check("A: the Settings switch is there (Display tab, Chat)", n.get("@a-switch-exists") == 1, str(n))
    check("A: the Settings switch is on by default", n.get("@a-switch") == 1, str(n))
    check("A: a human player's line is shown", n.get("@a-p1", 0) == n.get("@a-p0", 0) + 1, str(n))
    check("A: a bot's line is shown", n.get("@a-b1", 0) == n.get("@a-b0", 0) + 1, str(n))
    check("A: a system line is shown", n.get("@a-l1", 0) == n.get("@a-l0", 0) + 1, str(n))
    check("A: plain text sent from the chat box is shown (echo)", n.get("@a-p2", 0) == n.get("@a-p1", 0) + 1, str(n))
    check("A: plain text sent by default reaches the server (positive control for the log probe)", "typed-plain-default" in log)
    check("A: the chat box still takes /clear (the command path)", n.get("@a-l2", 0) > 0 and n.get("@a-l3") == 0, str(n))
    check("A: nothing stored, or on, in the store", stored() in (None, "true"), f"stored {stored()!r}")

    # B. the switch clicked off in the Settings window, from the default; a human line shown before is removed
    n, log = run("off", "0 menu\n300 netchat 0 shown-before-off\n350 netchat 3 bot-shown-before-off\n"
                 f"{open_settings(500, '@b-on')}"
                 f"""2400 clicksel {SWITCH}
2700 nodes @b-off {CHECKED}
2800 nodes @b-cleared {HUMAN}
2810 nodes @b-bcleared {BOT}
2900 call closWind
3000 play
3300 netchat 0 hidden-after-off
3400 nodes @b-after {HUMAN}
3450 netchat 3 bot-after-off
3500 nodes @b-bafter {BOT}
3600 netchat 2 system-after-off
3700 nodes @b-sys {LINE}
3900 down ENTER
3950 up ENTER
4000 type typed-plain-off
4200 down ENTER
4250 up ENTER
4400 nodes @b-typed {HUMAN}
4500 quit
""")
    check("B: a human line is shown before the switch is turned off", n.get("@b-on", 0) == 1 and "shown-before-off" in log, str(n))
    check("B: the Settings switch starts on (the default)", n.get("@b-on") == 1, str(n))
    check("B: clicking the switch unchecks it and stores false", n.get("@b-off") == 0 and stored() == "false", f"{n} stored {stored()!r}")
    check("B: turning it off removes the human lines already shown", n.get("@b-cleared") == 0, str(n))
    check("B: turning it off keeps the bot lines already shown", n.get("@b-bcleared") == 1, str(n))
    check("B: a human line after the switch is off is not shown", n.get("@b-after") == 0, str(n))
    check("B: a bot line after the switch is off is shown", n.get("@b-bafter", 0) == n.get("@b-bcleared", 0) + 1, str(n))
    check("B: a system line is still shown", n.get("@b-sys", 0) > 0, str(n))
    check("B: plain text sent while off is not shown (no echo)", n.get("@b-typed") == n.get("@b-after"), str(n))
    check("B: plain text sent while off never reaches the server", "typed-plain-off" not in log)

    # C1. the stored false survives a restart: in game, a human line is not shown
    before = stored()
    n, log = run("restart", """0 play
1000 nodes @c-p0 """ + HUMAN + """
1000 nodes @c-b0 """ + BOT + """
1100 netchat 0 hidden-after-restart
1200 nodes @c-p1 """ + HUMAN + """
1250 netchat 3 bot-after-restart
1300 nodes @c-b1 """ + BOT + """
1400 quit
""")
    check("C: the stored false survives a restart (no human line shown)", before == "false" and n.get("@c-p1") == n.get("@c-p0"), f"{n} stored {before!r}")
    check("C: a bot line is shown after the restart", n.get("@c-b1", 0) == n.get("@c-b0", 0) + 1, str(n))

    # C2. the switch clicked on in the Settings window: a human line shows, and plain text is sent again
    n, log = run("on", "0 menu\n" + open_settings(500, "@c-off") + f"""2400 clicksel {SWITCH}
2700 nodes @c-on {CHECKED}
2800 call closWind
3000 play
3300 netchat 0 shown-after-on
3400 nodes @c-p2 {HUMAN}
3900 down ENTER
3950 up ENTER
4000 type typed-plain-on
4200 down ENTER
4250 up ENTER
4400 nodes @c-p3 {HUMAN}
4500 quit
""")
    check("C: the Settings switch starts off after the restart", n.get("@c-off") == 0, str(n))
    check("C: clicking the switch on checks it and stores true", n.get("@c-on") == 1 and stored() == "true", f"{n} stored {stored()!r}")
    check("C: a human player's line is shown once it is on", n.get("@c-p2") == 1, str(n))
    check("C: plain text from the chat box is shown (echo)", n.get("@c-p3", 0) == n.get("@c-p2", 0) + 1, str(n))
    check("C: the typed line reached the server", "typed-plain-on" in log)

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
