#!/usr/bin/env python3
"""Mirror the game content of the kru download host into one directory per Pages project of the
asset CDN (deploy/pages/README.md "Asset CDN"; deploy/pages/assets-cdn.sh deploys them).

The tree is exactly what the native client gets: the launcher's content index (the updates
manifest <origin>updates/client/latest.json -> assets.manifest, every file of the web asset
tree except the ones the packages ship, as sha256-addressed objects) plus those packaged
files, fetched by path. Objects are verified and kept in --objects (CI caches the directory),
so only new content is downloaded. Pages takes at most 20,000 files of at most 25 MiB in a
deployment (Free plan), so the tree is split by directory over the projects of
deploy/asset-shards.json (asset_shards.py). Out, per shard <name>:

    <out>/<name>/assets/<path>       its part of the tree (hardlinks into --objects; wrangler
                                     skips symlinks)
    <out>/<name>/assets/version.txt  the origin's assets/version.txt: the ?v= of the web client
    <out>/<name>/_headers            CORS for any page origin, caching (README "Asset CDN")
    <out>/<default>/_redirects       the other shards' directories (301), the kru-only paths and
                                     the excluded prefixes (302 to the download host)

Prefixes in the file's "exclude" (and --exclude) go to no project. A shard over the Pages limits
fails the run instead of deploying a partial tree; over the file's max_files it only warns.

usage: assets_cdn.py <out> [--config FILE] [--origin URL] [--objects DIR] [--jobs N]
                     [--exclude PREFIX ...] [--max-files N] [--dry-run] [--current]
       --origin: default the file's "download" (kru, https://dl.krunk.io/)
       --dry-run: read the origin's content list, print the counts per shard and directory and
                  check the limits; nothing is downloaded or written (<out> is not touched).
       --current: print "same" and exit 0 when every shard's pages.dev assets/version.txt already
                  equals the origin's (nothing to deploy), else "changed".
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
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python3 -I: asset_shards next to this file
import asset_shards  # noqa: E402

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


def top_prefixes(files, packaged, limit):
    """(prefix, files, bytes) of the biggest two-level directories left, most files first."""
    groups = {}
    for path, size in [(f[0], f[2]) for f in files] + [(p, 0) for p in packaged]:
        parts = path.split("/")
        key = "/".join(parts[:2]) + "/" if len(parts) > 2 else (parts[0] + "/" if len(parts) > 1 else path)
        n, b = groups.get(key, (0, 0))
        groups[key] = (n + 1, b + size)
    return sorted(((k, n, b) for k, (n, b) in groups.items()), key=lambda g: (-g[1], g[0]))[:limit]


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
    # one temp file per call: workers fetching the same object (same bytes, same sha) must not share it
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(obj), prefix=os.path.basename(obj) + ".tmp")
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.replace(tmp, obj)
    return obj, size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--config", default=asset_shards.CONFIG, help="deploy/asset-shards.json")
    ap.add_argument("--origin", default="", help="the source (default: the file's download host)")
    ap.add_argument("--objects", default=os.path.expanduser("~/.cache/kn-assets-objects"))
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--exclude", action="append", default=[], help="path prefix below assets/ on no project")
    ap.add_argument("--max-files", type=int, default=20000, help="the Pages limit per project")
    ap.add_argument("--dry-run", action="store_true", help="count and check the limits only")
    ap.add_argument("--current", action="store_true", help="exit early when every shard has this version")
    args = ap.parse_args()
    try:
        with open(args.config) as f:
            cfg = json.load(f)
        cfg["exclude"] = list(dict.fromkeys(list(cfg.get("exclude", [])) + [x for x in args.exclude if x]))
        shards = asset_shards.Shards(cfg)
    except (OSError, ValueError) as e:
        sys.exit("assets_cdn: %s: %s" % (args.config, e))
    origin = args.origin or shards.download
    origin = origin if origin.endswith("/") else origin + "/"

    version = version_of(origin)
    if args.current:
        have = {}
        for s in shards.shards:
            try:
                have[s["name"]] = version_of(s["pages_dev"])
            except (OSError, ValueError, SystemExit, urllib.error.URLError):
                have[s["name"]] = ""
        same = all(v == version for v in have.values())
        print("same" if same else "changed")
        log("origin %s %s, %s" % (origin, version, ", ".join("%s %s" % (n, v or "none") for n, v in have.items())))
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

    for path, sha, size in files:
        if not PATH_RE.match(path) or ".." in path.split("/") or not HEX_RE.match(sha):
            sys.exit("assets_cdn: bad entry %r in %s" % (path, list_url))
    for path in packaged:
        if not PATH_RE.match(path) or ".." in path.split("/"):
            sys.exit("assets_cdn: bad packaged path %r" % path)

    # every path to its shard (None: excluded, served by the download host only)
    plan = {s["name"]: ([], []) for s in shards.shards}
    excluded = {}
    for f in files:
        s = shards.route(f[0])
        if s:
            plan[s["name"]][0].append(f)
        else:
            excluded[f[0]] = 1
    for path in packaged:
        s = shards.route(path)
        if s:
            plan[s["name"]][1].append(path)
        else:
            excluded[path] = 1
    total = len(files) + len(packaged) + 1  # + version.txt (_headers / _redirects are not files of the site)
    log("content list %s from %s: %d files (%d indexed + %d packaged + version.txt)" % (
        assets.get("version", "?"), origin, total, len(files), len(packaged)))
    for x in shards.exclude:
        n = sum(1 for p in excluded if p.startswith(x))
        log("  excluded %-32s %6d files%s" % (x, n, "" if n else "  (matches nothing: a typo?)"))
    over = []
    for s in shards.shards:
        sfiles, spack = plan[s["name"]]
        count = len(sfiles) + len(spack) + 1
        big = [f[0] for f in sfiles if f[2] > MAX_SIZE]
        log("shard %s (%s, %s): %d files, %.2f GB, max %d: %s%s" % (
            s["name"], s["project"], s["url"], count, sum(f[2] for f in sfiles) / 1e9, args.max_files,
            "%d to spare" % (args.max_files - count) if count <= args.max_files else "%d too many" % (count - args.max_files),
            ", over 25 MiB: %s" % big[:10] if big else ""))
        if args.dry_run:
            for prefix, n, size in top_prefixes(sfiles, spack, 12):
                log("  %6d files %8.1f MB  %s" % (n, size / 1e6, prefix))
        if count > args.max_files or big:
            over.append("%s: %d files (max %d), over 25 MiB: %s; biggest: %s" % (
                s["name"], count, args.max_files, big[:10],
                ", ".join("%s %d" % (p, n) for p, n, _s in top_prefixes(sfiles, spack, 6))))
        elif count > shards.max_files:
            msg = "shard %s has %d files, over the %d of deploy/asset-shards.json (Pages max %d): move a directory " \
                  "to another shard" % (s["name"], count, shards.max_files, args.max_files)
            print(("::warning::assets_cdn: " if os.environ.get("GITHUB_ACTIONS") else "assets_cdn: warning: ") + msg,
                  file=sys.stderr, flush=True)
    if over:
        sys.exit("assets_cdn: still over the Pages limits: %s. Move directories between the shards (or add one) in "
                 "deploy/asset-shards.json, or exclude them there (README \"Asset CDN\")" % "; ".join(over))
    if args.dry_run:
        return

    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    t0 = time.time()
    fetched = 0
    jobs_files = [(s["name"], f) for s in shards.shards for f in plan[s["name"]][0]]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        jobs = {pool.submit(fetch_object, objects_url, args.objects, f[1], f[2]): (name, f[0]) for name, f in jobs_files}
        for n, job in enumerate(concurrent.futures.as_completed(jobs), 1):
            obj, got = job.result()
            fetched += got
            name, path = jobs[job]
            link(obj, os.path.join(args.out, name, "assets", path))
            if n % 2000 == 0:
                log("%d/%d files, %.0f MB downloaded" % (n, len(jobs_files), fetched / 1e6))
    missing = []
    for s in shards.shards:
        for path in plan[s["name"]][1]:
            try:
                data = get(origin + "assets/" + path + "?v=" + version)
            except urllib.error.HTTPError as e:
                if e.code != 404:
                    raise
                missing.append(path)
                continue
            dst = os.path.join(args.out, s["name"], "assets", path)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(data)
    if missing:
        log("packaged files the origin does not have (left out): %s" % missing)

    # The origin's tree must not have moved on meanwhile: this version then names exactly it.
    if version_of(origin) != version:
        sys.exit("assets_cdn: the origin's assets/version.txt changed during the mirror; run again")
    for s in shards.shards:
        root = os.path.join(args.out, s["name"])
        os.makedirs(os.path.join(root, "assets"), exist_ok=True)
        with open(os.path.join(root, "assets", "version.txt"), "w") as f:
            f.write(version + "\n")
        with open(os.path.join(root, "_headers"), "w") as f:
            f.write(HEADERS)
        if s is shards.default:
            with open(os.path.join(root, "_redirects"), "w") as f:
                f.write(shards.redirects())
        tree = os.path.join(root, "assets")
        size = sum(os.path.getsize(os.path.join(d, n)) for d, _ds, fs in os.walk(tree) for n in fs)
        nfiles = sum(len(fs) for _d, _ds, fs in os.walk(tree))
        log("staged %s: shard %s (%s), version %s, %d files, %.2f GB" % (root, s["name"], s["project"], version, nfiles, size / 1e9))
    log("%.0f MB downloaded in %.0f s, %d files on no shard" % (fetched / 1e6, time.time() - t0, len(excluded)))


if __name__ == "__main__":
    main()
