"""The map card (#loadingBg, client/src/ui/v10_loading.c) against v10's page loads.
  python3 -I client/tests/loading_switch_e2e.py BUILD_DIR      (from the repository root)
v10 shows the card ("UP NEXT", map, mode) only on a map load that is not the page's first: a map change inside the
same game (deob-10.0.5.js:224307-224331). Every join, host or play is a fresh page there (the boot curtain and its
ring). Krunker Box Office switches games in place, so it checks:
  - launch: no card;
  - Host Game -> play offline on another map while the local game runs (community_host_slot -> menu_host_switch):
    no card, and the game arrives on the new map (the curtain does not stay up);
  - a map change inside the running game (the server's `map`): the card.
Exit code 0 when every check passed; the input-script logs are kept in ~/.cache/kn-loading-switch-e2e-* on failure.
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
WORK = tempfile.mkdtemp(prefix="kn-loading-switch-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def run_native(name, script, timeout=240):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg-" + name),
                "XDG_STATE_HOME": os.path.join(WORK, "state-" + name), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "33",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_DEBUG": "2"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
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


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ " + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


# launch: the boot curtain, never the card
log = run_native("launch", """
0 menu
300 vis @launch loadingBg
600 quit
""")
check(vis(log, "@launch").get("loadingBg") == "0", f"launch: no map card ({vis(log, '@launch')})")

# Host Game -> play offline on Sandstorm while the local game (Kanji) runs: a fresh page in v10, so no card
log = run_native("host", """
0 play
1500 call openHostWindow 0 0
1700 clickid gameMap2
1800 call knHostFlow "practice"
1850 vis @after loadingBg
4000 vis @later loadingBg
4100 quit
""")
check("knHostFlow('practice')" in log, "host: Host Game's play offline ran")
check(re.search(r"offline: playing map \d+", log) is not None, "host: the local game reloaded on the picked map")
check(vis(log, "@after").get("loadingBg") == "0", f"host: no map card after the switch ({vis(log, '@after')})")
check(vis(log, "@later").get("loadingBg") == "0", f"host: still no map card later ({vis(log, '@later')})")
check(len(re.findall(r"cam t=\d+ @spawn0 spawns=2", log)) >= 1, "host: the player spawned in the new game (the curtain lifted)")

# a map change inside the running game: v10's card
log = run_native("ingame", """
0 play
700 con map Sandstorm
760 vis @card loadingBg
1200 quit
""")
check(vis(log, "@card").get("loadingBg") == "1", f"in-game map change: the map card ({vis(log, '@card')})")

if failures:
    print(f"loading switch e2e: {len(failures)} failure(s); logs in {WORK}")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("loading switch e2e: all checks passed")
