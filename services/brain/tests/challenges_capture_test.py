#!/usr/bin/env python3
"""The capture tool's migration (tools/brain/challenges_from_capture.py, doc/v10/challenges.md 4c).

  challenges_capture_test.py <repo root>

Runs the tool on a fixture capture (the client's KRUNKNATIVE_CHALLENGE_CAPTURE shape, invented rows: this is no
official data) into a scratch migrations tree, then:
  - the three dialect files exist under the next number (after 0353), and the SQLite one applies to a
    `challenges` table of the brain's columns: the window types' seed rows retire, the captured rows are
    active with ids 100000 + CI, the per-account fields are gone, a rotation type the capture lacks keeps
    its seed rows;
  - a re-capture that drops a map row retires it; a re-run upserts (no duplicate ids);
  - PostgreSQL / MySQL files use their upsert forms, and quotes (and MySQL backslashes) are escaped;
  - a capture without a list, and a non-object challenge, exit 1.
"""
import importlib.util
import json
import os
import sqlite3
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(sys.argv[1])
TOOL = os.path.join(ROOT, "tools", "brain", "challenges_from_capture.py")
FAILS = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    if not ok:
        FAILS.append(name)


spec = importlib.util.spec_from_file_location("challenges_from_capture", TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def capture(rows, path):
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"format": 1, "source": "krunker.io", "game": "10.0.3", "captured_at": "2026-10-22T00:00:00Z",
                   "activeChallenges": rows}, f)
    return path


def row(ci, typ, dat, rew, icon, name, **per):
    r = {"CI": ci, "T": typ, "DAT": dat, "REW": rew, "IC": icon, "N": name, "PV": 0, "PT": 0, "SV": 0, "TR": 0}
    r.update(per)
    return r


FIRST = [
    row(100, "map", {"ky": "k", "val": 25, "mp": 0}, {"kr": 10}, 0, "Get 25 kills on Burg", PV=3),
    row(101, "map", {"ky": "k", "val": 10, "mp": 2}, {"kr": 15}, 1, "O'Brien's \\ map"),
    row(200, "weapon", {"ky": "k", "val": 50, "wep": 1}, {"kr": 15}, 5, "Get 50 kills with Pistol"),
    row(400, "nightmare", {"ky": "kstk", "val": 15}, {"kr": 50}, 8, "Get a 15 killstreak"),
    row(500, "ranked", {"ky": "w", "val": 5}, {"kr": 20}, 2, "Win 5 ranked matches"),
]
SECOND = [r for r in FIRST if r["CI"] != 101]  # the re-capture drops the second map row

