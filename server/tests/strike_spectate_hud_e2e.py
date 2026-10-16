#!/usr/bin/env python3
"""Krunker Strike's spectate state and HUD (client/src/ui/strike_hud.c view_spectate; spectate.c's
state). FRVR 1.37.2 never sets its spectate flag, so the state and the panel are ours: a dead
player taps the kill card's Spectate, the respawn enters spectating, the panel shows the target
(Free Camera, then a player after Next), Prev / Next / Play work, and Play leaves spectating.

  python3 -I server/tests/strike_spectate_hud_e2e.py BUILD_DIR      (run from the repository root)

One live offline Strike client (--flavor strike --offline, an input script): two bots, a kill of
ourselves (con kill), the kill card with its Spectate button, the tap on it, the spectate panel
(health and kill card gone), Next to a player, Play back to a live player (panel gone, health up).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). KRUNKNATIVE_ASSETS_DIR is passed through
(a worktree without the untracked asset dirs). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import os
import re
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-spectate-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def env(cfg, extra=None):
    e = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    e.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
              "XDG_STATE_HOME": cfg + "-state", "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    e.update(extra or {})
    return e


def run(args, cfg, log, extra=None, timeout=240):
    os.makedirs(cfg, exist_ok=True)
    try:
        with open(log, "w") as f:
            rc = subprocess.run(args, cwd=ROOT, env=env(cfg, extra), stdout=f, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, timeout=timeout).returncode
    except subprocess.TimeoutExpired:
        rc = -9
    with open(log, errors="replace") as f:
        return rc, f.read()


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else None


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d value=.*? text=(.*)" % re.escape(label), text)
    return (int(m.group(1)), m.group(2).strip()) if m else None


SCRIPT = """0 menu
3000 nodes @menu .mobMenuAcc--main-menu
3100 play
4000 nodes @ingame .frvr-in-game-ui__health-holder
4100 con bot 2
4500 con kill
5000 nodes @killcard .frvr-in-game-ui__kill-card-container
5050 nodes @specbtn .frvr-in-game-ui__kill-card-spectate
5100 clicksel .frvr-in-game-ui__kill-card-spectate
5300 nodes @specbtn2 .frvr-in-game-ui__kill-card-spectate
9500 nodes @spechud .frvr-in-game-ui__spectate
9550 nodes @spechealth .frvr-in-game-ui__health-holder
9600 nodes @killgone .frvr-in-game-ui__kill-card-container
9650 text @specname .frvr-in-game-ui__spectate-name
9700 clicksel .frvr-in-game-ui__spectate-next
9900 text @specnext .frvr-in-game-ui__spectate-name
10000 text @specperson .frvr-in-game-ui__spectate-person
10100 clicksel .frvr-in-game-ui__spectate-play
12500 nodes @spechud2 .frvr-in-game-ui__spectate
12550 nodes @health2 .frvr-in-game-ui__health-holder
13000 quit
"""


def main():
    script = os.path.join(WORK, "spectate.txt")
    with open(script, "w") as f:
        f.write(SCRIPT)
    rc, text = run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], os.path.join(WORK, "cfg"),
                   os.path.join(WORK, "spectate.log"), {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    check("client ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}")
    check("in a game: the in-game health holder is up", nodes(text, "ingame") == 1, str(nodes(text, "ingame")))
    check("con kill: the kill card with its Spectate button", nodes(text, "killcard") == 1 and nodes(text, "specbtn") == 1,
          f"card {nodes(text, 'killcard')} button {nodes(text, 'specbtn')}")
    check("the Spectate tap requests spectating (button hidden)", nodes(text, "specbtn2") == 0, str(nodes(text, "specbtn2")))
    check("respawn enters spectating: the spectate state is set and the panel is up",
          "[spectate] spectating" in text and nodes(text, "spechud") == 1, f"panel {nodes(text, 'spechud')}")
    check("spectating: the health holder is gone (no player of ours)", nodes(text, "spechealth") == 0, str(nodes(text, "spechealth")))
    check("spectating: the kill card is gone", nodes(text, "killgone") == 0, str(nodes(text, "killgone")))
    name = text_of(text, "specname")
    check("panel starts on Free Camera", name == (1, "Free Camera"), str(name))
    nxt = text_of(text, "specnext")
    check("Next follows a player: the name is a player's, not Free Camera", nxt is not None and nxt[1] not in ("", "Free Camera"),
          str(nxt))
    check("the person label is First Person", text_of(text, "specperson") == (1, "First Person"), str(text_of(text, "specperson")))
    check("Play leaves spectating: the spectate state is cleared", "[spectate] stopped spectating" in text, "")
    check("Play: the panel is gone", nodes(text, "spechud2") == 0, str(nodes(text, "spechud2")))
    check("Play: the health holder is back", nodes(text, "health2") == 1, str(nodes(text, "health2")))
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
