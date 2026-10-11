#!/usr/bin/env python3
"""Stage dist/web for Cloudflare Pages (deploy/pages/README.md).

Copies the web build without assets/ (game content stays on the asset host: Pages caps
a deployment at 20k files / 25 MiB each), renames client.{js,wasm,data} to
client.<hash>.* (one content hash over the three, so a page can never pair a stale
client.js with a new wasm), fills in the loader.js <meta> knobs of every page and writes
_headers (immutable hashed files; Pages revalidates everything else), _redirects
(/assets/* -> the asset host) and 404.html (unknown paths are 404s, not the SPA fallback). --ga-id puts Google Analytics into every page (ga.py).

usage: stage.py <dist/web> <out dir> --asset-base URL --brain URL [--bridge URL] [--wt URL|off] [--ga-id G-...|off]
"""
import argparse
import hashlib
import html
import os
import re
import shutil
import sys

import ga

CLIENT_FILES = ("client.js", "client.wasm", "client.data")
KNOBS = ("kn-build", "kn-asset-base", "kn-brain", "kn-bridge", "kn-wt")

# Pages' default for everything else (pages, js/, css/, img/, windows/) is
# "public, max-age=0, must-revalidate" + ETag: always revalidated, never stale.
HEADERS_HEAD = "# deploy/pages/stage.py: content-hashed client files never change\n"


NOT_FOUND_PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>404 Not Found</title></head>
<body style="background:#000;color:#fff;font-family:sans-serif;text-align:center;margin-top:20vh">
<h1>404 Not Found</h1><p><a href="/" style="color:#2196F3">krunk.io</a></p></body></html>
"""


def set_meta(page, name, value):
    tag = '<meta name="%s" content="%s">' % (name, html.escape(value, quote=True))
    pattern = re.compile(r'<meta\s+name="%s"[^>]*>' % re.escape(name), re.I)
    if pattern.search(page):
        return pattern.sub(lambda _m: tag, page, count=1)
    anchor = re.search(r"<meta\s+charset[^>]*>", page, re.I) or re.search(r"<head[^>]*>", page, re.I)
    if not anchor:
        sys.exit("stage.py: page without <head>")
    return page[:anchor.end()] + "\n" + tag + page[anchor.end():]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dist")
    ap.add_argument("out")
    ap.add_argument("--asset-base", default="")
    ap.add_argument("--brain", default="")
    ap.add_argument("--bridge", default="", help='net-bridge /native WebSocket URL; "" = same origin')
    ap.add_argument("--wt", default="off", help='WebTransport relay URL; "off" = WebSocket only (no wt.json fetch)')
    ap.add_argument("--ga-id", default="off", help='GA4 measurement id (KN_GA_ID); "" or "off" = no analytics')
    args = ap.parse_args()
    ga_id = ga.ga_id(args.ga_id)

    dist = os.path.abspath(args.dist)
    for name in CLIENT_FILES:
        if not os.path.isfile(os.path.join(dist, name)):
            sys.exit("stage.py: %s/%s missing: run `make web` first" % (dist, name))

    if os.path.exists(args.out):
        shutil.rmtree(args.out)
    shutil.copytree(dist, args.out, symlinks=False,
                    ignore=lambda d, names: ["assets"] if os.path.samefile(d, dist) and "assets" in names else [])

    digest = hashlib.sha256()
    for name in CLIENT_FILES:
        with open(os.path.join(args.out, name), "rb") as f:
            digest.update(name.encode() + b"\0" + f.read())
    build = digest.hexdigest()[:16]
    for name in CLIENT_FILES:
        stem, ext = name.split(".", 1)
        os.rename(os.path.join(args.out, name), os.path.join(args.out, "%s.%s.%s" % (stem, build, ext)))

    values = {"kn-build": build, "kn-asset-base": args.asset_base, "kn-brain": args.brain, "kn-bridge": args.bridge,
              "kn-wt": args.wt}
    pages = 0
    for name in sorted(os.listdir(args.out)):
        if not name.endswith(".html"):
            continue
        path = os.path.join(args.out, name)
        with open(path, encoding="utf-8") as f:
            page = f.read()
        if "js/loader.js" not in page:
            continue
        for knob in KNOBS:
            page = set_meta(page, knob, values[knob])
        with open(path, "w", encoding="utf-8") as f:
            f.write(page)
        pages += 1
    ga.inject_dir(args.out, ga_id)

    with open(os.path.join(args.out, "_headers"), "w") as f:
        f.write(HEADERS_HEAD)
        for name in CLIENT_FILES:
            stem, ext = name.split(".", 1)
            f.write("/%s.%s.%s\n  Cache-Control: public, max-age=31536000, immutable\n" % (stem, build, ext))

    # A missing file must be a 404, not a 200 index.html: without a top-level 404.html Pages
    # serves the site as an SPA (index.html for every unknown path), so a missing <img> (e.g.
    # an item preview the page asked this origin for) "loads" HTML instead of failing.
    with open(os.path.join(args.out, "404.html"), "w", encoding="utf-8") as f:
        f.write(NOT_FOUND_PAGE)

    # Anything still asking this origin for /assets/ (an old page, a relative URL) goes to
    # the asset host; 302 so the target can move.
    if args.asset_base:
        with open(os.path.join(args.out, "_redirects"), "w") as f:
            f.write("/assets/* %sassets/:splat 302\n" % args.asset_base)

    files = sum(len(fs) for _d, _ds, fs in os.walk(args.out))
    big = [os.path.join(d, n) for d, _ds, fs in os.walk(args.out) for n in fs
           if os.path.getsize(os.path.join(d, n)) > 25 * 1024 * 1024]
    if files > 20000 or big:
        sys.exit("stage.py: over the Pages limits (%d files, too large: %s)" % (files, big))
    print("staged %s: build %s, %d pages, %d files, analytics %s" % (args.out, build, pages, files, ga_id or "off"))


if __name__ == "__main__":
    main()