failed = 0
with tempfile.TemporaryDirectory(prefix="kn-cap-test-") as work:
    migs = os.path.join(work, "migrations")
    for d in ("sqlite", "postgres", "mysql"):
        os.makedirs(os.path.join(migs, d))
        with open(os.path.join(migs, d, "0001_base.sql"), "w") as f:
            f.write("-- base\n")
        with open(os.path.join(migs, d, "0353_paint_trade.sql"), "w") as f:
            f.write("-- last\n")
    first = capture(FIRST, os.path.join(work, "cap1.json"))
    second = capture(SECOND, os.path.join(work, "cap2.json"))

    def run(*args):
        return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True)

    r = run(first, "--out", migs)
    check("tool exits 0 on the fixture capture", r.returncode == 0, r.stderr)
    for d in ("sqlite", "postgres", "mysql"):
        check("%s migration written as 0354" % d, os.path.isfile(os.path.join(migs, d, "0354_official_challenges.sql")))
    sq = open(os.path.join(migs, "sqlite", "0354_official_challenges.sql"), encoding="utf-8").read()

    # the SQLite file on a challenges table of the brain's columns, with seed rows
    db = os.path.join(work, "brain.db")
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE challenges (challenge_id INTEGER PRIMARY KEY, ctype TEXT, dat TEXT, rew TEXT, icon INTEGER,"
                " name TEXT, sort INTEGER, active INTEGER)")
    seed = [
        (1, "map", '{"ky":"k","val":25,"mp":0}', '{"kr":10}', 0, "Get 25 kills on Burg", 0, 1),
        (13, "nightmare", '{"ky":"kstk","val":15}', '{"kr":50}', 8, "seed nightmare", 0, 1),
        (20, "daily", '{"ky":"k","val":10}', '{"kr":5}', 3, "seed daily", 0, 1),
    ]
    con.executemany("INSERT INTO challenges VALUES (?,?,?,?,?,?,?,?)", seed)
    con.executescript(sq)
    got = {r[0]: r for r in con.execute("SELECT challenge_id, ctype, dat, rew, icon, name, sort, active FROM challenges")}
    check("the seed's map row retires", got[1][7] == 0, got.get(1))
    check("the seed's nightmare row retires", got[13][7] == 0, got.get(13))
    check("the seed's daily row stays (the capture has no daily)", got[20][7] == 1, got.get(20))
    check("captured rows are active at 100000 + CI", all(got[100000 + ci][7] == 1 for ci in (100, 101, 200, 400, 500)),
          sorted(got))
    check("captured map row keeps DAT / REW / IC / N",
          got[100100][1:6] == ("map", '{"ky":"k","val":25,"mp":0}', '{"kr":10}', 0, "Get 25 kills on Burg"), got[100100])
    check("per-account fields are dropped from DAT / REW", "PV" not in got[100100][2] and "PT" not in got[100100][3])
    check("the quote and backslash survive in the name", got[100101][5] == "O'Brien's \\ map", got[100101][5])
    check("sort follows the list order", got[100200][6] == 2 and got[100500][6] == 4, (got[100200][6], got[100500][6]))
    con.close()

    # a re-capture without the second map row, applied as the next migration (the file the tool writes next)
    r2 = run(second, "--out", migs)
    check("the re-capture writes 0355", r2.returncode == 0 and os.path.isfile(os.path.join(migs, "sqlite", "0355_official_challenges.sql")), r2.stderr)
    con = sqlite3.connect(db)
    con.executescript(open(os.path.join(migs, "sqlite", "0354_official_challenges.sql"), encoding="utf-8").read())  # (rerun of 0354)
    con.executescript(open(os.path.join(migs, "sqlite", "0355_official_challenges.sql"), encoding="utf-8").read())
    got = {r[0]: r for r in con.execute("SELECT challenge_id, ctype, active FROM challenges")}
    check("a re-capture retires the dropped map row", got[100101][2] == 0, got.get(100101))
    check("a re-capture keeps the rest active", got[100100][2] == 1 and got[100500][2] == 1, got)
    check("a re-run upserts without duplicate ids", len(got) == 3 + 5, len(got))
    con.close()

    pg = open(os.path.join(migs, "postgres", "0354_official_challenges.sql"), encoding="utf-8").read()
    my = open(os.path.join(migs, "mysql", "0354_official_challenges.sql"), encoding="utf-8").read()
    check("PostgreSQL upserts with ON CONFLICT", "ON CONFLICT (challenge_id) DO UPDATE SET" in pg)
    check("MySQL upserts with ON DUPLICATE KEY UPDATE", "ON DUPLICATE KEY UPDATE" in my and "ON CONFLICT" not in my)
    check("MySQL doubles the backslash in the name", "O''Brien''s \\\\ map" in my, [l for l in my.splitlines() if "Brien" in l])
    check("PostgreSQL keeps the single backslash", "O''Brien''s \\ map" in pg)

    # the refusals
    empty = os.path.join(work, "empty.json")
    with open(empty, "w") as f:
        json.dump({"activeChallenges": []}, f)
    check("an empty list is refused (exit 1)", run(empty, "--out", migs).returncode == 1)
    bad = capture([{"CI": 1, "T": "map", "DAT": 5}], os.path.join(work, "bad.json"))
    check("a challenge without a DAT object is refused (exit 1)", run(bad, "--out", migs).returncode == 1)
    nolist = os.path.join(work, "nolist.json")
    with open(nolist, "w") as f:
        json.dump({"format": 1}, f)
    check("a capture without activeChallenges is refused (exit 1)", run(nolist, "--out", migs).returncode == 1)

if FAILS:
    print("%d failed" % len(FAILS))
    sys.exit(1)
print("all passed")
