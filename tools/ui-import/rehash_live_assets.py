#!/usr/bin/env python3
"""Drift check of every Krunker asset KrunkNative ships against Krunker's live hosts (``make assets-rehash``).

``make assets-sync`` only proves a referenced file is *present*; Krunker sometimes replaces a file in place
under the same name (2026-11-19: body_328/330.obj, melee_198/200.glb, hat_527.png). This compares each
shipped file with the live one without downloading it:

  - the inventory is sync_live_assets.inventory() (every map, official/ and custom/, their KrunkScript, the
    v7.1.4 data tables and every v10 store item), plus the paths both locks pin;
  - each path present in the tree is resolved the way the v10 client resolves it (assets/v10/<p> before
    assets/<p>; img/ paths under ui/), and its MD5 is compared with the host's: assets.krunker.io and
    user-assets.krunker.io are Google Cloud Storage buckets behind Cloudflare, whose HEAD answer carries
    ``x-goog-hash: md5=<base64>`` (and an ETag that is the hex MD5 for non-composite objects). No body is
    fetched, so a full pass is ~21.5k HEAD requests;
  - a text file Krunker serves with CRLF line ends that git normalised to LF counts as the same (as in
    fetch_live_assets.py check); an assets-aac entry (an MP4/AAC file the host serves under .mp3, installed
    transcoded) is not comparable and is skipped.

Results go to ``<state>/rehash.tsv`` (path, status, local md5, live md5, size, note), resumable: a rerun skips
paths already answered unless --refresh. The report lists every ``changed`` path (Krunker's bytes differ from
the tree's), ``absent`` (404 at Krunker now) and ``error``; exit 1 when anything changed, 2 on errors only.
Resolving a change (taking Krunker's new file or keeping KN's) is a separate, deliberate step:
fetch_live_assets.py fetch/install with the lock, then the documented asset deploy.

usage: rehash_live_assets.py [--state DIR] [--workers N] [--refresh] [--limit N] [--only PREFIX]
environment: KN_FETCH_PROXY (default http://127.0.0.1:8888 here; Krunker's hosts 403 some networks), KN_FETCH_UA.
"""
import argparse
import base64
import concurrent.futures
import hashlib
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import fetch_live_assets as fla  # noqa: E402  (url_of, read_lock, TEXT_EXTS, DEFAULT_UA)
import sync_live_assets as sla  # noqa: E402  (inventory)

SITE_HOST = "https://krunker.io/"
HEADER = "# path\tstatus\tlocal_md5\tlive_md5\tsize\tnote\n"


def local_file(path):
    """The file the v10 client loads for a host path, or None."""
    cands = [os.path.join(ROOT, "assets", "v10", path), os.path.join(ROOT, "assets", path)]
    if path.startswith("img/"):
        cands.insert(0, os.path.join(ROOT, "ui", path))
    for c in cands:
        if os.path.isfile(c):
            return c
    return None


def md5s(path):
    """MD5s the live file may have: of the bytes, and of the CRLF form of an LF-only text file."""
    with open(path, "rb") as f:
        data = f.read()
    out = [hashlib.md5(data).hexdigest()]
    if os.path.splitext(path)[1].lower() in fla.TEXT_EXTS and b"\r" not in data:
        out.append(hashlib.md5(data.replace(b"\n", b"\r\n")).hexdigest())
    return out, len(data)


class Head:
    def __init__(self):
        proxy = os.environ.get("KN_FETCH_PROXY", "http://127.0.0.1:8888")
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {}))
        self.ua = os.environ.get("KN_FETCH_UA", fla.DEFAULT_UA)
        self.lock = threading.Lock()
        self.pause_until = 0.0

    def live_md5(self, url, body_hash=False):
        """(status, hex md5 or '', note) from a HEAD (or, with body_hash, a GET whose body is hashed in memory
        and discarded: the site server sends no MD5); retries 429 / 5xx / network errors with backoff."""
        last = (0, "", "no answer")
        for attempt in range(6):
            wait = self.pause_until - time.time()
            if wait > 0:
                time.sleep(wait)
            req = urllib.request.Request(url, method="GET" if body_hash else "HEAD",
                                         headers={"User-Agent": self.ua, "Referer": "https://krunker.io/"})
            try:
                with self.opener.open(req, timeout=60) as r:
                    if body_hash:
                        return r.status, hashlib.md5(r.read()).hexdigest(), "site"
                    h = r.headers
                    for v in h.get_all("x-goog-hash") or []:
                        v = v.strip()
                        if v.startswith("md5="):
                            return r.status, base64.b64decode(v[4:]).hex(), ""
                    etag = (h.get("ETag") or "").strip('"')
                    if len(etag) == 32 and all(c in "0123456789abcdef" for c in etag):
                        return r.status, etag, "etag"
                    return r.status, "", "no md5 header"
            except urllib.error.HTTPError as e:
                last = (e.code, "", "")
                if e.code in (429, 500, 502, 503, 504):
                    with self.lock:
                        self.pause_until = max(self.pause_until, time.time() + min(60, 2 ** attempt))
                    continue
                return last
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                last = (0, "", "network: %s" % e)
                time.sleep(min(30, 2 ** attempt))
        return last


