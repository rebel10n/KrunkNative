#!/usr/bin/env python3
"""Krunker Strike's transcribed screens end to end (client/src/ui/strike_meta.c; ui/strike/screens/
MISSION_TAB_SCREEN, REWARD_TO_CLAIM, MATCH_DETAILS_SCREEN, PROGRESSION_TRACK.html). No FRVR capture
exists for these views, so this checks that each one opens with FRVR's structure and the right
data, not its pixels.

  python3 -I server/tests/strike_screens_e2e.py BUILD_DIR      (run from the repository root)

  1. files: every ./img/ and ./cdn/ image the four templates and LEAN_END_SCREEN reference is in
     ui/strike/ (img/currencies/krunkies.png, img/frvr-ui/krunkies.svg, img/frvr-ui/xp-icon.svg,
     img/victory-screen-bg.png among them) and is the file ui/strike/README.md records (sha256).
  2. --render-scenario of tools/uidiff/scenarios/strike/{missions,reward-to-claim,match-details,
     progression-track}.json: a 1280x720 PNG and the screen's own log line (3 daily missions of the
     signed-in fixture, the KR reward card, the FFA board, 50 track levels at level 12).
  3. a live offline Strike client (--flavor strike --offline, an input script clicking through
     strikeAction): Battle Pass > Missions for a guest is FRVR's I2 "Loading Missions" (no bpData),
     Rewards "Loading Rewards"; PROFILE has no Rewards Track (no game config: KU renders nothing);
     the Missions tab selector click switches back; after `bot 2` + `end` a click on the round end's podium opens MATCH_DETAILS_SCREEN (title, header + 3
     rows, our row --player, Report players), a click on Report players swaps in the report symbols, a
     click on Close returns to the round end with its victory title still filled.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). KRUNKNATIVE_ASSETS_DIR is passed through
(a worktree without the untracked asset dirs). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import hashlib
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
WORK = tempfile.mkdtemp(prefix="kn-strike-screens-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
STRIKE = os.path.join(ROOT, "ui", "strike")
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


# ---------------------------------------------------------------- 1. the images

def files_case():
    readme = open(os.path.join(STRIKE, "README.md"), encoding="utf-8").read()
    recorded = dict(re.findall(r"^\| ((?:cdn/)?img/[^ |]+) \| https://[^ |]+ \| ([0-9a-f]{64}) \|$", readme, re.M))
    for p in ("img/currencies/krunkies.png", "img/frvr-ui/krunkies.svg", "img/frvr-ui/xp-icon.svg", "img/victory-screen-bg.png",
              "img/thrown_0.png", "img/wallbang_0.png"):
        f = os.path.join(STRIKE, p)
        ok = os.path.isfile(f) and p in recorded and hashlib.sha256(open(f, "rb").read()).hexdigest() == recorded[p]
        check(f"1: {p} vendored, sha256 as README.md records", ok, recorded.get(p, "not in README.md"))
    refs = set()
    for name in ("MISSION_TAB_SCREEN", "REWARD_TO_CLAIM", "MATCH_DETAILS_SCREEN", "PROGRESSION_TRACK", "LEAN_END_SCREEN"):
        text = open(os.path.join(STRIKE, "screens", name + ".html"), encoding="utf-8").read()
        refs.update(m for m in re.findall(r'src="\./((?:img|cdn)/[^"{]+)"', text))
    # the image paths strike_meta.c builds (pa(), the class icons, the challenge icons of the fixture)
    src = open(os.path.join(ROOT, "client", "src", "ui", "strike_meta.c"), encoding="utf-8").read()
    refs.update(m for m in re.findall(r'"\./((?:img|cdn)/[^"%]+\.(?:png|svg))"', src) if not m.startswith(("img/premium/", "img/features/")))
    refs.update({"cdn/img/chals/icon_1.png", "cdn/img/chals/icon_3.png", "cdn/img/chals/icon_0.png", "cdn/textures/classes/icon_0.png"})
    missing = sorted(p for p in refs if not os.path.isfile(os.path.join(STRIKE, p)))
    check(f"1: the {len(refs)} images the transcribed screens use are in ui/strike/", not missing, " ".join(missing))


# ---------------------------------------------------------------- 2. the scenarios

SCENARIOS = [
    ("missions", r"\[strike\] MISSION_TAB_SCREEN 3 daily missions"),
    ("reward-to-claim", r"\[strike\] screen REWARD_TO_CLAIM"),
    ("match-details", r"\[strike\] MATCH_DETAILS_SCREEN ffa, 1 players, 3 stat columns"),
    ("progression-track", r"\[strike\] PROGRESSION_TRACK 50 levels, level 12, 12 claimable"),
    # the in-game UI (strike_hud.c): the kill feed's thrown / wallbang icons, the kill card's weapon colour,
    # the mini scoreboard in FFA and TDM (team headers)
    ("ingame-killfeed-icons", r"\[strike\] in-game scenario: 3 kill-feed rows, kill card 0, scoreboard 0 \(0 rows\)"),
    ("killcard", r"\[strike\] in-game scenario: 0 kill-feed rows, kill card 1, scoreboard 0 \(0 rows\)"),
    ("ingame-scoreboard", r"\[strike\] in-game scenario: 0 kill-feed rows, kill card 0, scoreboard 1 \(4 rows\)"),
    ("ingame-scoreboard-team", r"\[strike\] in-game scenario: 0 kill-feed rows, kill card 0, scoreboard 1 \(4 rows\)"),
]


def scenario_case():
    for name, line in SCENARIOS:
        out = os.path.join(WORK, name + ".png")
        rc, text = run([f"{BUILD}/client_bin", "--render-scenario", f"tools/uidiff/scenarios/strike/{name}.json", out],
                       os.path.join(WORK, "cfg-" + name), os.path.join(WORK, name + ".log"))
        check(f"2: {name}: rendered 1280x720", rc == 0 and png_size(out) == (1280, 720), f"rc {rc}, {png_size(out)}")
        check(f"2: {name}: {line.replace(chr(92), '')}", re.search(line, text) is not None, os.path.join(WORK, name + ".log"))


# ---------------------------------------------------------------- 3. the live client

LIVE_SCRIPT = """menu
1600 call strikeAction "missions"
2200 text @bpmiss .tabsContent .frvr-bp-loading span
2250 nodes @bpsel .tabSelectorItem.selected
2300 nodes @bpcards .frvr-mission-card
2400 call strikeAction "bp-tab:0"
2800 text @bprew .tabsContent .frvr-bp-loading span
2850 clicksel .tabSelectorItem
3000 text @bpclick .tabsContent .frvr-bp-loading span
3050 call strikeAction "home"
3200 call strikeAction "profile"
3800 nodes @profile .frvr-account-progression
3850 nodes @track .frvr-progression-track-rewards
3900 call strikeAction "home"
4000 con bot 2
4500 con end
11000 nodes @lean .lean-end-screen__header
11100 clicksel .lean-end-screen__bottom-buttons .frvr-button
11600 text @mdtitle .match-details__title
11650 nodes @mdrows .match-details__legacy-board .frvr-lboard-end-screen__row-wrapper
11700 nodes @mdme .match-details__legacy-board .frvr-lboard-end-screen__position--player
11750 nodes @mdreport .match-details__legacy-board .frvr-lboard-end-screen__report
11800 frames {shots}/match-details 1
11900 clicksel .match-details__legacy-board .frvr-lboard-end-screen__report
12300 nodes @mdreport2 .match-details__legacy-board .frvr-lboard-end-screen__report
12350 nodes @mdsym .frvr-lboard-end-screen__report-symbol
12360 rects @pfp .match-details__legacy-board .frvr-lboard-end-screen__pfp img
12400 clicksel .frvr-lboard-end-screen__report-symbol img
12900 text @rpname .report-player .player-name strong
12950 clicksel .report-player .buttons .frvr-button--red
13300 nodes @rperr .report-player .input-error
13350 nodes @rpdropErr .report-player .dropdown-container.is-error
13400 clicksel .report-player .dropdown-header
13700 nodes @rpopts .report-player .dropdown-option
13750 clicksel .report-player .dropdown-option
14100 text @rpreason .report-player .dropdown-header span
14150 nodes @rperr2 .report-player .input-error
14200 clickid reasonComments
14250 type aimbot through walls all round
14600 nodes @rperr3 .report-player .input-error
14650 clicksel .report-player .buttons .frvr-button--red
15100 text @alert .mobPopAlert .alert-text span
15150 nodes @alertimg .mobPopAlert img[src="./img/frvr-ui/info-close.svg"]
15200 clickid alertScreenBtn
15600 nodes @mdback .match-details__title
15650 clicksel .lightbox__buttons .frvr-button
16100 nodes @mdgone .match-details__title
16150 text @leantitle .lean-end-screen__victory-defeat .frvr-in-game-ui__victory-text
16200 quit
"""


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d value=.*? text=(.*)" % re.escape(label), text)
    return (int(m.group(1)), m.group(2).strip()) if m else (-1, "")


def rects(text, label):
    m = re.search(r"\[input_script\] rects t=\d+ %s n=\d+(.*)" % re.escape(label), text)
    return [tuple(map(float, r.split("@")[1].split(","))) for r in m.group(1).split()] if m else []


def live_case():
    script = os.path.join(WORK, "live.txt")
    with open(script, "w") as f:
        f.write(LIVE_SCRIPT.format(shots=WORK))
    rc, text = run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], os.path.join(WORK, "cfg-live"),
                   os.path.join(WORK, "live.log"), {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    check("3: live client ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}")
    check("3: Battle Pass > Missions (guest, no bpData): I2 Loading Missions", text_of(text, "bpmiss") == (1, "Loading Missions"),
          str(text_of(text, "bpmiss")))
    check("3: Missions tab selected, no mission cards without bpData", nodes(text, "bpsel") == 1 and nodes(text, "bpcards") == 0,
          f"selected {nodes(text, 'bpsel')} cards {nodes(text, 'bpcards')}")
    check("3: Rewards tab: I2 Loading Rewards", text_of(text, "bprew") == (1, "Loading Rewards"), str(text_of(text, "bprew")))
    check("3: a click on the Missions tab selector: Loading Missions again", text_of(text, "bpclick") == (1, "Loading Missions"),
          str(text_of(text, "bpclick")))
    check("3: PROFILE without a game config: no Rewards Track (KU's !X2)", nodes(text, "profile") == 1 and nodes(text, "track") == 0,
          f"profile {nodes(text, 'profile')} track {nodes(text, 'track')}")
    check("3: the round end is up", nodes(text, "lean") == 1, str(nodes(text, "lean")))
    check("3: podium -> MATCH_DETAILS_SCREEN \"Match Details\"", text_of(text, "mdtitle") == (1, "Match Details"), str(text_of(text, "mdtitle")))
    check("3: the FFA board: header + 3 players", nodes(text, "mdrows") == 4, str(nodes(text, "mdrows")))
    check("3: our row is --player", nodes(text, "mdme") == 1, str(nodes(text, "mdme")))
    check("3: Report players, then the report symbols on the 2 other rows",
          nodes(text, "mdreport") == 1 and nodes(text, "mdreport2") == 0 and nodes(text, "mdsym") == 2,
          f"{nodes(text, 'mdreport')} {nodes(text, 'mdreport2')} {nodes(text, 'mdsym')}")
    check("3: the board's screenshot", png_size(os.path.join(WORK, "match-details_0000.png")) == (1280, 720))
    pfp = rects(text, "@pfp")
    check("3: the board's class icons fill their 43px cell (engine: <img width:100%> in a flex box)",
          len(pfp) >= 3 and all(abs(w - h) < 0.6 and w > 30 for (_, _, w, h) in pfp), str(pfp))
    check("3: a report symbol -> REPORT_PLAYER for that player", text_of(text, "rpname")[0] == 1 and text_of(text, "rpname")[1] != "",
          str(text_of(text, "rpname")))
    check("3: Report player with nothing filled in: both errors, the dropdown is-error",
          nodes(text, "rperr") == 2 and nodes(text, "rpdropErr") == 1, f"{nodes(text, 'rperr')} {nodes(text, 'rpdropErr')}")
    check("3: the dropdown opens with Select + the 5 reasons", nodes(text, "rpopts") == 6, str(nodes(text, "rpopts")))
    check("3: picking a reason (the last: Other) clears its error", text_of(text, "rpreason")[1] == "Other" and nodes(text, "rperr2") == 1,
          f"{text_of(text, 'rpreason')} {nodes(text, 'rperr2')}")
    check("3: 10+ characters typed clear the comments error", nodes(text, "rperr3") == 0, str(nodes(text, "rperr3")))
    check("3: a guest's report -> POPUP_ERROR \"Must be logged in\" (kn_brain takes reports from accounts)",
          text_of(text, "alert") == (1, "Must be logged in") and nodes(text, "alertimg") == 1, f"{text_of(text, 'alert')} {nodes(text, 'alertimg')}")
    check("3: Close -> back to the match details", nodes(text, "mdback") == 1, str(nodes(text, "mdback")))
    n, title = text_of(text, "leantitle")
    check("3: Close -> the round end again, its title still filled (keepAlive)", nodes(text, "mdgone") == 0 and n == 4 and title.startswith("#"),
          f"{nodes(text, 'mdgone')} {n} {title!r}")


# The in-game mini scoreboard (strike_hud.c, Gde / U5): the scoreboard key (Tab) held shows the
# table, released hides it. Desktop only: FRVR installs the key listeners for non-mobile clients.
HUD_SCRIPT = """0 play
2000 nodes @hud .frvr-in-game-ui__kill-log-container
2100 nodes @boardoff .frvr-in-game-ui__score-board-container
2200 down TAB
2400 nodes @boardon .frvr-in-game-ui__score-board-container
2420 nodes @boardrows .mini-score-board .data-row
2430 nodes @boardme .mini-score-board .mini-score-board__thats-you-row
2440 frames {shots}/scoreboard 1
2500 up TAB
2700 nodes @boardgone .frvr-in-game-ui__score-board-container
2800 quit
"""


def hud_case():
    script = os.path.join(WORK, "hud.txt")
    with open(script, "w") as f:
        f.write(HUD_SCRIPT.format(shots=WORK))
    rc, text = run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], os.path.join(WORK, "cfg-hud"),
                   os.path.join(WORK, "hud.log"), {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    check("3: hud: live client ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}")
    check("3: hud: no board without the key", nodes(text, "boardoff") == 0, str(nodes(text, "boardoff")))
    check("3: hud: holding Tab shows the mini scoreboard", nodes(text, "boardon") == 1, str(nodes(text, "boardon")))
    check("3: hud: the board lists the player and marks our row", nodes(text, "boardrows") >= 1 and nodes(text, "boardme") == 1,
          f"{nodes(text, 'boardrows')} {nodes(text, 'boardme')}")
    check("3: hud: the board's screenshot", png_size(os.path.join(WORK, "scoreboard_0000.png")) == (1280, 720))
    check("3: hud: releasing Tab hides it", nodes(text, "boardgone") == 0, str(nodes(text, "boardgone")))


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin", file=sys.stderr)
        return 2
    files_case()
    scenario_case()
    live_case()
    hud_case()
    ok = all(results)
    print(f"\n{sum(results)}/{len(results)} passed" + ("" if ok else f"  (logs in {WORK})"))
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
