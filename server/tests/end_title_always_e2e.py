#!/usr/bin/env python3
"""The round-end result title always plays its animation (client/src/ui/hud_ui.c hud_end_step,
client/src/net/game_events.c events_end).

  python3 -I server/tests/end_title_always_e2e.py BUILD_DIR      (run from the repository root)

Two headless client_bin --sandbox runs fixed-step at 1000/60 ms per frame (KRUNKNATIVE_FIXED_DELTA), so
the script clock is exact. Both are defeats: our score goes to -100 and a bot (stopped, so it does not kill
us) wins the round.
  dead:  the local player is killed (`con kill`), the death menu comes up, then the round ends (`con end`).
         A dead player at round end gets the result title (#knVicTitle, the defeat image) and its animation
         over the open death menu, at the END's first frame, 100 ms and 300 ms later (the frames are
         written to KN_E2E_KEEP when set). The end screen itself (#endUI, the results) stays down.
  alive: the same round end for a playing player (the control: the end screen and the title).
The sandbox's round end comes 3 s after `con end` (END_DELAY); the probes sit a frame after that.
Frames are written to WORK (kept when KN_E2E_KEEP=<dir> names a directory, copied there).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import glob
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-end-title-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 1000.0 / 60.0
TITLE_IDS = "knVicTitle knVicTitle_vic knVicTitle_def knVicTitle_dra"
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def vis_line(lines, label):
    """the `vis` log line of @label: {id: value}"""
    for line in lines:
        m = re.search(r"\[input_script\] vis t=\d+ @" + label + r"(?: |$)(.*)", line)
        if m:
            out = {}
            for tok in m.group(1).split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    out[k] = v
            return out
    return {}


def run(name, script_lines):
    """one client run; returns its log lines"""
    d = os.path.join(WORK, name)
    cfg = os.path.join(d, "cfg", "krunknative")
    os.makedirs(cfg)
    script = os.path.join(d, "end.script")
    with open(script, "w") as f:
        f.write("\n".join(script_lines) + "\n")
    log = os.path.join(d, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(d, "cfg"), "KRUNKNATIVE_NO_BROWSER": "1",
                "XDG_STATE_HOME": os.path.join(d, "state"), "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": f"{FRAME_MS:.4f}", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--sandbox"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read().splitlines(), d


# The sandbox's /end ends the round 3 s after it (game time, so the frame clock): the END arrives on the
# frame where con end + 3000 falls, the probes sit 2 frames later (the END frame's own is the first one).
END_DELAY = 3000
FRAME_SLACK = 33


def defeat_setup():
    """a defeat: our score goes below the bot's 0 (`score` adds to ours). bot_stop first, so the bot that
    spawns is stopped from the start (it does not kill us: the alive run stays alive). One console command
    every 500 ms or more: the sandbox drops a command typed right after another."""
    return ["500 con score -100", "1000 con bot_stop", "1500 con bot"]


def end_probes(d, end_at):
    """the captures of the round end whose `con end` ran at `end_at` (script ms)"""
    e = end_at + END_DELAY + FRAME_SLACK
    return [
        f"{e} vis @e0 {TITLE_IDS} endUI endInfo menuHolder",
        f"{e} frames {d}/e0 1",
        f"{e + 100} vis @e100 {TITLE_IDS} endUI endInfo menuHolder",
        f"{e + 100} frames {d}/e100 1",
        f"{e + 300} vis @e300 {TITLE_IDS} endUI endInfo menuHolder",
        f"{e + 300} frames {d}/e300 1",
        f"{e + 1500} state",
        f"{e + 1600} quit",
    ]


def dead_script(d):
    # the self-kill (`kill`): the death card, then the death menu after the respawn delay
    return ["0 play"] + defeat_setup() + ["3500 con kill", "5000 state", "5000 vis @dead endUI menuHolder deathUIHolder " + TITLE_IDS,
                                         "5500 con end"] + end_probes(d, 5500)


def alive_script(d):
    return ["0 play"] + defeat_setup() + ["3500 con end"] + end_probes(d, 3500)


def title_shown(v):
    """the result title's layer and its image: shown when the layer is and one result image is"""
    return v.get("knVicTitle") == "1" and any(v.get(k) == "1" for k in ("knVicTitle_vic", "knVicTitle_def", "knVicTitle_dra"))


def check_run(name, lines, d):
    for label, what in (("e0", "the END's first frame"), ("e100", "100 ms after the END"), ("e300", "300 ms after the END")):
        v = vis_line(lines, label)
        check(f"{name}: the result title shows {what}", title_shown(v), f"{v}")
    v = vis_line(lines, "e300")
    if name == "dead":
        check("dead: no end screen (#endUI and the results stay gated)", v.get("endUI") == "0" and v.get("endInfo") == "0",
              f"endUI={v.get('endUI')} endInfo={v.get('endInfo')}")
        check("dead: the title plays over the open death menu (#menuHolder up)", vis_line(lines, "e100").get("menuHolder") == "1",
              f"menuHolder={vis_line(lines, 'e100').get('menuHolder')}")
    frames_ok = all(glob.glob(os.path.join(d, p + "_*.png")) or os.path.exists(os.path.join(d, p + ".png"))
                    for p in ("e0", "e100", "e300"))
    check(f"{name}: frames written", frames_ok, d)
    check(f"{name}: a defeat (the round's winner is the bot): the defeat image", vis_line(lines, "e100").get("knVicTitle_def") == "1",
          f"{vis_line(lines, 'e100')}")


def main():
    keep = os.environ.get("KN_E2E_KEEP")
    dead_lines, dead_dir = run("dead", dead_script(os.path.join(WORK, "dead")))
    check_run("dead", dead_lines, dead_dir)
    alive_lines, alive_dir = run("alive", alive_script(os.path.join(WORK, "alive")))
    check_run("alive", alive_lines, alive_dir)
    if keep:
        os.makedirs(keep, exist_ok=True)
        for sub in ("dead", "alive"):
            for p in glob.glob(os.path.join(WORK, sub, "e*.png")):
                shutil.copy(p, os.path.join(keep, f"{sub}-{os.path.basename(p)}"))
    if all(results) and results:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs and frames kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()
