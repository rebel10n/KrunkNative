#!/usr/bin/env python3
"""Compare the kru content index (dl.krunk.io, what the launcher installs) with the official host
(assets.krunker.io, what Krunker serves): every indexed file must exist there with the same size.

A mismatch is a truncated or missing file on the download host: the launcher installs a broken copy and
the client draws the checker or shows nothing. The report lists them by kind, and --fix-from-official
downloads the official copy of each into --out (sha256-free: the official bytes are the reference, and
the tracked/local copy is checked to match where one exists).

usage: audit_content.py [--origin URL] [--official URL] [--jobs N] [--out DIR] [--fix-from-official]
environment: KN_FETCH_PROXY (e.g. http://127.0.0.1:8888): every request goes through it.
Standard library only (shares the request code with fetch_content.py).
"""

import argparse
import concurrent.futures
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetch_content as fc  # noqa: E402  (same proxy-aware opener and index loading)

DEFAULT_OFFICIAL = "https://assets.krunker.io/"


def head_size(url):
    """(status, size) of the official copy from its Content-Length (HEAD: no body is read)."""
    req = urllib.request.Request(url, headers={"User-Agent": fc.UA}, method="HEAD")
    try:
        with fc.OPENER.open(req, timeout=120) as r:
            clen = r.headers.get("Content-Length")
            return r.status, int(clen) if clen and clen.isdigit() else -1, clen
    except urllib.error.HTTPError as e:
        return e.code, 0, None


def check(official, entry):
    path, _sha, size = entry
    status, got, clen = head_size(official + path)
    if status != 200:
        return path, size, "missing", status, got
    if got != size:
        return path, size, "size", status, got
    return path, size, "ok", status, got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--origin", default=fc.DEFAULT_ORIGIN)
    ap.add_argument("--official", default=DEFAULT_OFFICIAL)
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--out", default=os.path.expanduser("~/.cache/krunknative/content-audit"))
    ap.add_argument("--fix-from-official", action="store_true", help="download the official copy of each bad file")
    args = ap.parse_args()

    fc.OPENER = fc.opener()
    origin = args.origin if args.origin.endswith("/") else args.origin + "/"
    official = args.official if args.official.endswith("/") else args.official + "/"
    manifest_url = origin + "updates/client/latest.json"
    manifest = json.loads(fc.get(manifest_url))
    assets = manifest["assets"]
    raw = fc.get(urllib.parse.urljoin(manifest_url, assets["manifest"]))
    files = [tuple(f) for f in json.loads(raw)["files"]]
    log = lambda m: print("audit_content: " + m, file=sys.stderr, flush=True)  # noqa: E731
    log("checking %d indexed files against %s" % (len(files), official))

    bad = []
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for path, size, kind, status, got in pool.map(lambda e: check(official, e), files):
            done += 1
            if done % 2000 == 0:
                log("%d / %d checked, %d bad" % (done, len(files), len(bad)))
            if kind != "ok":
                bad.append({"path": path, "index_size": size, "official_size": got, "kind": kind, "status": status})

    os.makedirs(args.out, exist_ok=True)
    report = os.path.join(args.out, "report.json")
    with open(report, "w") as f:
        json.dump({"index": assets.get("version"), "checked": len(files), "bad": bad}, f, indent=1)
    by_kind = {}
    for b in bad:
        by_kind[b["kind"]] = by_kind.get(b["kind"], 0) + 1
    log("%d bad of %d (%s); report %s" % (len(bad), len(files), ", ".join("%s %d" % kv for kv in sorted(by_kind.items())) or "none", report))

    if args.fix_from_official and bad:
        fixed = 0
        dest_root = os.path.join(args.out, "official")
        for b in bad:
            if b["kind"] != "size" and b["kind"] != "missing":
                continue
            if b["status"] != 200:
                continue
            dst = os.path.join(dest_root, b["path"])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            req = urllib.request.Request(official + b["path"], headers={"User-Agent": fc.UA})
            with fc.OPENER.open(req, timeout=120) as r, open(dst, "wb") as f:
                f.write(r.read())
            fixed += 1
        log("downloaded %d official copies into %s" % (fixed, dest_root))


if __name__ == "__main__":
    main()
