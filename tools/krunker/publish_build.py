#!/usr/bin/env python3
"""Publish a Krunker build document for clients to fetch live (client/include/krunker_build_fetch.h).

The "Krunker build" workflow (.github/workflows/krunker-build.yml) extracts Krunker's live bundle with
tools/krunker/extract_build.py --print (a static parse; no Krunker JS runs), checks the result with the
client's own parser (tools/krunker/build_check.c) and stages it here for dl.krunk.io:

  OUT/krunker/<sha256>.json        the document: build.json without what clients never read (the
                                   verbatim cheat-check JS, the map table, warnings), compact JSON
  OUT/client/krunker-build.json    the manifest naming it: {"schema": 1, "build_version",
                                   "client_version", "sha256", "path": "krunker/<sha256>.json",
                                   "published", "source": {url, sha256, bytes}}

served as https://dl.krunk.io/updates/krunker/<sha256>.json (immutable) and
https://dl.krunk.io/updates/client/krunker-build.json (no-cache) by the updates store's nginx
(deploy/ansible/roles/kn_updates). Clients take the document only when its sha256 and versions are the
manifest's.

usage:
  python3 -I tools/krunker/publish_build.py stage BUILD_JSON OUT_DIR [--published ISO8601]
  python3 -I tools/krunker/publish_build.py same OUT_DIR PUBLISHED_MANIFEST
      exit 0 when the staged manifest names the same document as PUBLISHED_MANIFEST (nothing to upload)
"""

import argparse
import datetime
import hashlib
import json
import os
import sys

MANIFEST_SCHEMA = 1
# build.json keys clients read (shared/src/krunker_build_live.c); everything else stays in the repo
CLIENT_KEYS = ("generator_rev", "schema", "source", "ahkey", "ahkey_hex", "ahnum_step", "ahkey_form", "ahnum_trailer",
               "client_version", "build_version", "ahnum_exempt", "packets_c2s", "packets_s2c", "cheat", "layouts", "extras")


def client_doc(build):
    missing = [k for k in ("schema", "client_version", "build_version", "packets_c2s", "packets_s2c", "layouts") if k not in build]
    if missing:
        raise ValueError("not a build.json (no %s)" % ", ".join(missing))
    doc = {k: build[k] for k in CLIENT_KEYS if k in build}
    return (json.dumps(doc, ensure_ascii=False, separators=(",", ":"), sort_keys=False) + "\n").encode("utf8")


def stage(build_path, out, published=None):
    with open(build_path, encoding="utf8") as f:
        build = json.load(f)
    data = client_doc(build)
    sha = hashlib.sha256(data).hexdigest()
    rel = "krunker/%s.json" % sha
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "build_version": build["build_version"],
        "client_version": build["client_version"],
        "sha256": sha,
        "path": rel,
        "published": published or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": build.get("source") or {},
    }
    for path, body in ((os.path.join(out, rel), data),
                       (os.path.join(out, "client", "krunker-build.json"), (json.dumps(manifest, indent=1) + "\n").encode())):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(body)
    print("staged %s: Krunker %s (buildVersion %s), %d bytes, sha256 %s" % (
        out, manifest["client_version"], manifest["build_version"], len(data), sha))
    return manifest


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage")
    s.add_argument("build_json")
    s.add_argument("out")
    s.add_argument("--published")
    c = sub.add_parser("same")
    c.add_argument("out")
    c.add_argument("published_manifest")
    a = ap.parse_args(argv)
    if a.cmd == "stage":
        stage(a.build_json, a.out, a.published)
        return 0
    with open(os.path.join(a.out, "client", "krunker-build.json"), encoding="utf8") as f:
        mine = json.load(f)
    try:
        with open(a.published_manifest, encoding="utf8") as f:
            theirs = json.load(f)
    except (OSError, ValueError):
        theirs = {}
    same = isinstance(theirs, dict) and theirs.get("sha256") == mine["sha256"] and theirs.get("schema") == mine["schema"]
    print("published: %s" % ("the same document" if same else "%s -> %s" % (theirs.get("client_version") if isinstance(theirs, dict) else None,
                                                                              mine["client_version"])))
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
