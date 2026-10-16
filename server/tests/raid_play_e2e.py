#!/usr/bin/env python3
"""The end screen's raid promo "Play Now" starts a raid (client/src/ui/hud_endscreen_v10.c, ui_handlers.c).

  python3 -I server/tests/raid_play_e2e.py BUILD_DIR      (run from the repository root)

v10's card ("Battle Bosses in Raids!", Play Now ->) opens the Raid Server Setup (openHostWindow(false, 4)),
which ends in a room the player creates. KrunkNative hosts nothing from the client, and that setup ends
at a server_bin command, so the card's Play Now used to open a command screen and start nothing. It now
calls knPlayRaid, which starts Tortuga in Raid mode (23): a Box Office raid lobby on that map when the
brain lists one, else the offline game plays it. Two checks:

  1. the raid end-screen scenario (tools/uidiff/scenarios/ingame-endscreen-collect-raid.json, raid: 1)
     dumps a card whose onclick is knPlayRaid();
  2. one headless client_bin --offline with no brain: from the menu, knPlayRaid (the card's handler)
     plays Tortuga in Raid mode (console `status`: "map Tortuga (N) | mode raid (23)") and no Raid Server
     Setup window opens.

Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
"""
import os
import re
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
WORK = tempfile.mkdtemp(prefix="kn-raid-play-e2e-", dir=os.path.expanduser("~/.cache"))
results = []

# (time ms, command). The menu, then the card's handler; the console's status and the host flow probe after.
SCRIPT = """
0 menu
1000 check @menu menuHolder=1
3000 call knPlayRaid
3500 nodes @setup .hostMap
4000 state
4000 con status
6000 quit
"""


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def env_base():
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_ASSETS_DIR": os.path.join(REPO, "assets")})
    return env


def card_markup():
    """The raid promo card of the end-screen scenario, as the captured document has it."""
    doc = os.path.join(WORK, "raid-doc.html")
    env = env_base()
    env["KRUNKNATIVE_SCENARIO_HTML"] = doc
    p = subprocess.run([f"{BUILD}/client_bin", "--render-scenario", "tools/uidiff/scenarios/ingame-endscreen-collect-raid.json",
                        os.path.join(WORK, "raid.png")], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                       timeout=240, start_new_session=True)
    if p.returncode != 0 or not os.path.exists(doc):
        check("the raid end-screen scenario renders", False, f"exit {p.returncode}: {p.stderr[-300:]}")
        return None
    with open(doc, errors="replace") as f:
        return f.read()


def run_client():
    script = os.path.join(WORK, "raidplay.script")
    with open(script, "w") as f:
        f.write(SCRIPT.strip() + "\n")
    log = os.path.join(WORK, "client.log")
    env = env_base()
    env.update({"KRUNKNATIVE_FIXED_DELTA": "16.6667", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read().splitlines(), log


def main():
    doc = card_markup()
    if doc is not None:
        m = re.search(r'<div class="crStat raidPromo"[^>]*onclick="([^"]*)"', doc)
        action = m.group(1) if m else ""
        check("the raid promo card's Play Now calls knPlayRaid()", action == "knPlayRaid()", repr(action))
        check("the card no longer opens the Raid Server Setup (openHostWindow)", "openHostWindow" not in (m.group(0) if m else ""))

    lines, log = run_client()
    check("the page loaded and the script ran", any("[input_script] call" in l and "knPlayRaid" in l for l in lines), log)
    status = [l for l in lines if "[console]" in l and "| map " in l]
    st = status[-1] if status else ""
    check("Play Now starts Tortuga in Raid mode (console status: map Tortuga, mode raid 23)",
          re.search(r"map Tortuga \(\d+\) \| mode raid \(23\)", st) is not None, st or log)
    setup = [l for l in lines if "[input_script] nodes" in l and "@setup" in l]
    check("the Raid Server Setup does not open", bool(setup) and " match=0" in setup[-1], setup[-1] if setup else "no probe")
    state = [l for l in lines if "[input_script] state" in l]
    check("the menu closed: the game runs", bool(state) and "menu=0" in state[-1], state[-1] if state else "no state")

    print(f"\n{sum(results)}/{len(results)} passed  (log: {log})")
    return all(results)


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
