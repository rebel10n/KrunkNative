#!/usr/bin/env python3
"""Every Krunker asset the game references, in assets/ (``make assets-sync``): one run of all the inventories.

  1. install what the locks already pin (live_assets.lock, live_assets_v10.lock; never overwrites);
  2. list every referenced path: live-inventory/inventory.js (v7.1.4's data tables and every map under
     assets/maps, official/ and custom/ too: models, textures, sounds, user assets) and v10_store_assets.js
     (every store item of assets/skins.json: previews, models, textures);
  3. keep the paths the tree lacks (assets/<p>, assets/v10/<p>, ui/<p> for img/) and that
     live_assets_absent.tsv does not list (files Krunker's hosts answered 404 for);
  4. fetch them from Krunker's hosts (fetch_live_assets.py fetch; KN_FETCH_PROXY), add the valid 200s to
     live_assets_v10.lock and install them; 404s go to live_assets_absent.tsv;
  5. report what was added, what Krunker does not have, and what failed (exit 1: run again).

The deploy then ships the tree (deploy/ansible central.yml with kn_web_sync_assets, which re-indexes the
launcher's content set; the Asset CDN mirrors it). The client never depends on this alone: a user asset the
content set lacks still comes from the brain's mirror of Krunker's host (services/brain/user_assets.c).

usage: sync_live_assets.py [--stage DIR] [--list-only]
  --stage      download staging tree (default ~/.cache/krunknative/live-assets)
  --list-only  print the missing paths, fetch nothing
"""
import argparse
import datetime
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FETCH = os.path.join(HERE, "fetch_live_assets.py")
LOCK_V7 = os.path.join(HERE, "live_assets.lock")
LOCK_V10 = os.path.join(HERE, "live_assets_v10.lock")
ABSENT = os.path.join(HERE, "live_assets_absent.tsv")
ABSENT_HEADER = ("# Paths the game references that Krunker's asset hosts answer 404 for (tools/ui-import/sync_live_assets.py):\n"
                 "# not fetched again. Delete a line to retry it.\n# path\tstatus\tdate\n")


def log(msg):
    print("assets-sync: " + msg, file=sys.stderr, flush=True)


def run(cmd, cwd=ROOT, out=None):
    log("$ " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=cwd, stdout=out, text=True)
    return r.returncode


def tsv_paths(text):
    """(category, path) of `category<TAB>path[<TAB>source]` lines."""
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) >= 2:
            yield cols[0], cols[1].lstrip("/")


def present(path):
    roots = [os.path.join(ROOT, "assets", path), os.path.join(ROOT, "assets", "v10", path)]
    if path.startswith("img/"):
        roots.append(os.path.join(ROOT, "ui", path))
    return any(os.path.exists(r) for r in roots)


def read_absent():
    absent = set()
    if os.path.exists(ABSENT):
        for line in open(ABSENT, encoding="utf8"):
            if line.strip() and not line.startswith("#"):
                absent.add(line.split("\t")[0])
    return absent


def inventory():
    """Every referenced (category, path), de-duplicated by path (the first category wins)."""
    found = {}
    inv_dir = os.path.join(HERE, "live-inventory")
    r = subprocess.run(["node", "-r", "./v7env.js", "inventory.js"], cwd=inv_dir, capture_output=True, text=True,
                       env=dict(os.environ, KN_REPO=ROOT))
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit("assets-sync: inventory.js failed")
    for cat, p in tsv_paths(r.stdout):
        found.setdefault(p, cat)
    # exit 1 = a required store file is missing: that is what this run is for
    r = subprocess.run(["node", os.path.join(HERE, "v10_store_assets.js"), "--tsv"], cwd=ROOT, capture_output=True, text=True)
    if r.returncode not in (0, 1):
        sys.stderr.write(r.stderr)
        raise SystemExit("assets-sync: v10_store_assets.js failed")
    for cat, p in tsv_paths(r.stdout):
        found.setdefault(p, cat)
    return found


def manifest_rows(path):
    rows = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf8"):
            if line.startswith("#"):
                continue
            cols = line.rstrip("\n").split("\t")
            if len(cols) >= 7:
                rows[cols[1]] = (cols[2], cols[6] == "1")  # path -> (status, valid); the last answer wins
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--stage", default=os.path.join(os.path.expanduser("~"), ".cache", "krunknative", "live-assets"))
    ap.add_argument("--list-only", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.stage, exist_ok=True)

    if not a.list_only:
        for lock, roots in ((LOCK_V7, "assets,ui"), (LOCK_V10, "assets,assets-v10,assets-aac,ui")):
            if run([sys.executable, "-I", FETCH, "install", "--lock", lock, "--roots", roots, "--assets", "assets", "--ui", "ui",
                    "--stage", a.stage]):
                log("installing %s failed (above); continuing with the inventory" % os.path.basename(lock))

    refs = inventory()
    absent = read_absent()
    missing = sorted((p, c) for p, c in refs.items() if not present(p) and p not in absent)
    log("%d referenced paths, %d missing from the tree (%d known absent at Krunker)" % (len(refs), len(missing), len(absent)))
    if a.list_only:
        for p, c in missing:
            print("%s\t%s" % (c, p))
        return 0
    if not missing:
        log("nothing to fetch: every referenced file is in the tree")
        return 0

    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    todo = os.path.join(a.stage, "missing-%s.tsv" % stamp)
    manifest = os.path.join(a.stage, "manifest-%s.tsv" % stamp)
    with open(todo, "w", encoding="utf8") as f:
        f.writelines("%s\t%s\n" % (c, p) for p, c in missing)
    rc = run([sys.executable, "-I", FETCH, "fetch", "--list", todo, "--stage", a.stage, "--manifest", manifest,
              "--merge-lock", LOCK_V10])
    rows = manifest_rows(manifest)
    got = [p for p, (st, ok) in rows.items() if st == "200" and ok]
    gone = [p for p, (st, ok) in rows.items() if st == "404"]
    bad = sorted(p for p, _ in missing if p not in rows or (p not in got and p not in gone))
    if gone:
        new = not os.path.exists(ABSENT)
        with open(ABSENT, "a", encoding="utf8") as f:
            if new:
                f.write(ABSENT_HEADER)
            day = stamp[:8]
            f.writelines("%s\t404\t%s\n" % (p, day) for p in sorted(gone) if p not in absent)
    if got and run([sys.executable, "-I", FETCH, "install", "--lock", LOCK_V10, "--roots", "assets,assets-v10,assets-aac,ui",
                    "--assets", "assets", "--ui", "ui", "--stage", a.stage]):
        log("install failed (above)")
        rc = rc or 1
    log("added %d files (pinned in %s), %d not on Krunker's hosts (%s), %d failed"
        % (len(got), os.path.relpath(LOCK_V10, ROOT), len(gone), os.path.relpath(ABSENT, ROOT), len(bad)))
    for p in bad[:40]:
        log("  failed: %s %s" % (p, rows.get(p, ("no answer",))[0]))
    if bad:
        log("run again for the failures (a proxy or the host's rate limit); manifest %s" % manifest)
    log("next: ship the tree (deploy/ansible central.yml -e kn_web_sync_assets=true, see deploy/ansible/README.md)")
    return 1 if bad or rc else 0


if __name__ == "__main__":
    sys.exit(main())
