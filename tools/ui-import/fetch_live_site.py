#!/usr/bin/env python3
"""Mirror the live krunker.io site (the Krunker v10 menu) into a staging ``public/`` tree.

``import.py --ref v10 --public <stage>/public`` turns it into ``ui/v10/``. The live site is split
over two hosts: ``https://krunker.io/`` serves the pages, stylesheets, fonts and part of
``img/``; ``https://assets.krunker.io/`` (v10's ``assetsUrl``, also the bulk of ``img/``) the rest.
The page rewrites asset-host URLs to the same relative paths, so both land in one tree:
a path is asked of the host the reference names first, then of the other one.

Fetched: ``index.html``, ``social.html``, ``editor.html``, ``viewer.html``, ``docs/versions.txt``,
every stylesheet / font / image / sound / video those pages and their stylesheets reference
(followed recursively through CSS), every ``img/`` literal of the client script (``--client-js``:
read as text and grepped, never executed; ``.js`` files are never downloaded), the C UI's own
image list (``--manifest``, e.g. menu-assets-v7.txt) and the paths of ``--extra`` (one per line).

usage:
  python3 -I tools/ui-import/fetch_live_site.py --stage DIR [--client-js v10.js] [--manifest F] [--extra F]

writes <stage>/public/<path> and <stage>/site.lock (url, path, status, sha256, size, UTC date;
resumable: paths already 200 in the lock are not asked again). Environment as
fetch_live_assets.py: KN_FETCH_PROXY, KN_FETCH_UA, KN_FETCH_JOBS (<= 4), KN_FETCH_DELAY.
"""

import argparse
import concurrent.futures
import datetime
import hashlib
import os
import re
import sys
import threading
import urllib.parse

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetch_live_assets as fla  # noqa: E402  (Fetcher: proxy, browser headers, retries, validate)
import menu_assets  # noqa: E402  (manifest_paths: brace expansion of menu-assets*.txt)

SITE = "https://krunker.io/"
ASSETS = "https://assets.krunker.io/"
PAGES = ("index.html", "social.html", "editor.html", "viewer.html", "docs/versions.txt")
REF_RES = (re.compile(r"""(?:src|href)\s*=\s*["']([^"']+)["']""", re.I),
           re.compile(r"""url\(\s*(?:&quot;|["'])?([^)"'&]+?)(?:&quot;|["'])?\s*\)""", re.I),
           re.compile(r"""@import\s+["']([^"']+)["']""", re.I))
FETCH_EXT = (".css", ".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".ico", ".ttf", ".woff", ".woff2", ".otf",
             ".mp3", ".ogg", ".wav", ".mp4", ".webm", ".txt", ".html")
# client-script literals naming page images: "./img/x.png", "/img/x.png", "//assets.krunker.io/img/x.png"
JS_IMG_RE = re.compile(r"""(?:["'`]|//assets\.krunker\.io/|\.\.?/)(img/[A-Za-z0-9_\-./]+\.(?:png|svg|gif|jpe?g|webp))\b""")


def page_path(base_path, base_host, ref):
    """(path, preferred host) of a reference, or None when it is not ours to fetch."""
    ref = ref.strip().replace("&amp;", "&")
    if not ref or ref.startswith(("data:", "javascript:", "#", "mailto:")) or "${" in ref or "+" in ref:
        return None
    url = urllib.parse.urljoin(base_host + base_path, ref)
    u = urllib.parse.urlsplit(url)
    if u.netloc not in ("krunker.io", "assets.krunker.io"):
        return None
    path = os.path.normpath(urllib.parse.unquote(u.path).lstrip("/")).replace(os.sep, "/")
    if path.startswith("..") or path == "." or not path.lower().endswith(FETCH_EXT):
        return None
    return path, (ASSETS if u.netloc == "assets.krunker.io" else SITE)


