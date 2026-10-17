#!/usr/bin/env python3
"""Krunker Strike's round end (LEAN_END_SCREEN, client/src/ui/strike_hud.c end_fill) from live data.

  python3 -I server/tests/strike_roundend_e2e.py BUILD_DIR      (run from the repository root)

FRVR's WB (the lean screen) fills from the account and the END packet, not from the template's sample
numbers: the header's title (_Ae: "#1 VICTORY" / "2nd PLACE" with gold / silver / bronze, team modes
VICTORY / DEFEAT / DRAW), the level and XP bar (strike_econ's account), the KR the server sent
(player.reward), the next level's reward (the track) and the end timer ("Next round in").

  1. --render-scenario of tools/uidiff/scenarios/strike/end.json (endReward 25, endSeconds 30): the
     title "#1 VICTORY", level 1 with 0 / 100 (a guest's XP is 0), the 25 KR card, the timer 00:30.
  2. the same screen with endRank 2 / 3 / 12 (scenario keys): "2nd PLACE", "3rd PLACE", "12th PLACE"
     and no reward card when player.reward is 0.
  3. a live offline Strike client (--flavor strike --offline, an input script): after `bot 2` + `end`
     the lean screen's title, the bar's text (a guest: Level 1, 0 / 100), the next level reward card,
     the timer's MM:SS (ticking), and no sample numbers (no "Level 12", "100 / 400", "350 XP", Claim).
  4. the round end's Settings (Home -> MAIN_MENU -> Settings): Settings stays up while the end timer
     packets come in (they only update the lean screen's values, FRVR's endTimer; they never push the
     round end again), and closing it (Back) lands on the menu, since the round is over. Escape from
     the menu over the round end keeps the round end up (the game view never shows with it gone).

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Prints PASS/FAIL lines; exit 0 only when all
passed.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-roundend-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
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
        rc = "timeout"
    with open(log, errors="replace") as f:
        return rc, f.read()


def png_size(path):
    try:
        with open(path, "rb") as f:
            head = f.read(24)
        return struct.unpack(">II", head[16:24]) if head[:8] == b"\x89PNG\r\n\x1a\n" else None
    except OSError:
        return None


def scenario_json(name, strike):
    """a render-scenario fixture: the end.json state with the given state.strike keys"""
    import json
    with open(os.path.join(ROOT, "tools", "uidiff", "scenarios", "strike", "end.json"), encoding="utf-8") as f:
        fixture = json.load(f)
    fixture["state"]["strike"] = strike
    path = os.path.join(WORK, name + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(fixture, f)
    return path


def end_line(text):
    m = re.search(r'\[strike\] end screen: title "([^"]*)" level (\d+) \((\d+) / (\d+)\), reward (\d+) KR', text)
    return m.groups() if m else None


# ---------------------------------------------------------------- 1. the scenarios

def scenario_case():
    out = os.path.join(WORK, "end.png")
    rc, text = run([f"{BUILD}/client_bin", "--render-scenario", "tools/uidiff/scenarios/strike/end.json", out],
                   os.path.join(WORK, "cfg-end"), os.path.join(WORK, "end.log"))
    check("1: end.json rendered 1280x720", rc == 0 and png_size(out) == (1280, 720), f"rc {rc}, {png_size(out)}")
    check("1: title #1 VICTORY, a guest's level 1 at 0 / 100, the 25 KR reward",
          end_line(text) == ("#1 VICTORY", "1", "0", "100", "25"), str(end_line(text)))

    for rank, title in ((2, "2nd PLACE"), (3, "3rd PLACE"), (12, "12th PLACE")):
        path = scenario_json(f"rank{rank}", {"end": 1, "endRank": rank, "endReward": 0, "endSeconds": 12})
        rc, text = run([f"{BUILD}/client_bin", "--render-scenario", path, os.path.join(WORK, f"rank{rank}.png")],
                       os.path.join(WORK, f"cfg-rank{rank}"), os.path.join(WORK, f"rank{rank}.log"))
        got = end_line(text)
        check(f"2: endRank {rank} -> {title}, no reward card at 0 KR", rc == 0 and got == (title, "1", "0", "100", "0"), str(got))


# ---------------------------------------------------------------- 3. the live client

LIVE_SCRIPT = """menu
1500 nodes @page #frvrMenuMobile
4000 con bot 2
4500 con end
11000 text @title .lean-end-screen__victory-defeat .frvr-in-game-ui__victory-text-holder
11050 text @bar .progress-bar__left-text
11100 text @xp .progress-bar__right-text
11150 nodes @claim .lean-end-screen__reward .frvr-button
11200 text @next .lean-end-screen__reward-text
11250 text @timer1 .lean-end-screen__timer__time
11300 text @sample .lean-end-screen__main-container
13300 text @timer2 .lean-end-screen__timer__time
13350 frames {shots}/round-end 1
13400 quit
"""


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d value=.*? text=(.*)" % re.escape(label), text)
    return (int(m.group(1)), m.group(2).strip()) if m else (-1, "")


def live_case():
    script = os.path.join(WORK, "live.txt")
    with open(script, "w") as f:
        f.write(LIVE_SCRIPT.format(shots=WORK))
    rc, text = run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], os.path.join(WORK, "cfg-live"),
                   os.path.join(WORK, "live.log"), {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    check("3: live client ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}")
    title = text_of(text, "title")
    # the holder's text: "#N VICTORY" (first) or "Nth PLACE" (the bot round's FFA board)
    check("3: the title is \"#N VICTORY\" or \"Nth PLACE\"", re.match(r"^(#\d+ VICTORY|\d+(st|nd|rd|th) PLACE)$", title[1]) is not None, str(title))
    check("3: the bar shows the guest's level (1) and XP (0 / 100), not the sample's Level 12",
          text_of(text, "bar")[1].startswith("Level 1") and text_of(text, "xp") == (1, "0 / 100"),
          f"{text_of(text, 'bar')} {text_of(text, 'xp')}")
    check("3: no Claim button: FRVR shows it only after a level-up this match", nodes(text, "claim") == 0, str(nodes(text, "claim")))
    check("3: the next level's reward card (\"Next level reward\")", text_of(text, "next")[1].startswith("Next level reward"), str(text_of(text, "next")))
    check("3: the sample's numbers are gone (no 100 / 400, 350 XP, 25 KR from the template)",
          "100 / 400" not in text_of(text, "sample")[1] and "350 XP" not in text_of(text, "sample")[1], text_of(text, "sample")[1][:120])
    t1, t2 = text_of(text, "timer1")[1], text_of(text, "timer2")[1]
    check("3: the end timer reads MM:SS and counts down", re.match(r"^\d\d:\d\d$", t1) is not None and re.match(r"^\d\d:\d\d$", t2) is not None and t1 != t2,
          f"{t1} {t2}")
    check("3: the round end's screenshot", png_size(os.path.join(WORK, "round-end_0000.png")) == (1280, 720))


# ---------------------------------------------------------------- 4. Settings over the round end

SETTINGS_SCRIPT = """menu
1500 nodes @page #frvrMenuMobile
4000 con bot 2
4500 con end
24000 nodes @lean-up .lean-end-screen__header
24200 clicksel .frvr-button-home
24700 nodes @menu-home .frvr-main-menu--top-right
24900 clickid mobMenuSett
25100 nodes @settings-open .frvr-settings-screen
27500 nodes @settings-later .frvr-settings-screen
27500 nodes @lean-later .lean-end-screen__header
27700 clicksel .frvr-button-back
27900 nodes @settings-closed .frvr-settings-screen
27900 nodes @menu-back .frvr-main-menu--top-right
27900 nodes @lean-closed .lean-end-screen__header
28100 quit
"""

# Escape over a round end: the menu closes (Krunker's pressButton(27) ends the menu), the round end
# stays up on the game view, and it is still up after the end timer's packets.
ESCAPE_SCRIPT = """menu
1500 nodes @page #frvrMenuMobile
4000 con bot 2
4500 con end
24000 nodes @lean-up .lean-end-screen__header
24200 clicksel .frvr-button-home
24700 nodes @menu-home .frvr-main-menu--top-right
24900 down ESC
24950 up ESC
25100 state
25100 nodes @lean-esc .lean-end-screen__header
27500 state
27500 nodes @lean-later .lean-end-screen__header
27700 quit
"""


def escape_case():
    script = os.path.join(WORK, "escape.txt")
    with open(script, "w") as f:
        f.write(ESCAPE_SCRIPT)
    rc, text = run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], os.path.join(WORK, "cfg-escape"),
                   os.path.join(WORK, "escape.log"), {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    check("4: Escape run ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}")
    check("4: the round end is up before Escape", nodes(text, "lean-up") == 1, str(nodes(text, "lean-up")))
    check("4: Escape over the round end closes the menu (the game view is up)", re.search(r"state t=\d+ .*menu=0", text) is not None,
          "no menu=0 state" if re.search(r"state t=\d+ .*menu=0", text) is None else "")
    check("4: the round end is still up after Escape (its screen is not cleared with the menu)",
          nodes(text, "lean-esc") == 1, str(nodes(text, "lean-esc")))
    check("4: and after the end timer's packets", nodes(text, "lean-later") == 1, str(nodes(text, "lean-later")))


def settings_case():
    escape_case()
    script = os.path.join(WORK, "settings.txt")
    with open(script, "w") as f:
        f.write(SETTINGS_SCRIPT)
    rc, text = run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], os.path.join(WORK, "cfg-settings"),
                   os.path.join(WORK, "settings.log"), {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    check("4: live client ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}")
    check("4: the round end is up before Home", nodes(text, "lean-up") == 1, str(nodes(text, "lean-up")))
    check("4: Home takes the player to the main menu (the round end goes with it)", nodes(text, "menu-home") >= 1, str(nodes(text, "menu-home")))
    check("4: Settings opens over the main menu", nodes(text, "settings-open") == 1, str(nodes(text, "settings-open")))
    check("4: Settings stays up after the end timer's packets (no round end pushed over it)",
          nodes(text, "settings-later") == 1, f"settings {nodes(text, 'settings-later')}, lean end {nodes(text, 'lean-later')}")
    check("4: the round end is not pushed again while Settings is up", nodes(text, "lean-later") == 0, str(nodes(text, "lean-later")))
    check("4: the round end screen was pushed once this round", text.count("[strike] screen LEAN_END_SCREEN") == 1,
          str(text.count("[strike] screen LEAN_END_SCREEN")))
    check("4: Back closes Settings and lands on the main menu (the round is over)", nodes(text, "settings-closed") == 0 and nodes(text, "menu-back") >= 1,
          f"settings {nodes(text, 'settings-closed')}, menu {nodes(text, 'menu-back')}")
    check("4: no round end after closing Settings", nodes(text, "lean-closed") == 0, str(nodes(text, "lean-closed")))


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin", file=sys.stderr)
        return 2
    scenario_case()
    live_case()
    settings_case()
    ok = all(results)
    print(f"\n{sum(results)}/{len(results)} passed" + ("" if ok else f"  (logs in {WORK})"))
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
