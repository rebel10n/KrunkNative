"""v10.0.7's rankedRank and flagIndex on the Tab board and the end table, on --render-scenario fixtures.

The scenario rows carry what NET_EXT_KN_PLAYER_RANK_FLAG gives a live row (player_accounts.c): "rankedRank" and
"flagIndex" (with "verified" / "premium" / "creator" / "clan" / "badge" for the end table's other player cell icons).
  - Tab board (f1985, deob-10.0.7.js:227637): `!isUsingRankedNames && rankedRank` adds, after the badges and before
    .newLeaderName, <div class="newLeaderRanked"><img src="<asset host>img/ranked/ranks/rank_<tier>.svg" alt="Rank"
    class="newLeaderRankedIcon"></div> (iîíìíîì, :98188-98191); a row with rankedRank 0 has none.
  - End table (showTable, :226755-226764), the name cell in order: <div class='endTableFlag flag-icon flag-icon-<iso>'>
    (flagIndex > -1, flags[i][1]), the .endTablePfp picture, the .endTableN link with the " [clan]" span, the
    verified / beenhere / check_circle icons (margin-top:-12px; font-size:28px) and <div class="endTableRanked"><img ...
    class="endTableRankedIcon"></div>; a row without them has none of it.
  - Hide player names: another row's flag, rank, clan and icons go.

  python3 -I client/tests/player_rank_flag_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ROOT
BIN = os.path.join(BUILD, "client_bin")
SCRATCH = os.path.expanduser("~/.cache/krunknative/rank-flag/e2e-%d" % os.getpid())
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def render(name, state):
    """Renders the state; returns the serialized document ("" on failure)."""
    os.makedirs(SCRATCH, exist_ok=True)
    base = {"menu": False, "window": None, "connected": True, "ready": True, "inGame": True, "mapName": "Kanji",
            "mapFile": "maps/311_kanji.json", "modeName": "Free for All", "name": "Local User", "region": "...", "anim_time": 0.8,
            "chat": []}
    base.update(state)
    path = os.path.join(SCRATCH, name + ".json")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": base, "frames": 3}, f)
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    html = os.path.join(SCRATCH, name + ".html")
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_DEBUG="1", XDG_CONFIG_HOME=os.path.join(SCRATCH, "cfg-" + name),
               KRUNKNATIVE_SCENARIO_HTML=html)
    png = os.path.join(SCRATCH, name + ".png")
    r = subprocess.run([BIN, "--render-scenario", path, png], cwd=SRC, env=env, capture_output=True, text=True, timeout=180)
    check(name + ": client_bin exits 0", r.returncode == 0, (r.stdout + r.stderr)[-800:] if r.returncode else "")
    if not os.path.isfile(html):
        check(name + ": the document was written", False)
        return ""
    with open(html, encoding="utf-8", errors="replace") as f:
        return f.read()


ROWS = [
    {"rank": 1, "name": "Local User", "score": 30, "kills": 3, "me": True},
    {"rank": 2, "name": "Quentin", "score": 20, "kills": 2, "rankedRank": 8, "flagIndex": 12, "verified": 1, "premium": 1,
     "creator": 1, "clan": "QQ"},
    {"rank": 3, "name": "Rhea", "score": 10, "kills": 1, "rankedRank": 0, "flagIndex": -1},
]
HUD = {"health": 100, "maxHealth": 100, "scoreboard": True}


def board_row(doc, name):
    """The .newLeaderItem of the row named `name` (its markup up to the name's end)."""
    m = re.search(r"<div class=['\"]newLeaderItem['\"]>((?:(?!newLeaderItem).)*?" + re.escape(name) + ")", doc, re.S)
    return m.group(1) if m else ""


def end_cell(doc, name):
    """The end table's name cell of the row named `name`."""
    for m in re.finditer(r"<td style=['\"]position: relative;['\"]>(.*?)</td>", doc, re.S):
        if ">" + name in m.group(1):
            return m.group(1).replace("'", '"')  # the document serializer writes double quotes
    return ""


RANK_IMG = re.compile(r'<div class="newLeaderRanked"><img src="([^"]*)" alt="Rank" class="newLeaderRankedIcon"></div>')
END_RANK_IMG = re.compile(r'<div class="endTableRanked"><img src="([^"]*)" alt="Rank" class="endTableRankedIcon"></div>')


def main():
    if not os.path.isfile(BIN):
        print("player_rank_flag_e2e: no client_bin in %s: skipped" % BUILD)
        return 77

    doc = render("board", {"hud": HUD, "scoreboard": ROWS})
    q, rh = board_row(doc, "Quentin"), board_row(doc, "Rhea")
    m = RANK_IMG.search(q)
    check("board: Quentin's rankedRank 8 adds .newLeaderRanked with rank_gold.svg", m is not None and m.group(1).endswith("img/ranked/ranks/rank_gold.svg"),
          q[-600:])
    check("board: after the badges, before .newLeaderName", m is not None and q.find("check_circle") < m.start() < q.find("newLeaderName"), q[-600:])
    check("board: Rhea (rankedRank 0) has no rank icon", rh and "newLeaderRanked" not in rh, rh[-300:])
    check("board: the local row has none", "newLeaderRanked" not in board_row(doc, "Local User"))

    end = {"hud": dict(HUD, scoreboard=False), "scoreboard": ROWS, "endTab": "leaderboard",
           "endscreen": {"place": 1, "result": 1, "text": "#1 VICTORY", "nextRound": "00:17", "results": 1}}
    doc = render("endtable", end)
    q, rh = end_cell(doc, "Quentin"), end_cell(doc, "Rhea")
    check("end: Quentin's cell starts with the flag (flagIndex 12: au)", q.startswith('<div class="endTableFlag flag-icon flag-icon-au"></div><img class="endTablePfp"'),
          q[:300])
    check("end: the clan in the link", re.search(r'<a class="endTableN"[^>]*>Quentin<span style="color:#fff"> \[QQ\]</span></a>', q) is not None, q[:600])
    icons = [i for i in ("verified", "beenhere", "check_circle") if
             "margin-top:-12px;font-size:28px;vertical-align:middle;\">%s</i>" % i in q]
    check("end: the verified / beenhere / check_circle icons after the link", icons == ["verified", "beenhere", "check_circle"]
          and q.find("</a>") < q.find(">verified</i>"), q[-700:])
    m = END_RANK_IMG.search(q)
    check("end: the rank last (.endTableRanked, rank_gold.svg)", m is not None and m.group(1).endswith("img/ranked/ranks/rank_gold.svg") and q.endswith(m.group(0)),
          q[-400:])
    check("end: Rhea has no flag, icon or rank", rh and "endTableFlag" not in rh and "endTableRanked" not in rh and "material-icons" not in rh, rh)

    # Client > Hide player names (knClient.json hidePlayerNames, client/tests/hide_names_e2e.py)
    doc = render("endtable-hidden", dict(end, storage={"knClient": json.dumps({"hidePlayerNames": True})}))
    cells = [m.group(1) for m in re.finditer(r"<td style=['\"]position: relative;['\"]>(.*?)</td>", doc, re.S) if "endTableN" in m.group(1)]
    check("hide player names: no other row's flag or rank", cells and not any("endTableFlag" in c or "endTableRanked" in c for c in cells),
          str([c[:120] for c in cells]))

    failed = [r for r in results if not r[1]]
    print("player_rank_flag_e2e: %d/%d passed" % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
