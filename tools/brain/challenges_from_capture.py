#!/usr/bin/env python3
"""Turns a captured krunker.io challenge catalogue into a kn_brain migration (all three SQL dialects).

krunker.io's challenge catalogue is server data: no API serves it and the v10.0.3 bundle does not carry it
(doc/v10/challenges.md 4a). It only reaches a signed-in client as the account's activeChallenges (slot 38).
The native client writes that list to a file when KRUNKNATIVE_CHALLENGE_CAPTURE is set (account_ui.c,
doc/v10/challenges.md 4c):

    KRUNKNATIVE_CHALLENGE_CAPTURE=1 client_bin      # ~/.cache/krunknative/official-challenges-<date>.json
    python3 -I tools/brain/challenges_from_capture.py ~/.cache/krunknative/official-challenges-<date>.json

This writes services/db/migrations/{sqlite,postgres,mysql}/<NNNN>_official_challenges.sql (NNNN = the next
migration number, the same in the three directories). The migration:

  - inserts (or updates, on a rerun with a newer capture) one `challenges` row per captured challenge, with
    challenge_id = 100000 + CI (OFFICIAL_BASE), ctype = T, dat = DAT, rew = REW, icon = IC, name = N, sort = the
    list's order, active = 1. The per-account fields (PV, PT, SV, TR, PER) are dropped: progress lives in
    player_challenges.
  - retires (active = 0) every row of the Challenges window's types (map, weapon, nightmare, ranked, event:
    the 0107 seed's rows and any row an earlier capture left) before the upserts, so the captured rows are the
    whole list; a re-capture that drops a row leaves it retired. Daily / weekly rows retire only when the
    capture carries that type (the brain's rotation keeps its own otherwise).

The brain has no generator: the list is the seed plus the newest capture's migration. Options: --out DIR (the migrations directory), --number NNNN (the migration number),
--name NAME (the file stem after the number, default official_challenges), --stdout (print the SQLite text
only, write nothing). Exit 1 on a capture that is not the expected shape.
"""
import argparse
import json
import os
import re
import sys

OFFICIAL_BASE = 100000
WINDOW_TYPES = ("map", "weapon", "nightmare", "ranked", "event")
ROTATION_TYPES = ("daily", "weekly")
COLUMNS = ("challenge_id", "ctype", "dat", "rew", "icon", "name", "sort", "active")
# the client's progress keys (v10.0.3 checkChallenge / finalize, shared/src/progression.c) and the i18n templates
KNOWN_KEYS = {
    "chall", "score", "k", "w", "gp", "d", "stk", "hs", "ls", "mk", "fk", "ast", "spry", "wb", "airk", "qs", "ns", "lns",
    "cc", "drk", "tk", "tmk", "fb", "cps", "rk", "cb", "bzk", "dmg", "sad", "cad", "bst", "find", "rcm", "twm", "atkm",
    "defm", "dny", "cnf", "eggs", "geggs", "amp", "wpb", "cvrt", "aik", "prks", "pump", "mirror", "mybx", "kstk", "tower",
    "pkr", "island", "time", "amc", "uwk", "rnd",
}
PER_ACCOUNT = ("PV", "PT", "SV", "TR", "PER")


class CaptureError(Exception):
    pass


def load_capture(path):
    """(rows, meta): the activeChallenges list of a capture file (format 1), a bare list, or {"activeChallenges"}"""
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    meta = {}
    if isinstance(doc, list):
        rows = doc
    elif isinstance(doc, dict) and isinstance(doc.get("activeChallenges"), list):
        rows = doc["activeChallenges"]
        meta = {k: doc.get(k) for k in ("format", "source", "game", "captured_at") if doc.get(k) is not None}
    else:
        raise CaptureError("%s: not a capture (no activeChallenges list)" % path)
    if not rows:
        raise CaptureError("%s: the activeChallenges list is empty (the account had no challenges when captured)" % path)
    return rows, meta


def normalise(rows):
    """The catalogue rows: validated, per-account fields dropped, one per CI (the last wins), in list order."""
    out = {}
    order = []
    warnings = []
    for i, c in enumerate(rows):
        if not isinstance(c, dict):
            raise CaptureError("challenge %d is not an object: %r" % (i, c))
        ci = c.get("CI")
        if not isinstance(ci, int) or isinstance(ci, bool) or ci < 0 or ci >= 2 ** 31 - OFFICIAL_BASE:
            raise CaptureError("challenge %d: CI %r is not a challenge id" % (i, ci))
        typ = c.get("T")
        if not isinstance(typ, str) or not 1 <= len(typ) <= 16 or not re.fullmatch(r"[a-z_]+", typ):
            raise CaptureError("challenge CI %d: T %r is not a challenge type" % (ci, typ))
        dat = c.get("DAT")
        if not isinstance(dat, dict) or "ky" not in dat:
            raise CaptureError("challenge CI %d: DAT %r has no ky" % (ci, dat))
        keys = dat["ky"] if isinstance(dat["ky"], list) else [dat["ky"]]
        for k in keys:
            if not isinstance(k, str):
                raise CaptureError("challenge CI %d: ky %r is not a key" % (ci, dat["ky"]))
            if k not in KNOWN_KEYS:
                warnings.append("CI %d: progress key %r is not one the client tracks (kept as is)" % (ci, k))
        if "val" not in dat and "chall" not in keys:
            warnings.append("CI %d: DAT has no val" % ci)
        rew = c.get("REW")
        if rew is None:
            rew = {}
        if not isinstance(rew, dict):
            raise CaptureError("challenge CI %d: REW %r is not an object" % (ci, rew))
        icon = c.get("IC", 0)
        if not isinstance(icon, int) or isinstance(icon, bool):
            raise CaptureError("challenge CI %d: IC %r is not a number" % (ci, icon))
        name = c.get("N") or ""
        if not isinstance(name, str):
            raise CaptureError("challenge CI %d: N %r is not text" % (ci, name))
        if len(name) > 128:
            raise CaptureError("challenge CI %d: the name is longer than 128 characters" % ci)
        row = {"CI": ci, "T": typ, "DAT": dat, "REW": rew, "IC": icon, "N": name}
        for k in c:
            if k not in row and k not in PER_ACCOUNT:
                warnings.append("CI %d: field %r is not part of the catalogue (dropped)" % (ci, k))
        if ci not in out:
            order.append(ci)
        out[ci] = row
    return [out[ci] for ci in order], warnings


