#!/usr/bin/env python3
"""Stage dist/web for GitHub Pages or Cloudflare Pages (deploy/pages/README.md).

Copies the web build without assets/ (game content stays on the asset origins: see
README "Asset origins"), renames client.{js,wasm,data} to client.<hash>.* (one content
hash over the three, so a page can never pair a stale client.js with a new wasm), fills in
the loader.js <meta> knobs of every page and writes 404.html (unknown paths are 404s, not
an SPA fallback). --ga-id puts Google Analytics into every page (ga.py).

--asset-origins name=URL,... with --asset-origin NAME (and --asset-fallback NAME) writes
kn-config.json, which loader.js reads before boot: the asset origin is then a run-time
setting of the published site (no client build). Without them only kn-asset-base is set.

--target cloudflare: _headers (immutable hashed files; Pages revalidates everything else)
  and _redirects (/assets/* -> the asset origin); Pages limits 20k files / 25 MiB each.
--target github: no _headers / _redirects (GitHub Pages ignores both and serves every file
  with max-age=600); a CNAME file (--cname; informational, a workflow deployment takes the
  custom domain from the repository's Pages settings); --keep-from URL also ships the
  client.<hash>.* files of the build that site serves now, so a page a browser still has
  cached for those 10 minutes finds its client after the deploy. Limits 1 GB, 100 MB a file.

usage: stage.py <dist/web> <out dir> --brain URL [--asset-base URL | --asset-origins n=URL,... --asset-origin n
                [--asset-fallback n]] [--bridge URL] [--rtc URL|off] [--wt URL|off] [--ga-id G-...|off]
                [--target cloudflare|github] [--cname HOST] [--keep-from URL]
"""
import argparse
import hashlib
import html
import json
import os
import re
import shutil
import sys
import urllib.request

import ga

CLIENT_FILES = ("client.js", "client.wasm", "client.data")
KNOBS = ("kn-build", "kn-asset-base", "kn-config", "kn-brain", "kn-bridge", "kn-rtc", "kn-wt")
CONFIG_FILE = "kn-config.json"
NAME_RE = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
URL_RE = re.compile(r"^https?://[^\s\"'<>]+/$")
LIMITS = {"cloudflare": (20000, 25 * 1024 * 1024, None), "github": (None, 100 * 1000 * 1000, 1000 * 1000 * 1000)}

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


def asset_config(args):
    """(kn-asset-base URL, kn-config.json dict or None) from the asset options."""
    if not args.asset_origins:
        return args.asset_base, None
    origins = {}
    for item in args.asset_origins.split(","):
        name, sep, url = item.strip().partition("=")
        url = url.strip()
        if not url:  # name= : an origin that is not set up (yet)
            continue
        url = url if url.endswith("/") else url + "/"
        if not sep or not NAME_RE.match(name) or not URL_RE.match(url):
            sys.exit("stage.py: --asset-origins entry %r is not name=https://host/path/" % item)
        origins[name] = url
    if args.asset_origin not in origins:
        sys.exit("stage.py: --asset-origin %r is not one of %s" % (args.asset_origin, sorted(origins)))
    fallback = args.asset_fallback or ""
    if fallback and fallback not in origins:
        sys.exit("stage.py: --asset-fallback %r is not one of %s" % (fallback, sorted(origins)))
    if fallback == args.asset_origin:
        fallback = ""
    return origins[args.asset_origin], {"asset_origin": args.asset_origin, "asset_fallback": fallback,
                                         "asset_origins": origins}