def targets(only):
    refs = dict(sla.inventory())
    skip = set()
    for lock in (sla.LOCK_V7, sla.LOCK_V10):
        for e in fla.read_lock(lock):
            if e["root"] == "assets-aac":
                skip.add(e["path"])
            elif e["root"] in ("assets", "assets-v10", "ui"):
                refs.setdefault(e["path"], "lock")
    out = []
    for p in sorted(refs):
        if p in skip or (only and not p.startswith(only)):
            continue
        f = local_file(p)
        if f:
            out.append((p, f))
    return out, len(refs)


def read_state(path):
    done = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf8"):
            if line.startswith("#") or not line.strip():
                continue
            cols = line.rstrip("\n").split("\t")
            if len(cols) >= 6:
                done[cols[0]] = cols
    return done


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--state", default=os.path.join(os.path.expanduser("~"), ".cache", "krunknative", "asset-rehash"))
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--refresh", action="store_true", help="ask again for paths the state file already answered")
    ap.add_argument("--limit", type=int, default=0, help="check at most N paths (smoke runs)")
    ap.add_argument("--only", default="", help="only host paths starting with this prefix")
    a = ap.parse_args()
    os.makedirs(a.state, exist_ok=True)
    state_path = os.path.join(a.state, "rehash.tsv")
    done = {} if a.refresh else read_state(state_path)

    todo, nrefs = targets(a.only)
    pending = [(p, f) for p, f in todo if p not in done or done[p][1] == "error"]
    if a.limit:
        pending = pending[:a.limit]
    print("assets-rehash: %d referenced paths, %d in the tree, %d to ask (%d answered before)"
          % (nrefs, len(todo), len(pending), len(todo) - len(pending)), file=sys.stderr, flush=True)

    head = Head()
    out_lock = threading.Lock()
    new = not os.path.exists(state_path) or a.refresh
    sf = open(state_path, "w" if a.refresh else "a", encoding="utf8")
    if new:
        sf.write(HEADER)

    def one(item):
        p, f = item
        local, size = md5s(f)
        status, live, note = head.live_md5(fla.url_of(p))
        if status == 404 and p.startswith("img/"):
            # the menu page's own images: krunker.io/img/ (the site), not the asset bucket
            status, live, note = head.live_md5(SITE_HOST + urllib.parse.quote(p), body_hash=True)
        if status == 200 and live:
            res = "same" if live in local else "changed"
        elif status == 404:
            res = "absent"
        elif status == 200:
            res = "unknown"
        else:
            res = "error"
            note = note or "HTTP %s" % status
        row = [p, res, local[0], live, str(size), note or os.path.relpath(f, ROOT)]
        with out_lock:
            sf.write("\t".join(row) + "\n")
            sf.flush()
        done[p] = row

    n = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.workers)) as ex:
        for _ in ex.map(one, pending):
            n += 1
            if n % 1000 == 0:
                print("assets-rehash: %d / %d" % (n, len(pending)), file=sys.stderr, flush=True)
    sf.close()

    rows = [done[p] for p, _ in todo if p in done]
    counts = {}
    for r in rows:
        counts[r[1]] = counts.get(r[1], 0) + 1
    for kind in ("changed", "absent", "unknown", "error"):
        for r in rows:
            if r[1] == kind:
                print("%s\t%s\t%s" % (kind, r[0], r[5]))
    print("assets-rehash: %s (state %s)" % (", ".join("%d %s" % (v, k) for k, v in sorted(counts.items())), state_path),
          file=sys.stderr)
    if counts.get("changed"):
        return 1
    return 2 if counts.get("error") else 0


if __name__ == "__main__":
    sys.exit(main())
