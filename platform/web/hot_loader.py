#!/usr/bin/env python3
"""Inlines the web hot loader (platform/web/hot-loader.js, doc/web-hot-loader.md) into the assembled
web build's game page, as the first script of <head>.

  hot_loader.py DIST_DIR SHARED_H

Runs from CMake's web_dist target after ui/ is copied (and after pwa.py). ui/index.html is not
touched: the native client's page has no script, and the hot loader is the web build's network
glue. The script goes right after <meta charset>, so the deploy's <meta name="kn-*"> knobs, which
deploy/pages/stage.py also puts right after <meta charset>, come before it and it can read them.
@KN_PROTOCOL@ becomes this build's NET_PROTOCOL_MAJOR.MINOR (shared/include/shared.h), the seek's
proto (the brain never matches a lobby of another protocol major). Re-running replaces the block.
"""
import os
import re
import sys

BEGIN, END = "<!-- kn-hot-loader -->", "<!-- /kn-hot-loader -->"


def protocol(shared_h):
    with open(shared_h, encoding="utf-8") as f:
        text = f.read()
    major = re.search(r"#define NET_PROTOCOL_MAJOR (\d+)", text)
    minor = re.search(r"#define NET_PROTOCOL_MINOR (\d+)", text)
    if not (major and minor):
        sys.exit("hot_loader.py: no NET_PROTOCOL_MAJOR / NET_PROTOCOL_MINOR in " + shared_h)
    return "%s.%s" % (major.group(1), minor.group(1))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dist, shared_h = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "hot-loader.js"), encoding="utf-8") as f:
        script = f.read().replace("@KN_PROTOCOL@", protocol(shared_h))
    if "</script" in script.lower():
        sys.exit("hot_loader.py: hot-loader.js must not contain </script")
    page_path = os.path.join(dist, "index.html")
    with open(page_path, encoding="utf-8") as f:
        page = f.read()
    page = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n?", "", page, flags=re.S)
    anchor = re.search(r"<meta\s+charset[^>]*>", page, re.I) or re.search(r"<head[^>]*>", page, re.I)
    if not anchor:
        sys.exit("hot_loader.py: %s has no <head>" % page_path)
    block = "%s<script>%s</script>%s\n" % (BEGIN, script, END)
    page = page[:anchor.end()] + block + page[anchor.end():]
    with open(page_path, "w", encoding="utf-8") as f:
        f.write(page)


if __name__ == "__main__":
    main()
