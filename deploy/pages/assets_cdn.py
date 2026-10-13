#!/usr/bin/env python3
"""Mirror the game content of the own asset origin into a directory for the Cloudflare Pages
asset CDN (deploy/pages/README.md "Asset CDN"; deploy/pages/assets-cdn.sh deploys it).

The tree is exactly what the native client gets: the launcher's content index (the updates
manifest <origin>updates/client/latest.json -> assets.manifest, every file of the web asset
tree except the ones the packages ship, as sha256-addressed objects) plus those packaged
files, fetched by path. Objects are verified and kept in --objects (CI caches the directory),
so only new content is downloaded. Out:

    <out>/assets/<path>          the tree (hardlinks into --objects; wrangler skips symlinks)
    <out>/assets/version.txt     the origin's assets/version.txt: the ?v= of the web client
    <out>/_headers               CORS for any page origin, caching (README "Asset CDN")

Cloudflare Pages takes at most 20,000 files of at most 25 MiB in a deployment (Free plan);
--exclude drops path prefixes (the page's kn-config.json asset_fallback then serves them from
the own origin), anything still over the limits fails the run instead of deploying a partial tree.

usage: assets_cdn.py <out> [--origin URL] [--objects DIR] [--jobs N] [--exclude PREFIX ...]
                     [--max-files N] [--current URL]
       --current URL: print "same" and exit 0 when URL/assets/version.txt already equals the
                      origin's (nothing to deploy), else "changed".
Standard library only.
"""
import argparse
import concurrent.futures
import hashlib
import http.client
import json
import os
import re
import shutil
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

MAX_SIZE = 25 * 1024 * 1024
PATH_RE = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$")
VERSION_RE = re.compile(r"^[0-9A-Za-z._-]{1,64}$")
HEX_RE = re.compile(r"^[0-9a-f]{64}$")
UA = "KrunkNative-assets-cdn/1"

# Pages _headers (max 100 rules). Any page origin may read the content (public files, no
# credentials). ?v= URLs change with the content; Pages cannot match on the query, so an
# unversioned request (loader.js could not read version.txt) is the reason this is a day,
# not a year. version.txt itself is never cached (loader.js also fetches it no-store).
HEADERS = """# deploy/pages/assets_cdn.py
/*
  Access-Control-Allow-Origin: *
  Access-Control-Expose-Headers: Content-Length, Content-Range, ETag
  Timing-Allow-Origin: *
  X-Content-Type-Options: nosniff
/assets/*
  Cache-Control: public, max-age=86400
/assets/version.txt
  ! Cache-Control
  Cache-Control: no-store
"""


def log(msg):
    print("assets_cdn: " + msg, file=sys.stderr, flush=True)


def get(url, tries=6):
    # The origin sits behind a tunnel that now and then cuts a body short
    # (IncompleteRead is an HTTPException, not an OSError): retry those too.
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404 or attempt == tries - 1:
                raise
        except (OSError, ValueError, http.client.HTTPException):
            if attempt == tries - 1:
                raise
        time.sleep(min(2 ** attempt, 20))


