#!/usr/bin/env python3
"""The developer console's `autofill` (shared/src/chat_commands.c cmd_autofill, doc/console.md "Bots").

  python3 -I client/tests/console_autofill_e2e.py BUILD_DIR      (run from the repository root)

The bug: `autofill` was only a chat command ("/autofill"). The developer console did not list it, so
`autofill off` typed there was dropped without a word ("unknown commands are silent"), and `help
autofill` answered "unknown command: autofill". A player who wanted the lobby's bots gone had only
bot_kick, which the quota fill refills at once.

An offline Kanji client (the sandbox: its own machine, so the local player is a DEV peer and may run
every bot command) runs a headless input script; the client log carries the console's lines
("[console] ..."), so the checks read them:
  1. `help autofill` lists the command with its usage;
  2. `autofill off` typed in the console reaches the server: the sandbox answers "Autofill OFF: 0 bots
     left." (no quota bots exist offline, so none leave);
  3. `autofill on` answers "Autofill ON: ..." (the lobby fills again).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-autofill-", dir=CACHE if os.path.isdir(CACHE) else None)
SCRIPT = (
    "0 play\n"
    "200 con help autofill\n"
    "300 con autofill off\n"
    "500 con autofill on\n"
    "800 quit\n"
)
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(timeout=180):
    script = os.path.join(WORK, "autofill.script")
    with open(script, "w") as f:
        f.write(SCRIPT)
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read()


def main():
    text = run()
    check("client_bin ran the script", "[input_script] " in text and "events from" in text, "")
    check("1. help autofill lists the command in the console", "[console] autofill on|off|players" in text,
          "unknown command: autofill" if "unknown command: autofill" in text else "")
    check("2. autofill off typed in the console reaches the server (sandbox reply)",
          "[console] Server: Autofill OFF" in text and "[cmd] Guest_1: /autofill off" in text, "")
    check("3. autofill on typed in the console reaches the server (sandbox reply)",
          "[console] Server: Autofill ON" in text and "[cmd] Guest_1: /autofill on" in text, "")


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
finally:
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}", flush=True)
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)
