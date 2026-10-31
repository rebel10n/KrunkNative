#!/usr/bin/env python3
"""Krunker's official maps: every map the live client can be told to play, as JSON in assets/maps/official/.

How the official client gets a map (game.deob.js, tools/krunker/maps.py extracts every anchor):
  - the list: at startup `api.maps.getMaps()` = GET <GAPI_URI>/maps -> {"data": [{"name", "id"}, ...]};
    each entry is pushed onto the client's map array, so the array index IS the `init` mapIndex
    (the entries' "id" equals their position);
  - a map: `init [mapIndex, ...]` with no inline custom data -> `api.maps.getMap(mapIndex)` =
    GET <GAPI_URI>/maps/<mapIndex> (cache: no-cache) -> {"data": <the map JSON>};
  - a custom (community) game sends its map inline instead (init arg 9 = {data, id, creator, ...}).

`fetch` downloads the list and every map (GET only, one request at a time, --delay seconds apart,
retrying 429 / 5xx with backoff), keeps the raw responses in a stage directory (reused when their
sha256 matches the lock unless --refresh), writes each map's "data" to
assets/maps/official/<index>_<slug>.json (compact JSON; every map has its own file, even where an
older asset under assets/maps/ holds the same JSON), and pins everything in
tools/krunker/maps.lock.json (source URL, sha256 / size of the response and of the file).
`verify` checks the files against the lock (no network). After a fetch run
`make krunker-build` so shared/include/krunker_maps.h follows the lock.

usage:
  python3 -I tools/krunker/fetch_maps.py fetch [--api URL] [--delay S] [--refresh] [--only 43,44]
  python3 -I tools/krunker/fetch_maps.py verify

environment: KN_FETCH_PROXY (e.g. http://127.0.0.1:8888: gapi answers 403 to direct requests from
some networks), KN_FETCH_UA.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOCK = os.path.join(ROOT, "tools", "krunker", "maps.lock.json")
OUT_DIR = os.path.join(ROOT, "assets", "maps", "official")
DEFAULT_API = "https://gapi.svc.krunker.io"  # GAPI_URI's prod default (maps.py checks the bundle agrees)
DEFAULT_UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
              "Chrome/141.0.0.0 Safari/537.36")
MAX_MAPS = 1000          # sanity bound on the list
MAX_BYTES = 32 << 20     # per response


def log(msg):
    sys.stderr.write("fetch_maps: %s\n" % msg)


def stage_dir():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    return os.path.join(base, "krunknative", "krunker-maps")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def slug(name):
    s = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return s or "map"


def map_file(index, name):
    """assets-relative path of official map `index`."""
    return "maps/official/%02d_%s.json" % (index, slug(name))


def write_atomic(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def read_lock():
    try:
        with open(LOCK, encoding="utf8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


class Fetcher:
    def __init__(self, delay):
        proxy = os.environ.get("KN_FETCH_PROXY", "")
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {}))
        self.delay = delay
        self.last = 0.0
        self.count = 0

    def get(self, url):
        # the official client's request (api fetch(): Accept: application/json, GET, no credentials
        # for a guest), from krunker.io's page
        headers = {"User-Agent": os.environ.get("KN_FETCH_UA", DEFAULT_UA), "Accept": "application/json",
                   "Origin": "https://krunker.io", "Referer": "https://krunker.io/", "Cache-Control": "no-cache"}
        for attempt in range(5):
            wait = self.last + self.delay - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            self.last = time.monotonic()
            self.count += 1
            try:
                with self.opener.open(urllib.request.Request(url, headers=headers, method="GET"), timeout=60) as r:
                    data = r.read(MAX_BYTES + 1)
                if len(data) > MAX_BYTES:
                    raise SystemExit("%s: larger than %d bytes" % (url, MAX_BYTES))
                return data
            except urllib.error.HTTPError as e:
                # the client's own fetch retries 408 / 500 / 502 / 503 / 504 / 522 / 524 (game.deob.js
                # iììîìiî); 429 is its rate limit
                if e.code not in (408, 429, 500, 502, 503, 504, 522, 524) or attempt == 4:
                    raise SystemExit("%s: HTTP %d" % (url, e.code))
                log("%s: HTTP %d, retrying" % (url, e.code))
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                if attempt == 4:
                    raise SystemExit("%s: %s" % (url, getattr(e, "reason", e)))
                log("%s: %s, retrying" % (url, getattr(e, "reason", e)))
            time.sleep(min(60, 2 * 2 ** attempt))
        raise SystemExit("%s: failed" % url)


def parse_envelope(url, raw):
    try:
        doc = json.loads(raw.decode("utf8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise SystemExit("%s: not JSON (%s): %r" % (url, e, raw[:80]))
    if not isinstance(doc, dict) or "data" not in doc:
        raise SystemExit("%s: no {\"data\": ...} envelope" % url)
    if doc.get("error"):
        raise SystemExit("%s: error %r" % (url, doc["error"]))
    return doc["data"]


def compact(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf8")


def staged(path, digest):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    return data if digest and sha256(data) == digest else None


def cmd_fetch(a):
    api = a.api.rstrip("/")
    if not re.fullmatch(r"https?://[A-Za-z0-9.-]+(?::\d+)?", api):
        raise SystemExit("--api %r is not an origin" % a.api)
    old = read_lock() or {}
    old_maps = {m["index"]: m for m in old.get("maps", [])}
    stage = a.stage or stage_dir()
    os.makedirs(stage, exist_ok=True)
    f = Fetcher(a.delay)

    list_url = api + "/maps"
    raw_list = f.get(list_url)
    write_atomic(os.path.join(stage, "maps.json"), raw_list)
    entries = parse_envelope(list_url, raw_list)
    if not isinstance(entries, list) or not entries or len(entries) > MAX_MAPS:
        raise SystemExit("%s: data is not a map list" % list_url)
    only = set(int(x) for x in a.only.split(",")) if a.only else None

    maps = []
    for index, e in enumerate(entries):
        if not isinstance(e, dict) or not isinstance(e.get("name"), str) or not e["name"].strip():
            raise SystemExit("%s: entry %d has no name: %r" % (list_url, index, e))
        name = e["name"]
        mid = e.get("id")
        if mid != index:
            # the client indexes its array by position (push order); an id that differs is worth knowing
            log("warning: entry %d (%s) has id %r" % (index, name, mid))
        url = "%s/maps/%d" % (api, index)
        spath = os.path.join(stage, "map_%d.json" % index)
        prev = old_maps.get(index)
        raw = None
        # --refresh refetches what --only names (everything without --only); the rest comes from the stage
        if prev and prev.get("name") == name and (only is not None and index not in only or only is None and not a.refresh):
            raw = staged(spath, prev.get("response_sha256"))
        if raw is None and only is not None and index not in only and prev:
            raise SystemExit("map %d: --only skips it but its staged response is missing or stale" % index)
        if raw is None:
            raw = f.get(url)
            write_atomic(spath, raw)
            log("GET %s: %d bytes" % (url, len(raw)))
        data = parse_envelope(url, raw)
        if not isinstance(data, dict) or not isinstance(data.get("objects"), list):
            raise SystemExit("%s: data is not a map (no objects list)" % url)
        body = compact(data)
        rel = map_file(index, name)
        write_atomic(os.path.join(ROOT, "assets", rel), body)
        maps.append({
            "index": index,
            "name": name,
            "id": mid,
            "map_name": data.get("name") if isinstance(data.get("name"), str) else None,
            "file": rel,           # assets-relative: assets/maps/official/<index>_<slug>.json
            "url": url,
            "response_sha256": sha256(raw),
            "response_bytes": len(raw),
            "sha256": sha256(body),
            "bytes": len(body),
            "objects": len(data["objects"]),
        })

    # drop files of maps the list no longer has (only ours: maps/official/NN_*.json)
    keep = set(os.path.basename(m["file"]) for m in maps)
    if os.path.isdir(OUT_DIR):
        for fn in sorted(os.listdir(OUT_DIR)):
            if re.fullmatch(r"\d{2,}_[a-z0-9_]+\.json", fn) and fn not in keep:
                os.unlink(os.path.join(OUT_DIR, fn))
                log("removed stale %s" % fn)

    lock = {
        "schema": 1,
        "comment": "Krunker's official map list and every map's JSON (tools/krunker/fetch_maps.py); "
                   "shared/include/krunker_maps.h is generated from this file (make krunker-build)",
        "api": api,
        "list_url": list_url,
        "list_sha256": sha256(raw_list),
        "list_bytes": len(raw_list),
        "fetched": old.get("fetched") if old.get("list_sha256") == sha256(raw_list) and
        [(m["index"], m["sha256"]) for m in old.get("maps", [])] == [(m["index"], m["sha256"]) for m in maps]
        else datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "maps": maps,
    }
    write_atomic(LOCK, (json.dumps(lock, indent=1, ensure_ascii=False) + "\n").encode("utf8"))
    log("%d maps (%d requests), wrote %s; now run `make krunker-build`" % (len(maps), f.count, os.path.relpath(LOCK, ROOT)))
    return 0


def verify():
    """[problem strings]: every locked map file present with its sha256."""
    lock = read_lock()
    if not lock:
        return ["%s missing or invalid" % os.path.relpath(LOCK, ROOT)]
    out = []
    for i, m in enumerate(lock.get("maps", [])):
        if m.get("index") != i:
            out.append("map entry %d has index %r" % (i, m.get("index")))
        path = os.path.join(ROOT, "assets", m["file"])
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            out.append("map %d (%s): %s missing" % (m["index"], m["name"], m["file"]))
            continue
        if sha256(data) != m["sha256"]:
            out.append("map %d (%s): %s sha256 %s, lock %s" % (m["index"], m["name"], m["file"], sha256(data)[:16],
                                                               m["sha256"][:16]))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    fa = sub.add_parser("fetch")
    fa.add_argument("--api", default=DEFAULT_API)
    fa.add_argument("--delay", type=float, default=2.0, help="seconds between requests (default 2)")
    fa.add_argument("--refresh", action="store_true", help="download every map again, or only the --only maps (default: reuse the stage)")
    fa.add_argument("--only", help="comma-separated indices to download again (the rest from the stage)")
    fa.add_argument("--stage", help="raw response directory (default $XDG_CACHE_HOME/krunknative/krunker-maps)")
    sub.add_parser("verify")
    a = ap.parse_args(argv)
    if a.cmd == "fetch":
        return cmd_fetch(a)
    problems = verify()
    for p in problems:
        log(p)
    if not problems:
        log("%d maps match %s" % (len(read_lock()["maps"]), os.path.relpath(LOCK, ROOT)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
