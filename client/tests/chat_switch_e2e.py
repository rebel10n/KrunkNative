#!/usr/bin/env python3
"""Settings > Chat "Show Player Messages" (showMessages), default OFF, end to end (client/src/ui/hud_ui.c,
ui_controls.c, ui_render_settings.c, client/src/net/game_events.c).

  python3 -I client/tests/chat_switch_e2e.py BUILD_DIR      (run from the repository root)

The switch gates human players only. A bot's line (the server's bot chat: gg, kill comments) and system
lines (kill feed, notices, errors) are always shown: the server decides whether a bot line exists at all.

Headless client_bin runs (offline Kanji, input scripts, a private XDG_CONFIG_HOME kept across the runs).
The `netchat` input-script command injects a server chat line: 0 a human's line (uid 2, a guest: no
PLAYER_INFO), 3 a bot's line (uid 1, after the bot PLAYER_INFO the server sends), 1 game, 2 server.
Rows in #chatList carry data-kind: "human", "bot", "line".
  A. the default (nothing stored): the Settings switch is there and off; a human line is not shown, a
     bot line and a system line are; plain text typed into the chat box and sent with Enter is not sent
     (no echo, nothing in the server log); "/clear" still works from the chat box.
  B. the switch clicked in the Settings window: stored "true", the human line and the typed line show
     (positive control: the server log has the typed line, so the log probe is meaningful).
  C. the stored value survives a restart (human line shown at once); then the switch is clicked off from
     the in-game Settings: stored "false", the human rows already shown are removed, the bot rows stay,
     and a later human line is not shown while a bot line and a system line are.
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
2900 type typed-plain-off
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
    check("A: the Settings switch is off by default", n.get("@a-switch") == 0, str(n))
    check("A: a human player's line is not shown", n.get("@a-p1") == n.get("@a-p0"), str(n))
    check("A: a bot's line is shown", n.get("@a-b1", 0) == n.get("@a-b0", 0) + 1, str(n))
    check("A: a system line is shown", n.get("@a-l1", 0) == n.get("@a-l0", 0) + 1, str(n))
    check("A: plain text sent from the chat box is not shown (no echo)", n.get("@a-p2") == n.get("@a-p0"), str(n))
    check("A: plain text is not sent to the server", "typed-plain-off" not in log)
    check("A: the chat box still takes /clear (the command path)", n.get("@a-l2", 0) > 0 and n.get("@a-l3") == 0, str(n))
    check("A: the switch is unchecked in the store", stored() in (None, "false"), f"stored {stored()!r}")

    # B. the switch on, from the Settings window
    n, log = run("on", "0 menu\n" + open_settings(500, "@b-before") + f"""1400 clicksel {SWITCH}
1700 nodes @b-on {CHECKED}
1800 call closWind
2000 play
2400 nodes @b-p0 {HUMAN}
2500 netchat 0 shown-human-line
2600 nodes @b-p1 {HUMAN}
2800 down ENTER
2850 up ENTER
2900 type typed-plain-on
3000 down ENTER
3050 up ENTER
3200 nodes @b-p2 {HUMAN}
3300 quit
""")
    check("B: the Settings switch starts unchecked", n.get("@b-before") == 0, str(n))
    check("B: clicking the switch checks it and stores true", n.get("@b-on") == 1 and stored() == "true", f"{n} stored {stored()!r}")
    check("B: a human player's line is shown", n.get("@b-p1", 0) == n.get("@b-p0", 0) + 1, str(n))
    check("B: plain text from the chat box is shown (echo)", n.get("@b-p2", 0) == n.get("@b-p1", 0) + 1, str(n))
    check("B: the typed line reached the server (positive control for the log probe)", "typed-plain-on" in log)

    # C. persisted on, then off from the in-game Settings
    n, log = run("off", f"""0 play
1000 nodes @c-p0 {HUMAN}
1000 nodes @c-b0 {BOT}
1100 netchat 0 kept-from-on-run
1200 nodes @c-p1 {HUMAN}
1250 netchat 3 bot-kept-while-on
1300 nodes @c-b1 {BOT}
1400 down ESC
1450 up ESC
{open_settings(1500, "@c-on")}2400 clicksel {SWITCH}
2700 nodes @c-off {CHECKED}
2800 nodes @c-cleared {HUMAN}
2810 nodes @c-bcleared {BOT}
2900 call closWind
3100 play
3300 netchat 0 hidden-after-off
3400 nodes @c-after {HUMAN}
3450 netchat 3 bot-after-off
3500 nodes @c-bafter {BOT}
3600 netchat 2 system-after-off
3700 nodes @c-sys {LINE}
3900 quit
""")
    check("C: the stored true survives a restart (human line shown)", n.get("@c-p1", 0) == n.get("@c-p0", 0) + 1, str(n))
    check("C: the in-game Settings switch is checked", n.get("@c-on") == 1, str(n))
    check("C: clicking it off stores false", stored() == "false" and n.get("@c-off") == 0, f"{n} stored {stored()!r}")
    check("C: turning it off removes the human lines already shown", n.get("@c-cleared") == 0, str(n))
    check("C: turning it off keeps the bot lines already shown", n.get("@c-bcleared") == n.get("@c-b1", 0), str(n))
    check("C: a human line after the switch is off is not shown", n.get("@c-after") == 0, str(n))
    check("C: a bot line after the switch is off is shown", n.get("@c-bafter", 0) == n.get("@c-bcleared", 0) + 1, str(n))
    check("C: a system line is still shown", n.get("@c-sys", 0) > 0, str(n))

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
