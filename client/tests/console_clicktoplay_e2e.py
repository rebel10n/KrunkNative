#!/usr/bin/env python3
"""The developer console stays usable after Click to Play (client/src/main.c mouse_button_callback
and char_callback, client/src/ui/menu_ui.c client_pointer_update / menu_lock_ready).

  python3 -I client/tests/console_clicktoplay_e2e.py BUILD_DIR      (run from the repository root)

The bug: "when i have the console open and press click to play im left in a state where i cant
click on anything until i close the dev console (cant even interact with the console itself)."
Opening the console (`) opens the menu and frees the pointer. Click to Play then closes the menu
while the console stays on screen. From there the page no longer had the mouse (html_pointer_input
looked at the menu only), so a click went to the game branch, where ui_controls_mouse_event swallows
a press while the console is open: the console got no clicks. Typed characters were dropped too
(char_callback forwards them only with the menu open).

An offline Kanji client (headless, input script, fixed 60 fps step, 1280x720, a fresh config so the
console sits at its default place) does:
  1. ` opens the console (the menu is up behind it), then Click to Play (#instructions): the game
     starts (the menu closes, the pointer stays free for the console) and the console is still shown;
  2. a click on the console's input line focuses it, the typed text reaches the input and Enter
     runs it (the input empties);
  3. a click on the console's close button (titlebar x) hides the console.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-clicktoplay-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def script_text():
    # '#' starts a comment in input scripts, so the selectors below use no '#'
    return "\n".join([
        "0 menu",
        "200 down GRAVE", "220 up GRAVE",          # the console opens; the menu is up behind it
        "300 console @open",
        "400 clickid instructions",                # Click to Play with the console open
        "500 console @after_play",
        "510 state @after_play",
        "600 clicksel .inputShell input",          # a click on the console's input line
        "620 type echo clicked",                   # typed through the char callback
        "640 conin @typed",
        "660 down ENTER", "680 up ENTER",          # runs the typed line: the input empties
        "700 conin @sent",
        "800 clicksel .modal .titlebar .x",        # a click on the console's close button
        "820 console @closed",
        "900 quit",
    ]) + "\n"


def run(timeout=240):
    script = os.path.join(WORK, "clicktoplay.script")
    with open(script, "w") as f:
        f.write(script_text())
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS), "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check("client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def last_line(text, pattern):
    m = None
    for m in re.finditer(pattern, text, re.M):
        pass
    return m


def main():
    if not os.path.exists(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin")
        return 1
    text = run()

    m = last_line(text, r"^\[input_script\] console t=\d+ @open (.*)$")
    check("console opened with `", m and "open=1" in m.group(1) and "shown=1" in m.group(1),
          m.group(1) if m else "no probe")

    m = last_line(text, r"^\[input_script\] console t=\d+ @after_play (.*)$")
    check("console still shown after Click to Play", m and "open=1" in m.group(1) and "shown=1" in m.group(1),
          m.group(1) if m else "no probe")

    m = last_line(text, r"^\[input_script\] state t=\d+ .*menu=(\d)")
    check("Click to Play started the game (menu closed)", m and m.group(1) == "0", m.group(0) if m else "no state")

    m = last_line(text, r"^\[input_script\] state t=\d+ .*cursor=(\w+)")
    check("the pointer stays free over the console (cursor=normal)", m and m.group(1) == "normal",
          m.group(0) if m else "no state")

    typed = last_line(text, r"^\[input_script\] conin t=\d+ @typed value=\[(.*?)\] hints=")
    check("a click on the console input focuses it and the typed text lands",
          typed and typed.group(1) == "echo clicked", f"value=[{typed.group(1)}]" if typed else "no probe")

    sent = last_line(text, r"^\[input_script\] conin t=\d+ @sent value=\[(.*?)\] hints=")
    check("Enter runs the typed line (the input empties)",
          typed and typed.group(1) == "echo clicked" and sent and sent.group(1) == "",
          f"value=[{sent.group(1)}]" if sent else "no probe")

    m = last_line(text, r"^\[input_script\] console t=\d+ @closed (.*)$")
    check("a click on the console's close button hides it", m and "open=0" in m.group(1),
          m.group(1) if m else "no probe")

    failed = len(results) - sum(results)
    print(f"{len(results) - failed}/{len(results)} passed" + (f", log {os.path.join(WORK, 'client.log')}" if failed else ""))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