def fetch(url, timeout):
    # urllib's default User-Agent is refused by Cloudflare's bot filter (403)
    req = urllib.request.Request(url, headers={"User-Agent": "KrunkNative-pages-stage/1"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def keep_previous(out, build, live):
    """Copy the client.<hash>.* files the live site's index.html names (another build than
    this one) into out; a missing or unreachable site is not an error."""
    try:
        page = fetch(live, 30).decode("utf-8", "replace")
        m = re.search(r'<meta\s+name="kn-build"\s+content="([0-9a-f]{16})"', page)
        if not m or m.group(1) == build:
            print("stage.py: --keep-from %s: %s" % (live, "same build" if m else "no kn-build"))
            return
        old = m.group(1)
        for name in CLIENT_FILES:
            stem, ext = name.split(".", 1)
            file = "%s.%s.%s" % (stem, old, ext)
            data = fetch(live.rstrip("/") + "/" + file, 120)
            with open(os.path.join(out, file), "wb") as f:
                f.write(data)
        print("stage.py: kept the live build %s's client files" % old)
    except (OSError, ValueError) as e:
        print("stage.py: --keep-from %s: %s (not kept)" % (live, e))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dist")
    ap.add_argument("out")
    ap.add_argument("--asset-base", default="")
    ap.add_argument("--brain", default="")
    ap.add_argument("--bridge", default="", help='net-bridge /native WebSocket URL; "" = same origin')
    ap.add_argument("--rtc", default="off", help='WebRTC gateway signaling URL (net-bridge/rtc); "off" = no WebRTC (no rtc.json fetch)')
    ap.add_argument("--wt", default="off", help='WebTransport relay URL; "off" = WebSocket only (no wt.json fetch)')
    ap.add_argument("--ga-id", default="off", help='GA4 measurement id (KN_GA_ID); "" or "off" = no analytics')
    ap.add_argument("--asset-origins", default="", help="name=URL,... the asset origins kn-config.json offers")
    ap.add_argument("--asset-origin", default="", help="the one of them the page uses")
    ap.add_argument("--asset-fallback", default="", help='the one a failed download is retried on; "" = none')
    ap.add_argument("--target", choices=("cloudflare", "github"), default="cloudflare")
    ap.add_argument("--cname", default="", help="github: custom domain written to CNAME")
    ap.add_argument("--keep-from", default="", help="github: the live site, whose client files are kept")
    args = ap.parse_args()
    ga_id = ga.ga_id(args.ga_id)
    asset_base, config = asset_config(args)

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

    values = {"kn-build": build, "kn-asset-base": asset_base, "kn-config": CONFIG_FILE if config else "",
              "kn-brain": args.brain, "kn-bridge": args.bridge, "kn-rtc": args.rtc, "kn-wt": args.wt}
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

    if config:
        with open(os.path.join(args.out, CONFIG_FILE), "w") as f:
            json.dump(config, f, indent=1, sort_keys=True)
            f.write("\n")

    if args.target == "cloudflare":
        with open(os.path.join(args.out, "_headers"), "w") as f:
            f.write(HEADERS_HEAD)
            for name in CLIENT_FILES:
                stem, ext = name.split(".", 1)
                f.write("/%s.%s.%s\n  Cache-Control: public, max-age=31536000, immutable\n" % (stem, build, ext))
        # Anything still asking this origin for /assets/ (an old page, a relative URL) goes to
        # the asset origin; 302 so the target can move.
        if asset_base:
            with open(os.path.join(args.out, "_redirects"), "w") as f:
                f.write("/assets/* %sassets/:splat 302\n" % asset_base)
    else:
        if args.cname:
            with open(os.path.join(args.out, "CNAME"), "w") as f:
                f.write(args.cname + "\n")
        if args.keep_from:
            keep_previous(args.out, build, args.keep_from)

    # A missing file must be a 404, not a 200 index.html: without a top-level 404.html Pages
    # serves the site as an SPA (index.html for every unknown path), so a missing <img> (e.g.
    # an item preview the page asked this origin for) "loads" HTML instead of failing.
    # GitHub Pages answers unknown paths with it as well (status 404).
    with open(os.path.join(args.out, "404.html"), "w", encoding="utf-8") as f:
        f.write(NOT_FOUND_PAGE)

    max_files, max_size, max_total = LIMITS[args.target]
    sizes = [os.path.getsize(os.path.join(d, n)) for d, _ds, fs in os.walk(args.out) for n in fs]
    big = [os.path.join(d, n) for d, _ds, fs in os.walk(args.out) for n in fs
           if os.path.getsize(os.path.join(d, n)) > max_size]
    if (max_files and len(sizes) > max_files) or big or (max_total and sum(sizes) > max_total):
        sys.exit("stage.py: over the %s Pages limits (%d files, %d bytes, too large: %s)" % (args.target, len(sizes), sum(sizes), big))
    print("staged %s for %s: build %s, %d pages, %d files, %.1f MB, assets %s%s, analytics %s" % (
        args.out, args.target, build, pages, len(sizes), sum(sizes) / 1e6, asset_base or "(this origin)",
        " (kn-config.json: %s, fallback %s)" % (config["asset_origin"], config["asset_fallback"] or "none") if config else "",
        ga_id or "off"))


if __name__ == "__main__":
    main()
