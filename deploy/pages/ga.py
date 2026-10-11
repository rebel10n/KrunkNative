#!/usr/bin/env python3
"""Google Analytics (gtag.js) for the web build (deploy/pages/README.md "Analytics").

The ui/ pages stay free of analytics (the native HTML engine renders them and ignores
<script>); the tag is put in at web build / deploy time only: `make web KN_GA_ID=...`
on dist/web, deploy/pages/stage.py on the Pages copy (deploy.sh default G-6EGYKRF020).

Every HTML document under <dir> (a file with a <head> element; ui/windows/ templates are
fragments and are skipped) gets Google's snippet as the first child of <head>. Idempotent:
an existing snippet (any id) is removed first, so rerunning never doubles it and an empty
or "off" id strips it.

usage: ga.py <dir> [--id G-XXXX]   (default: $KN_GA_ID)
"""
import argparse
import os
import re
import sys

SNIPPET = """<!-- Google tag (gtag.js) -->
<script async src="https://www.googletagmanager.com/gtag/js?id=%(id)s"></script>
<script>
  window.dataLayer = window.dataLayer || [];
  function gtag(){dataLayer.push(arguments);}
  gtag('js', new Date());

  gtag('config', '%(id)s');
</script>
"""
ID_RE = re.compile(r"^G-[A-Z0-9]+$")
HEAD_RE = re.compile(r"<head(?:\s[^>]*)?>", re.I)
# the snippet above with any id, plus the newline it is inserted with
EXISTING_RE = re.compile(
    r"<!-- Google tag \(gtag\.js\) -->\s*"
    r'<script async src="https://www\.googletagmanager\.com/gtag/js\?id=[^"]*"></script>\s*'
    r"<script>\s*window\.dataLayer = .*?</script>\n?", re.S)

def ga_id(value):
    """The validated measurement id, or "" when analytics is off."""
    value = (value or "").strip()
    if value.lower() in ("", "off"):
        return ""
    if not ID_RE.match(value):
        sys.exit("ga.py: KN_GA_ID %r is not a GA4 measurement id (G-XXXXXXXX), or empty/off" % value)
    return value


def inject(page, gid):
    """page with exactly one snippet for gid right after <head> (none when gid is "")."""
    page = EXISTING_RE.sub("", page)
    head = HEAD_RE.search(page)
    if not gid or not head:
        return page
    at = head.end()
    lead = ""
    if page.startswith("\r\n", at):
        at += 2
    elif page.startswith("\n", at):
        at += 1
    else:
        lead = "\n"
    return page[:at] + lead + SNIPPET % {"id": gid} + page[at:]


def inject_dir(root, gid):
    """Apply inject() to every HTML document under root; returns the number of pages tagged."""
    tagged = 0
    for d, _dirs, files in os.walk(root):
        for name in sorted(files):
            if not name.lower().endswith((".html", ".htm")):
                continue
            path = os.path.join(d, name)
            if os.path.islink(path):  # dist/web/assets is a symlink tree: never write through it
                continue
            with open(path, encoding="utf-8", newline="") as f:
                page = f.read()
            if not HEAD_RE.search(page):
                continue
            new = inject(page, gid)
            if new != page:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(new)
            tagged += bool(gid)
    return tagged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--id", default=os.environ.get("KN_GA_ID", ""), help='GA4 measurement id; "" or "off" = none')
    args = ap.parse_args()
    gid = ga_id(args.id)
    n = inject_dir(args.dir, gid)
    print("ga.py: %s" % ("gtag %s in %d pages of %s" % (gid, n, args.dir) if gid else "analytics off (%s)" % args.dir))


if __name__ == "__main__":
    main()