def version_of(base):
    v = get(base + "assets/version.txt").decode().strip()
    if not VERSION_RE.match(v):
        sys.exit("assets_cdn: %sassets/version.txt is not a version: %r" % (base, v[:80]))
    return v


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.unlink(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def fetch_object(objects_url, cache, sha, size):
    obj = os.path.join(cache, sha[:2], sha)
    if os.path.isfile(obj) and os.path.getsize(obj) == size:
        return obj, 0
    data = get(objects_url + sha[:2] + "/" + sha)
    if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
        raise ValueError("object %s: size or sha256 mismatch" % sha)
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    tmp = obj + ".tmp%d" % os.getpid()
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, obj)
    return obj, size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--origin", default="https://assets.krunk.io/")
    ap.add_argument("--objects", default=os.path.expanduser("~/.cache/kn-assets-objects"))
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--exclude", action="append", default=[], help="path prefix below assets/ to leave out")
    ap.add_argument("--max-files", type=int, default=20000)
    ap.add_argument("--current", default="", help="the deployed CDN: exit early when it has this version")
    args = ap.parse_args()
    origin = args.origin if args.origin.endswith("/") else args.origin + "/"

    version = version_of(origin)
    if args.current:
        current = args.current if args.current.endswith("/") else args.current + "/"
        try:
            have = version_of(current)
        except (OSError, ValueError, SystemExit):
            have = ""
        print("same" if have == version else "changed")
        log("origin %s, cdn %s" % (version, have or "none"))
        return

    manifest_url = origin + "updates/client/latest.json"
    manifest = json.loads(get(manifest_url))
    assets = manifest.get("assets") or sys.exit("assets_cdn: %s has no assets section" % manifest_url)
    list_url = urllib.parse.urljoin(manifest_url, assets["manifest"])
    objects_url = urllib.parse.urljoin(manifest_url, assets["objects"])
    raw = get(list_url)
    if hashlib.sha256(raw).hexdigest() != assets["sha256"]:
        sys.exit("assets_cdn: %s does not match the manifest's sha256" % list_url)
    files = json.loads(raw)["files"]
    packaged = assets.get("packaged") or []
    excluded = lambda p: any(p.startswith(x) for x in args.exclude)

    for path, sha, size in files:
        if not PATH_RE.match(path) or ".." in path.split("/") or not HEX_RE.match(sha):
            sys.exit("assets_cdn: bad entry %r in %s" % (path, list_url))
    for path in packaged:
        if not PATH_RE.match(path) or ".." in path.split("/"):
            sys.exit("assets_cdn: bad packaged path %r" % path)

    files = [f for f in files if not excluded(f[0])]
    packaged = [p for p in packaged if not excluded(p)]
    big = [f[0] for f in files if f[2] > MAX_SIZE]
    count = len(files) + len(packaged) + 1  # + version.txt (_headers is not a file of the site)
    if count > args.max_files or big:
        sys.exit("assets_cdn: over the Pages limits: %d files (max %d), over 25 MiB: %s. Leave prefixes out with "
                 "--exclude (KN_ASSET_CDN_EXCLUDE; the page's asset_fallback serves them), or switch the page "
                 "to the own origin (README \"Asset CDN\")" % (count, args.max_files, big[:10]))

    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    tree = os.path.join(args.out, "assets")
    os.makedirs(tree)
    t0 = time.time()
    fetched = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        jobs = {pool.submit(fetch_object, objects_url, args.objects, sha, size): path for path, sha, size in files}
        for n, job in enumerate(concurrent.futures.as_completed(jobs), 1):
            obj, got = job.result()
            fetched += got
            link(obj, os.path.join(tree, jobs[job]))
            if n % 2000 == 0:
                log("%d/%d files, %.0f MB downloaded" % (n, len(files), fetched / 1e6))
    missing = []
    for path in packaged:
        try:
            data = get(origin + "assets/" + path + "?v=" + version)
        except urllib.error.HTTPError as e:
            if e.code != 404:
                raise
            missing.append(path)
            continue
        dst = os.path.join(tree, path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
    if missing:
        log("packaged files the origin does not have (left out): %s" % missing)

    # The origin's tree must not have moved on meanwhile: this version then names exactly it.
    if version_of(origin) != version:
        sys.exit("assets_cdn: the origin's assets/version.txt changed during the mirror; run again")
    with open(os.path.join(tree, "version.txt"), "w") as f:
        f.write(version + "\n")
    with open(os.path.join(args.out, "_headers"), "w") as f:
        f.write(HEADERS)

    total = sum(os.path.getsize(os.path.join(d, n)) for d, _ds, fs in os.walk(tree) for n in fs)
    nfiles = sum(len(fs) for _d, _ds, fs in os.walk(tree))
    log("staged %s: version %s, %d files (%d packaged), %.2f GB, %.0f MB downloaded in %.0f s%s" % (
        args.out, version, nfiles, len(packaged) - len(missing), total / 1e9, fetched / 1e6, time.time() - t0,
        ", excluded " + " ".join(args.exclude) if args.exclude else ""))


if __name__ == "__main__":
    main()
