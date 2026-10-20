#!/usr/bin/env python3
"""A match's end must not leave the client on a blank screen with the console open (client/src/ui/hud_ui.c
hud_ui_hide_end_screen, pointer_lock.c: play needs no console).

  python3 -I server/tests/end_respawn_e2e.py BUILD_DIR      (run from the repository root)

One node (ffa on Burg), one client. The client opens the developer console (`), the round ends (`con end`),
and the map changes (`con map 1`): a map change does not respawn the players who were out at the end, so
the round restarts with the client out of the game and the console open over it.

  E1: the end card drops at the restart and the spawn prompt is up (menuHolder shown, the menu open), not an
      empty world with no HUD and no prompt
  E2: the prompt still lets the player in: Esc closes the console and plays (the in-game HUD comes up)

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 when the database is unavailable.
"""
import json
import os
import re
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
_argv = sys.argv
sys.argv = [_argv[0], _argv[1] if len(_argv) > 1 else "cmake-build-debug"]
import reload_rejoin_e2e as R  # noqa: E402
sys.argv = _argv

BUILD = R.BUILD
ROOT = R.ROOT
VIS = "@hud endUI inGameUI gameUI chatHolder ammoHolder uiBase devConsoleOverlay menuHolder"
VIS_RE = re.compile(r"\[input_script\] vis t=(\d+) @hud (.*)")
STATE_RE = re.compile(r"\[input_script\] state t=(\d+) .*? menu=(\d)")
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def fields(text):
    return dict(kv.split("=", 1) for kv in text.split() if "=" in kv)


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("end_respawn_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", R.DRIVER, R.NAME], capture_output=True)
    if r.returncode:
        print("end_respawn_e2e: %s unavailable: skipped" % R.DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=R.SECRET, **dbenv)
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port = R.start_brain(env, os.path.join(R.WORK, "brain.log"))
        check("kn_brain starts", port, R.read_log(os.path.join(R.WORK, "brain.log"))[-300:])
        if not port:
            return 1
        _, game = R.start_node(port, "RLE", R.PORT_A, "ffa", "Burg")
        check("the lobby is up", game, "")
        if not game:
            return 1
        # the console opens (`) before the end; the end card runs; the map changes at 20 s; probes from 8 s on
        lines = ["2000 down GRAVE", "2100 up GRAVE", "3000 con end", "20000 con map 1"]
        lines += ["%d state" % t + "\n%d vis %s" % (t + 1, VIS) for t in range(8000, 46000, 2000)]
        lines += ["35000 down ESC", "35100 up ESC", "46000 quit"]
        lines.sort(key=lambda l: int(l.split()[0]))
        script = "0 play\n" + "\n".join(lines) + "\n"
        p, log = R.client("endrespawn", port, script, "0 quit\n", game[4]["mode"])
        R.finish(p, 150)
        text = R.read_log(log)
        vis = [(int(m.group(1)), fields(m.group(2))) for m in VIS_RE.finditer(text)]
        ended = [i for i, (_, f) in enumerate(vis) if f.get("endUI") == "1"]
        check("the round ended (the end card showed)", bool(ended), str(vis[:2])[:200])
        drop = None
        if ended:
            drop = next((i for i in range(ended[0], len(vis)) if vis[i][1].get("endUI") == "0"), None)
        check("the end card dropped at the restart", drop is not None, str([f.get("endUI") for _, f in vis][-6:]))
        if drop is not None:
            _, f = vis[drop]
            ok = f.get("menuHolder") == "1" or f.get("inGameUI") == "1"
            check("E1: the spawn prompt is up after the restart (no blank screen with the console open)", ok,
                  "endUI=%s inGameUI=%s menuHolder=%s devConsoleOverlay=%s" % (f.get("endUI"), f.get("inGameUI"),
                                                                             f.get("menuHolder"), f.get("devConsoleOverlay")))
        # E2 needs an Esc (the test's key) to close the console and play: the prompt's own path
        # (ESC closes the console with the menu open: menu_play)
        after = [f for t, f in vis if t > 40000]
        check("E2: Esc with the prompt up plays (the HUD comes up)", bool(after) and after[-1].get("inGameUI") == "1",
              str(after[-1:]))
    finally:
        for q in R.procs:
            if q.poll() is None:
                try:
                    os.killpg(q.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        subprocess.run(scratch + ["drop", R.DRIVER, R.NAME], capture_output=True)
    if all(results):
        import shutil
        shutil.rmtree(R.WORK, ignore_errors=True)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