class Mirror:
    def __init__(self, stage):
        self.public = os.path.join(stage, "public")
        self.lock_path = os.path.join(stage, "site.lock")
        self.fetcher = fla.Fetcher()
        self.rows = {}  # path -> (url, status, sha256, size, date)
        self.mutex = threading.Lock()
        if os.path.exists(self.lock_path):
            with open(self.lock_path, encoding="utf8") as f:
                for line in f:
                    if line.startswith("#") or not line.strip():
                        continue
                    url, path, status, digest, size, when = line.rstrip("\n").split("\t")
                    self.rows[path] = (url, status, digest, int(size), when)

    def get(self, item):
        """(path, host the reference came from, bytes or None)."""
        path, host = item
        row = self.rows.get(path)
        dest = os.path.join(self.public, path)
        if row and row[1] == "200" and os.path.isfile(dest):
            with open(dest, "rb") as fh:
                return path, row[0][:row[0].index("/", 8) + 1], fh.read()
        tried = []
        for h in (host, ASSETS if host == SITE else SITE):
            url = h + urllib.parse.quote(path)
            status, data, _, note = self.fetcher.get(url, path)
            ok = status == 200 and bool(data)
            if ok and not path.endswith((".html", ".txt", ".css")):
                ok, note = fla.validate(path, data)
            if ok:
                fla.write_atomic(dest, data)
                self.record(path, url, "200", hashlib.sha256(data).hexdigest(), len(data))
                print("200 %s" % url, flush=True)
                return path, h, data
            tried.append((url, status if status != 200 else "invalid"))
        self.record(path, tried[0][0], str(tried[-1][1]), "", 0)
        print("%s %s (%s)" % (tried[-1][1], path, " / ".join("%s %s" % t for t in tried)), flush=True)
        return path, host, None

    def record(self, path, url, status, digest, size):
        when = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        with self.mutex:
            self.rows[path] = (url, status, digest, size, when)

    def write_lock(self):
        with open(self.lock_path, "w", encoding="utf8") as f:
            f.write("# live krunker.io site mirror (tools/ui-import/fetch_live_site.py)\n"
                    "# url\tpath\tstatus\tsha256\tsize\tdate\n")
            for path in sorted(self.rows):
                url, status, digest, size, when = self.rows[path]
                f.write("%s\t%s\t%s\t%s\t%d\t%s\n" % (url, path, status, digest, size, when))


def refs_of(path, host, data):
    if not path.endswith((".html", ".css")):
        return []
    text = data.decode("utf8", "replace")
    out = []
    for rx in REF_RES:
        for m in rx.finditer(text):
            r = page_path(path, host, m.group(1))
            if r:
                out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--stage", required=True, help="staging dir (public/ + site.lock)")
    ap.add_argument("--client-js", help="the live client script, grepped (as text) for img/ literals")
    ap.add_argument("--manifest", help="menu-assets*.txt: the C UI's image paths")
    ap.add_argument("--extra", help="more paths (relative to the site root), one per line")
    a = ap.parse_args()
    mirror = Mirror(os.path.abspath(a.stage))
    queue = [(p, SITE) for p in PAGES]
    if a.client_js:
        with open(a.client_js, encoding="utf8", errors="replace") as f:
            queue += [(p, ASSETS) for p in sorted(set(JS_IMG_RE.findall(f.read())))]
    if a.manifest:
        queue += [(p, ASSETS) for p in menu_assets.manifest_paths(a.manifest) if not p.endswith(".css")]
    if a.extra:
        with open(a.extra, encoding="utf8") as f:
            queue += [(l.strip().lstrip("/"), ASSETS) for l in f if l.strip() and not l.startswith("#")]
    seen, uniq = set(), []
    for item in queue:
        if item[0] not in seen:
            seen.add(item[0])
            uniq.append(item)
    queue = uniq
    jobs = max(1, min(4, int(os.environ.get("KN_FETCH_JOBS", "3"))))
    try:
        while queue:
            nxt = []
            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
                for path, host, data in ex.map(mirror.get, queue):
                    for r in (refs_of(path, host, data) if data else ()):
                        if r[0] not in seen:
                            seen.add(r[0])
                            nxt.append(r)
            queue = nxt
    finally:
        mirror.write_lock()
    ok = [r for r in mirror.rows.values() if r[1] == "200"]
    print("site: %d paths, %d fetched (%d bytes), %d missing on both hosts" % (
        len(mirror.rows), len(ok), sum(r[3] for r in ok), len(mirror.rows) - len(ok)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