def sql_text(s, mysql=False):
    s = s.replace("'", "''")
    if mysql:
        s = s.replace("\\", "\\\\")
    return "'" + s + "'"


def compact(o):
    return json.dumps(o, separators=(",", ":"), ensure_ascii=True, sort_keys=False)


def render(rows, dialect, meta, source):
    """The migration text of one dialect."""
    mysql = dialect == "mysql"
    label = {"sqlite": "SQLite", "postgres": "PostgreSQL", "mysql": "MySQL/MariaDB"}[dialect]
    types = sorted({r["T"] for r in rows})
    retire = [t for t in WINDOW_TYPES] + [t for t in ROTATION_TYPES if t in types]
    lines = [
        "-- official_challenges (%s): krunker.io's challenge catalogue, captured from a signed-in account's" % label,
        "-- activeChallenges (KRUNKNATIVE_CHALLENGE_CAPTURE, account_ui.c) and written by",
        "-- tools/brain/challenges_from_capture.py from %s" % os.path.basename(source),
    ]
    if meta:
        lines.append("-- (%s)." % ", ".join("%s %s" % (k, meta[k]) for k in ("source", "game", "captured_at") if k in meta))
    lines += [
        "-- %d rows: %s. challenge_id = %d + CI. The window types' rows retire first (active = 0), then the"
        % (len(rows), ", ".join("%d %s" % (sum(1 for r in rows if r["T"] == t), t) for t in types), OFFICIAL_BASE),
        "-- captured rows are upserted active. doc/v10/challenges.md 4c.",
        "UPDATE challenges SET active = 0 WHERE ctype IN (%s);" % ", ".join(sql_text(t) for t in retire),
    ]
    for sort, r in enumerate(rows):
        values = [
            str(OFFICIAL_BASE + r["CI"]),
            sql_text(r["T"], mysql),
            sql_text(compact(r["DAT"]), mysql),
            sql_text(compact(r["REW"]), mysql),
            str(r["IC"]),
            sql_text(r["N"], mysql),
            str(sort),
            "1",
        ]
        head = "INSERT INTO challenges (%s) VALUES (%s)" % (", ".join(COLUMNS), ", ".join(values))
        if mysql:
            tail = " ON DUPLICATE KEY UPDATE " + ", ".join("%s = VALUES(%s)" % (c, c) for c in COLUMNS[1:])
        else:
            tail = " ON CONFLICT (challenge_id) DO UPDATE SET " + ", ".join("%s = excluded.%s" % (c, c) for c in COLUMNS[1:])
        lines.append(head + tail + ";")
    return "\n".join(lines) + "\n"


def next_number(out_dir):
    numbers = {}
    for dialect in ("sqlite", "postgres", "mysql"):
        d = os.path.join(out_dir, dialect)
        if not os.path.isdir(d):
            raise CaptureError("%s: no %s migrations directory" % (out_dir, dialect))
        highest = 0
        for name in os.listdir(d):
            m = re.match(r"(\d{4})_", name)
            if m:
                highest = max(highest, int(m.group(1)))
        numbers[dialect] = highest
    if len(set(numbers.values())) != 1:
        raise CaptureError("the dialects' latest migrations differ (%s): pass --number" % numbers)
    return next(iter(numbers.values())) + 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("capture", help="the capture file (official-challenges-<date>.json)")
    ap.add_argument("--out", default=os.path.join("services", "db", "migrations"), help="the migrations directory")
    ap.add_argument("--number", type=int, help="the migration number (default: the next one)")
    ap.add_argument("--name", default="official_challenges", help="the file stem after the number")
    ap.add_argument("--stdout", action="store_true", help="print the SQLite migration, write nothing")
    args = ap.parse_args()
    try:
        rows, meta = load_capture(args.capture)
        rows, warnings = normalise(rows)
        for w in warnings:
            print("warning: " + w, file=sys.stderr)
        if args.stdout:
            sys.stdout.write(render(rows, "sqlite", meta, args.capture))
            return 0
        if not re.fullmatch(r"[a-z0-9_]+", args.name):
            raise CaptureError("--name: letters, digits and _ only")
        number = args.number if args.number is not None else next_number(args.out)
        if not 1 <= number <= 9999:
            raise CaptureError("--number: 1..9999")
        for dialect in ("sqlite", "postgres", "mysql"):
            path = os.path.join(args.out, dialect, "%04d_%s.sql" % (number, args.name))
            with open(path, "w", encoding="utf-8") as f:
                f.write(render(rows, dialect, meta, args.capture))
            print(path)
        by_type = {}
        for r in rows:
            by_type[r["T"]] = by_type.get(r["T"], 0) + 1
        print("%d challenges: %s" % (len(rows), ", ".join("%d %s" % (n, t) for t, n in sorted(by_type.items()))))
        return 0
    except (CaptureError, OSError, ValueError) as e:
        print("challenges_from_capture: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
