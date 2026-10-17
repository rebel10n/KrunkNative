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
  run 4  a fresh config: the controls tutorial (FRVR controls_ftue) runs in the first run's game on the
         sandbox map bot_ftue_v2 (FRVR's copy, assets/maps/strike/bot_ftue_v2.json): welcome, basics, hold
         and move advance in order on their taps and move_controls is entered (the stick steps and the rest
         are not driven: the sandbox's move stick has no hit box, see ui/strike/README.md), and the flow's
         step is stored.
  art    every fetched FTUE file under ui/strike/ matches its sha256 in ui/strike/README.md (the record
         of its URL), and every image the controls markup names exists.
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
"""
import hashlib
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
# the Strike flavor's own config directory (shared/src/flavor.c flavor_storage_subdir: "krunknative-strike",
# not Box Office's "krunknative"): every html storage key of a --flavor strike client lives there
STORE = os.path.join(CFG, "krunknative-strike", "knFtue.json")
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
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_MOBILE": "1"})  # a phone: the controls tutorial and the in-game controls are the touch layer's
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


REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FTUE_ART = ["img/ftue/controls/tutor.png", "img/ftue/controls/hands_pointer.svg", "img/ftue/controls/checkmark.svg",
            "img/ftue/controls/highlight.png", "img/ftue/controls/hands_hold.svg", "img/ftue/controls/hands_walk.svg",
            "img/ftue/controls/hands_shoot.svg", "img/ftue/controls/hands_touch.svg",
            "img/ftue/controls/arrows_horizontal.svg", "img/frvr-ui/bubble/baloon.png", "img/frvr-ui/bubble/baloon-flipped.png"]


def readme_sha(rel):
    """The sha256 the README table records for ui/strike/<rel> (empty when not listed)."""
    with open(os.path.join(REPO, "ui", "strike", "README.md")) as f:
        for line in f:
            if f"| {rel} |" in line:
                cells = [c.strip() for c in line.split("|")]
                for c in cells:
                    if len(c) == 64 and all(ch in "0123456789abcdef" for ch in c):
                        return c
    return ""


def file_sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


# The controls tutorial, step by step (FRVR controls_ftue, ftue_flows). Each input waits for the step it needs
# (input_script "waitfor <selector>", a node that exists; "waitfor !<selector>" until none does): the clock alone
# cannot time a flow whose steps wait on the page, and a loaded host runs few frames.
CONTROLS_STEPS = ["welcome", "basics", "hold", "move", "move_controls", "move_interaction", "move_end", "camera",
                  "camera_controls", "camera_interaction", "camera_end", "jump", "jump_end", "reload", "complete",
                  "complete_go"]
TAP_STEPS = 6  # the tap blur passes welcome, basics, hold, move, camera, complete


def run_controls():
    """The whole tutorial on the sandbox map, driven through the page (taps), the touch sticks and the keys."""
    gated = [
        ("waitfor !.frvr-loading-holder .loading-screen", None),  # the quick connect loading screen is gone
        ("waitfor .ftue-controls__tap-text", "click 640 360"),    # welcome -> basics
        ("waitfor .ftue-controls__tap-text", "click 640 360"),    # basics -> hold
        ("waitfor .ftue-controls__tap-text", "click 640 360"),    # hold -> move
        ("waitfor .ftue-controls__tap-text", "click 640 360"),    # move -> move_controls
        ("waitfor .ftue-controls__touch-hand-vertical", None),    # move_controls: the move stick
        ("cursor 200 470", None), ("mdown left", None), ("cursor 190 420", None), ("cursor 180 400", None),
        ("mup left", None),                                       # -> move_interaction -> move_end (timed)
        ("waitfor .ftue-controls__tap-text", "click 640 360"),    # camera (after move_end's timer) -> camera_controls
        ("waitfor .ftue-controls__arrows-horizontal", None),      # camera_controls: the look stick
        ("cursor 1000 400", None), ("mdown left", None), ("cursor 1040 400", None), ("cursor 1060 400", None),
        ("mup left", None),                                       # -> camera_interaction -> camera_end (timed)
        ("waitfor .ftue-controls__right .ripples", None),         # jump (after camera_end's timer)
        ("down SPACE", None), ("up SPACE", None),                 # -> jump_end (timed)
        ("waitfor .ftue-controls__highlight-reload-animation", None),  # reload (after jump_end's timer)
        ("down R", None), ("up R", None),                         # -> complete
        ("waitfor .ftue-controls__tap-text", "click 640 360"),    # complete -> complete_go
        ("waitfor .ftue-controls__button", None),                 # "Let's Go!"
        ("clicksel .ftue-controls__button", None),                # the flow ends, the game is left
        ("waitfor !.ftue-controls__root", None),                  # the overlay is gone (complete_go's last step)
    ]
    lines = ["0 menu"]
    t = 200
    for cmd, follow in gated:
        t += 200
        lines.append(f"{t} {cmd}")
        if follow:
            t += 200
            lines.append(f"{t} {follow}")
    lines.append(f"{t + 500} quit")
    return lines


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
    # the controls tutorial (run 4): a fresh config, the first run's quick connect starts the flow in the sandbox
    shutil.rmtree(CFG, ignore_errors=True)
    os.makedirs(CFG, exist_ok=True)
    text, nodes = run("run4", run_controls())
    check("controls: the sandbox map is loaded for the first run (FRVR bot_ftue_v2)",
          "[strike] ftue: controls sandbox bot_ftue_v2" in text)
    steps = re.findall(r"\[strike\] ftue: controls step (\w+)", text)
    check("controls: every live step runs in FRVR's order (enemy steps are skipped: no FTUE bots)",
          steps == CONTROLS_STEPS, str(steps))
    check("controls: the taps go through the tap blur (strikeAction ftue-tap)",
          text.count("[strike] action ftue-tap") >= TAP_STEPS, f"taps={text.count('[strike] action ftue-tap')}")
    check("controls: \"Let's Go!\" ends the flow (strikeAction ftue-go, the flow's end)",
          "[strike] action ftue-go" in text and "[strike] ftue: controls done" in text)
    check("controls: no wait timed out", "waitfor timeout" not in text)
    flow = ((stored() or {}).get("guest") or {}).get("flows", {}).get("controls_ftue")
    check("controls: the finished flow is stored as done in knFtue.json", flow == "done", f"stored={flow}")

    # the sandbox map is FRVR's copy (the Strike catalogue), and the art is the fetched file
    mp = os.path.join(REPO, "assets", "maps", "strike", "bot_ftue_v2.json")
    check("the sandbox map is vendored (assets/maps/strike/bot_ftue_v2.json)", os.path.isfile(mp))
    if os.path.isfile(mp):
        check("the sandbox map sha256 matches the FRVR 1.37.2 copy (README, 4a8f6ed8...)",
              file_sha(mp) == "4a8f6ed85d420f27686cf300c3ae3b026e169fc13cabe5acb9dd4d2a4e55740a", file_sha(mp))

    missing, bad = [], []
    for rel in FTUE_ART:
        path = os.path.join(REPO, "ui", "strike", rel)
        if not os.path.isfile(path):
            missing.append(rel)
            continue
        want = readme_sha(rel)
        if not want or file_sha(path) != want:
            bad.append(f"{rel} ({'not in README' if not want else 'sha mismatch'})")
    check("every FTUE art file is vendored", not missing, ", ".join(missing))
    check("every FTUE art file matches its README sha256", not bad, ", ".join(bad))
    return all(results)


ok = main()
if ok:
    shutil.rmtree(WORK, ignore_errors=True)
print(f"\n{sum(results)}/{len(results)} passed" + ("" if ok else f"  (logs: {WORK})"))
sys.exit(0 if ok else 1)
