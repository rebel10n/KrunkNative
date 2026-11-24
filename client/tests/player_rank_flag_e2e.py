"""v10.0.7's rankedRank, flagIndex and titleIndex on the Tab board and the end table, on --render-scenario fixtures.

The scenario rows carry what NET_EXT_KN_PLAYER_RANK_FLAG and NET_EXT_KN_PLAYER_TITLE give a live row (player_accounts.c):
"rankedRank", "flagIndex" and "titleIndex" (with "verified" / "premium" / "creator" / "clan" / "badge" for the end
table's other player cell icons).
  - Tab board (f1985, deob-10.0.7.js:227637): `!isUsingRankedNames && rankedRank` adds, after the badges and before
    .newLeaderName, <div class="newLeaderRanked"><img src="<asset host>img/ranked/ranks/rank_<tier>.svg" alt="Rank"
    class="newLeaderRankedIcon"></div> (iîíìíîì, :98188-98191); a row with rankedRank 0 has none.
  - Tab board (:227638): .newLeaderName ends with iiiiìïí(titleIndex, !isUsingRankedNames) (:227597-227603), the title's
    <span class='leaderTitle' style='color:<its colour>;[Contraband outline]'><name></span> (iiiíîíî, :104102-104108);
    a row without a title (or with an id the catalogue lacks) has none.
  - End table (showTable, :226755-226764), the name cell in order: <div class='endTableFlag flag-icon flag-icon-<iso>'>
    (flagIndex > -1, flags[i][1]), the .endTablePfp picture, the .endTableN link with the " [clan]" span, the
    verified / beenhere / check_circle icons (margin-top:-12px; font-size:28px) and <div class="endTableRanked"><img ...
    class="endTableRankedIcon"></div>; a row without them has none of it.
  - Hide player names: another row's flag, rank, title, clan and icons go.

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
     "creator": 1, "clan": "QQ", "titleIndex": 87},
    {"rank": 3, "name": "Rhea", "score": 10, "kills": 1, "rankedRank": 0, "flagIndex": -1, "titleIndex": -1},
]
HUD = {"health": 100, "maxHealth": 100, "scoreboard": True}


def board_row(doc, name):
    """The .newLeaderItem of the row named `name` (its markup up to the name's end)."""
    m = re.search(r"<div class=['\"]newLeaderItem['\"]>((?:(?!newLeaderItem).)*?" + re.escape(name) + ")", doc, re.S)
    return m.group(1) if m else ""


def board_name(doc, name):
    """The .newLeaderName div of the row named `name`, double quotes."""
    m = re.search(r"<div class=['\"]newLeaderName[MF]?['\"]>" + re.escape(name) + r"(.*?)</div>", doc, re.S)
    return m.group(0).replace("'", '"') if m else ""


def title_of(tid):
    """assets/titles.json's title `tid`: (name, the tag style iiiíîíî gives it)."""
    with open(os.path.join(SRC, "assets", "titles.json"), encoding="utf-8") as f:
        t = next(t for t in json.load(f) if t["id"] == tid)
    style = "color:%s;" % t["color"]  # a title with its own colour (No-Life: #ff3333); Contraband without one is outlined
    return t["name"], style


def end_cell(doc, name):
    """The end table's name cell of the row named `name`."""
    for m in re.finditer(r"<td style=['\"]position: relative;['\"]>(.*?)</td>", doc, re.S):
        if ">" + name in m.group(1):
            return m.group(1).replace("'", '"')  # the document serializer writes double quotes
    return ""


def end_actions(doc):
    """The end table's last cell (the actions) of each row, by player name."""
    acts = {}
    for m in re.finditer(r"<tr[^>]*>(.*?)</tr>", doc, re.S):
        row = m.group(1).replace("'", '"')
        name = next((n for n in ("Local User", "Quentin", "Rhea") if ">" + n in row), None)
        tds = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S)
        if name and tds:
            acts[name] = tds[-1]
    return acts


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
    tname, tstyle = title_of(87)
    qn = board_name(doc, "Quentin")
    # (a scenario board row has no clan: the live row's " [clan]" span comes before the title, as in v10's markup)
    check("board: Quentin's titleIndex 87 ends .newLeaderName with its .leaderTitle tag (No-Life, #ff3333)",
          qn == '<div class="newLeaderName">Quentin<span class="leaderTitle" style="%s">%s</span></div>' % (tstyle, tname), qn)
    check("board: Rhea (titleIndex -1) and the local row (none) have no title",
          board_name(doc, "Rhea") and "leaderTitle" not in board_name(doc, "Rhea") and "leaderTitle" not in board_name(doc, "Local User"),
          board_name(doc, "Rhea"))
    doc = render("board-unknown-title", {"hud": HUD, "scoreboard": [dict(ROWS[1], titleIndex=9999)]})
    check("board: an id the catalogue lacks draws no title", doc and "Quentin" in doc and "leaderTitle" not in doc)
    doc = render("board-hidden", {"hud": HUD, "scoreboard": ROWS, "storage": {"knClient": json.dumps({"hidePlayerNames": True})}})
    check("hide player names: no other row's title on the board", doc and "leaderTitle" not in doc)

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

    # the actions cell (:226775-226777): an account sees its own row's share button and a Report button on another account's
    # row (`player.level`), none on a guest's; reportPlayer(`name`, this) opens the profile (:227065-227070)
    acct_rows = [dict(ROWS[0], level=42), dict(ROWS[1], level=18), dict(ROWS[2])]
    doc = render("endtable-actions", dict(end, scoreboard=acct_rows, account={"name": "Local User", "score": 200000, "funds": 500},
                                          official=True))
    acts = end_actions(doc)
    check("end actions: another account's row has v10's Report button",
          re.fullmatch(r'<div class="material-icons reportBut" title="Report Player" onclick="reportPlayer\((&quot;|")Quentin(&quot;|"),this\)">report</div>',
                       acts.get("Quentin", "")) is not None, acts.get("Quentin"))
    check("end actions: your own row has the share button (Share Match Results)",
          acts.get("Local User") == '<div class="material-icons reportBut shareBut" title="Share Match Results" onclick="shareResult()">share</div>',
          acts.get("Local User"))
    check("end actions: no button on a guest's row (no level)", acts.get("Rhea") == "", acts.get("Rhea"))
    doc = render("endtable-actions-guest", dict(end, scoreboard=acct_rows))
    check("end actions: a guest viewer gets neither button", doc and "reportBut" not in doc)

    # v10.0.7's skin reward in the end table's reward cell (ed "reward" [kr, skinReward], deob-10.0.7.js:226765-226770; the
    # server's NET_EXT_KN_END_REWARD): the item's .skinReward thumbnail in its rarity's border, openSales(index), then the KR
    rows = [dict(r) for r in ROWS]
    rows[1].update(reward=3, skinReward=7540)
    rows[2].update(reward=2)
    doc = render("endreward", dict(end, scoreboard=rows)).replace("'", '"')
    m = re.search(r'<div class="skinReward" style="border: 2px solid (#[0-9A-Fa-f]{3,8});" onclick="openSales\(7540\)">'
                  r'<img draggable="false" style="width: 32px;image-rendering: auto;" loading="lazy" src="([^"]+)"></div>'
                  r'<span style="color:#F8C55C">\+3</span> KR', doc)
    check("end reward: Quentin's cell draws the skin reward (.skinReward, openSales(7540)) before his +3 KR",
          m is not None and m.group(2).endswith(".png"), (doc[doc.find("skinReward") - 200:doc.find("skinReward") + 400] if "skinReward" in doc else doc[-400:]))
    check("end reward: one thumbnail only (Rhea's reward has no skin)", doc.count('class="skinReward"') == 1 and "\">+2</span> KR" in doc,
          str(doc.count('class="skinReward"')))

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
