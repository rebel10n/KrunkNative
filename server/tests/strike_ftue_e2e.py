#!/usr/bin/env python3
"""Krunker Strike's first-run gate (FRVR hasAlreadyPlayedBefore, the FTUE storage) end to end.

  python3 -I server/tests/strike_ftue_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin --flavor strike --offline (Kanji), one private config shared by the runs:
  run 1  a fresh config: the boot gate sends the first run to quickConnect (the main menu never
         shows, the loading screen goes up and the game starts: the in-game controls exist), the
         log says so, and the account's "played" flag is stored in knFtue.json (the local entry
         that holds the FTUE state; the brain has no first-run route).
  run 2  the same config (played): the main menu is up at boot, no quick connect.
  run 3  the same config, but the flag cleared by the test: the first run again (the flag is
         written back at once).
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
WORK = tempfile.mkdtemp(prefix="kn-strike-ftue-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
STORE = os.path.join(CFG, "krunknative", "knFtue.json")
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script_lines):
    script = os.path.join(WORK, f"{name}.script")
    with open(script, "w") as f:
        f.write("\n".join(script_lines) + "\n")
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": CFG,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
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
    nodes = {}
    for m in re.finditer(r"\[input_script\] nodes t=\d+ @(\S+) elements=\d+ match=(\d+)", text):
        nodes[m.group(1)] = int(m.group(2))
    return text, nodes


def stored():
    try:
        with open(STORE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def clear_played():
    doc = stored() or {}
    for entry in doc.values():
        if isinstance(entry, dict):
            entry.pop("played", None)
    with open(STORE, "w") as f:
        json.dump(doc, f)


GATE_LINE = "[strike] ftue: first run for guest, quick connect"


def run1():
    return ["0 menu",
            "3000 nodes @main .mobMenuAcc--main-menu",
            "12000 nodes @ingame .control-buttons",
            "14000 quit"]


def main():
    text, nodes = run("run1", run1())
    check("the Strike page loaded", "[strike] Krunker Strike page ready" in text)
    check("a fresh config: the first run goes to quickConnect, the main menu is not up at boot",
          nodes.get("main") == 0 and GATE_LINE in text, f"main={nodes.get('main')}")
    check("the first run starts the game (the in-game controls exist)", nodes.get("ingame", 0) >= 1,
          f"ingame={nodes.get('ingame')}")
    doc = stored()
    check("the first run stores the account's played flag in knFtue.json",
          isinstance(doc, dict) and isinstance(doc.get("guest"), dict) and doc["guest"].get("played") is True, str(doc))

    text, nodes = run("run2", ["0 menu", "3000 nodes @main .mobMenuAcc--main-menu", "4000 quit"])
    check("a played config: the main menu is up at boot", nodes.get("main") == 1, f"main={nodes.get('main')}")
    check("a played config: no quick connect", GATE_LINE not in text)

    clear_played()
    text, nodes = run("run3", ["0 menu", "3000 nodes @main .mobMenuAcc--main-menu", "14000 quit"])
    check("the flag cleared: the first run again (quick connect, no main menu)",
          nodes.get("main") == 0 and GATE_LINE in text, f"main={nodes.get('main')}")
    check("the flag is written back", (stored() or {}).get("guest", {}).get("played") is True)
    return all(results)


ok = main()
if ok:
    shutil.rmtree(WORK, ignore_errors=True)
print(f"\n{sum(results)}/{len(results)} passed" + ("" if ok else f"  (logs: {WORK})"))
sys.exit(0 if ok else 1)
