#!/usr/bin/env python3
"""Download the whole game content (every map, model, texture, skin and sound the client loads) from
the kru download host into a local assets/ directory, the same tree the launcher installs.

The list is the launcher's content index: <origin>updates/client/latest.json -> assets.manifest (every
file as [path, sha256, size]) plus assets.packaged (files fetched by path). Objects are fetched by
sha256 from assets.objects and verified; a file already present with the right size and sha256 is
skipped, so the run can be repeated and resumes after an interruption. Failures are listed at the end
and written to <out>/fetch-failures.txt; exit status 1 when any file failed.

usage: fetch_content.py <out> [--origin URL] [--jobs N] [--only PREFIX ...] [--list]
  <out>        the assets directory (e.g. <install>/runtime/assets); files land at <out>/<path>
  --origin     default https://dl.krunk.io/
  --jobs       parallel downloads (default 6)
  --only       only paths under these prefixes (e.g. --only maps/ --only textures/weapons/skins/)
  --list       print the file count and size of the selection, download nothing
environment: KN_FETCH_PROXY (e.g. http://127.0.0.1:8888): every request goes through it.
Standard library only.
"""

import argparse
import concurrent.futures
import hashlib
import http.client
import json
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36"
DEFAULT_ORIGIN = "https://dl.krunk.io/"
PATH_RE = re.compile(r"^[A-Za-z0-9._/@+-]+$")
HEX_RE = re.compile(r"^[0-9a-f]{64}$")


def log(msg):
    print("fetch_content: " + msg, file=sys.stderr, flush=True)


def opener():
    proxy = os.environ.get("KN_FETCH_PROXY", "")
    handlers = [urllib.request.ProxyHandler({"http": proxy, "https": proxy} if proxy else {})]
    return urllib.request.build_opener(*handlers)


OPENER = None


def get(url, tries=6):
    # the origin's tunnel sometimes cuts a body short (IncompleteRead): retry those too
    global OPENER
    if OPENER is None:
        OPENER = opener()
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with OPENER.open(req, timeout=120) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404 or attempt == tries - 1:
                raise
        except (OSError, ValueError, http.client.HTTPException):
            if attempt == tries - 1:
                raise
        time.sleep(min(2 ** attempt, 20))


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_atomic(dst, data):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(dst), prefix=os.path.basename(dst) + ".tmp")
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.replace(tmp, dst)


def fetch_one(out, objects_url, origin_assets, entry):
    path, sha, size = entry
    dst = os.path.join(out, path)
    if os.path.isfile(dst) and os.path.getsize(dst) == size and sha256_of(dst) == sha:
        return "have", path, 0
    data = get(objects_url + sha[:2] + "/" + sha)
    if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
        raise ValueError("%s: size or sha256 mismatch" % path)
    write_atomic(dst, data)
    return "got", path, size


def fetch_packaged(out, origin_assets, version, path):
    dst = os.path.join(out, path)
    if os.path.isfile(dst) and os.path.getsize(dst) > 0:
        return "have", path, 0
    data = get(origin_assets + path + "?v=" + version)
    write_atomic(dst, data)
    return "got", path, len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--origin", default=DEFAULT_ORIGIN)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--only", action="append", default=[], help="path prefix below assets/")
    ap.add_argument("--list", action="store_true", help="count the selection, download nothing")
    args = ap.parse_args()

    origin = args.origin if args.origin.endswith("/") else args.origin + "/"
    manifest_url = origin + "updates/client/latest.json"
    manifest = json.loads(get(manifest_url))
    assets = manifest.get("assets") or sys.exit("fetch_content: %s has no assets section" % manifest_url)
    list_url = urllib.parse.urljoin(manifest_url, assets["manifest"])
    objects_url = urllib.parse.urljoin(manifest_url, assets["objects"])
    origin_assets = origin + "assets/"
    raw = get(list_url)
    if hashlib.sha256(raw).hexdigest() != assets["sha256"]:
        sys.exit("fetch_content: %s does not match the manifest's sha256" % list_url)
    files = json.loads(raw)["files"]
    packaged = assets.get("packaged") or []
    for path, sha, size in files:
        if not PATH_RE.match(path) or ".." in path.split("/") or not HEX_RE.match(sha):
            sys.exit("fetch_content: bad entry %r in %s" % (path, list_url))
    for path in packaged:
        if not PATH_RE.match(path) or ".." in path.split("/"):
            sys.exit("fetch_content: bad packaged path %r" % path)

    def wanted(path):
        return not args.only or any(path.startswith(p) for p in args.only)

    files = [f for f in files if wanted(f[0])]
    packaged = [p for p in packaged if wanted(p)]
    total = sum(f[2] for f in files)
    log("content %s from %s: %d indexed files (%.1f MB) + %d packaged" % (
        assets.get("version", "?"), origin, len(files), total / 1e6, len(packaged)))
    if args.list:
        return

    os.makedirs(args.out, exist_ok=True)
    version = assets.get("version", "")
    failures = []
    done = have = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        jobs = {pool.submit(fetch_one, args.out, objects_url, origin_assets, f): f[0] for f in files}
        jobs.update({pool.submit(fetch_packaged, args.out, origin_assets, version, p): p for p in packaged})
        for n, fut in enumerate(concurrent.futures.as_completed(jobs), 1):
            try:
                kind, path, _ = fut.result()
                if kind == "have":
                    have += 1
                else:
                    done += 1
            except Exception as e:  # keep going; failures are listed at the end
                failures.append("%s: %s" % (jobs[fut], e))
            if n % 500 == 0:
                log("%d / %d (got %d, had %d, failed %d)" % (n, len(jobs), done, have, len(failures)))

    if version:
        write_atomic(os.path.join(args.out, "version.txt"), (version + "\n").encode())
    failfile = os.path.join(args.out, "fetch-failures.txt")
    if failures:
        write_atomic(failfile, ("\n".join(sorted(failures)) + "\n").encode())
        for line in sorted(failures)[:20]:
            log("failed " + line)
        log("%d failed (full list: %s)" % (len(failures), failfile))
        sys.exit(1)
    if os.path.exists(failfile):
        os.unlink(failfile)
    log("done: %d got, %d already there" % (done, have))


if __name__ == "__main__":
    main()
