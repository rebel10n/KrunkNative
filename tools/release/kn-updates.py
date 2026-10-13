#!/usr/bin/env python3
"""KrunkNative updates server (doc/backend.md "Client updates"): static files the launcher reads.

    stage    CI (client.yml job `updates`): picks each platform's executable-only update package
             (.tar.gz, Windows .zip) out of the client packages, writes build.json (kn-build line,
             sha256/size per package, human download links, the assets/ files the packages ship).
    publish  kru (over ssh after the CI rsync): moves a staged build into public/client/builds/,
             indexes the game content (the web asset tree, minus the files the packages ship)
             into content-addressed objects (hardlinks: no extra disk), writes the asset list and,
             last, the channel manifest public/client/<channel>.json. Only moves forward.
    assets   kru: re-index the content after the web asset tree changed (role kn_updates).
    status   print what is published.

Layout under --root (default /opt/krunknative-updates; openresty serves public/ as
https://assets.krunk.io/updates/):
    public/client/<channel>.json              channel manifest (format 1), no-cache
    public/client/builds/<commit12>/<pkg>     update packages (immutable)
    public/assets/<version>.json              asset list {"format":1,"version","files":[[path,sha256,size]]}
    public/assets/objects/<aa>/<sha256>       content-addressed files (immutable)
    state/                                    lock, hash cache, current build, history
    incoming/<commit12>/                      CI uploads (build.json + packages/)
Standard library only (Debian's python3 on kru, the GitHub runner, the e2e test).
"""
import argparse
import datetime
import errno
import fcntl
import hashlib
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
import zipfile

FORMAT = 1
PKG_RE = re.compile(r"^krunknative-client-(?P<plat>[a-z0-9]+(?:-[a-z0-9_]+)*?)\.(?P<ext>tar\.gz|zip|deb|rpm)$")
HEX_RE = re.compile(r"^[0-9a-f]+$")
PLAT_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9_]+)*$")
DEFAULT_ROOT = "/opt/krunknative-updates"
DEFAULT_WEB_ASSETS = "/opt/krunknative-web/assets"
DEFAULT_PAGE = "https://github.com/rebel10n/KrunkNative/releases/tag/client-latest"


def log(msg):
    print(f"kn-updates: {msg}", flush=True)


def die(msg):
    print(f"kn-updates: error: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_atomic(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def dumps(obj, pretty=False):
    if pretty:
        return (json.dumps(obj, indent=1, sort_keys=False) + "\n").encode()
    return json.dumps(obj, separators=(",", ":")).encode()


def path_ok(rel):
    """Same rule as kn_update_path_ok (launcher/src/update_core.c)."""
    if not rel or rel.startswith("/") or len(rel.encode()) >= 400:
        return False
    for seg in rel.split("/"):
        if seg in ("", ".", ".."):
            return False
    return not any(c in rel for c in "\\:") and all(0x20 <= ord(c) != 0x7F for c in rel)


# ---------------------------------------------------------------- stage (CI)

def package_members(path):
    """Member names of a .tar.gz / .zip package (files only)."""
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            return [i.filename for i in z.infolist() if not i.is_dir()]
    with tarfile.open(path, "r:gz") as t:
        return [m.name for m in t.getmembers() if m.isfile()]


def shipped_assets(members):
    """assets/... files inside a package: the part after the first `assets` component."""
    out = set()
    for name in members:
        parts = name.lstrip("./").split("/")
        if "assets" in parts:
            rel = "/".join(parts[parts.index("assets") + 1:])
            if path_ok(rel):
                out.add(rel)
    return out


def cmd_stage(a):
    try:
        build = json.loads(a.kn_build)
    except ValueError as e:
        die(f"--kn-build is not JSON: {e}")
    commit = str(build.get("commit", "")).lower()
    if not HEX_RE.match(commit) or len(commit) < 12 or not isinstance(build.get("commit_time"), int):
        die("--kn-build needs commit (hex) and commit_time (int)")
    c12 = commit[:12]
    out = a.out
    pkg_out = os.path.join(out, "packages")
    os.makedirs(pkg_out, exist_ok=True)
    found = {}
    for name in sorted(os.listdir(a.packages)):
        m = PKG_RE.match(name)
        if not m:
            continue
        found.setdefault(m.group("plat"), {})[m.group("ext")] = os.path.join(a.packages, name)
    if not found:
        die(f"no krunknative-client-<platform>.<ext> packages in {a.packages}")
    page = a.page
    if not page and a.download_base:
        page = re.sub(r"/releases/download/([^/]+)/?$", r"/releases/tag/\1", a.download_base.rstrip("/"))
    platforms, assets = {}, set()
    for plat, exts in sorted(found.items()):
        if not PLAT_RE.match(plat):
            die(f"bad platform id {plat}")
        kind = "zip" if plat.startswith("windows-") else "tar.gz"
        entry = {"downloads": {}}
        for ext, path in sorted(exts.items()):
            name = os.path.basename(path)
            digest = sha256_file(path)
            side = path + ".sha256"
            if os.path.exists(side):
                with open(side) as f:
                    want = f.read().split()
                if not want or want[0].lower() != digest:
                    die(f"{name}: does not match {os.path.basename(side)}")
            if a.download_base:
                entry["downloads"][ext] = f"{a.download_base.rstrip('/')}/{name}"
            if ext == kind:
                shutil.copyfile(path, os.path.join(pkg_out, name))
                entry["update"] = {"file": name, "sha256": digest, "size": os.path.getsize(path)}
                if not a.download_base:
                    entry["downloads"][ext] = f"builds/{c12}/{name}"
                assets |= shipped_assets(package_members(path))
        if "update" not in entry:
            log(f"{plat}: no {kind} package, listed without an update")
        platforms[plat] = entry
    staged = {
        "format": FORMAT,
        "channel": a.channel,
        "build": build,
        "page": page or DEFAULT_PAGE,
        "platforms": platforms,
        "package_assets": sorted(assets),
    }
    write_atomic(os.path.join(out, "build.json"), dumps(staged, pretty=True))
    log(f"staged {c12} ({build.get('describe', '?')}): {', '.join(sorted(platforms))}; "
        f"{len(assets)} assets/ files ship in the packages")


# ---------------------------------------------------------------- publish (kru)

class Root:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.public = os.path.join(self.path, "public")
        self.state = os.path.join(self.path, "state")
        self.objects = os.path.join(self.public, "assets", "objects")
        os.makedirs(self.state, exist_ok=True)
        os.makedirs(self.objects, exist_ok=True)
        os.makedirs(os.path.join(self.public, "client", "builds"), exist_ok=True)
        self._lock = open(os.path.join(self.state, "lock"), "w")
        fcntl.flock(self._lock, fcntl.LOCK_EX)

    def read_json(self, rel, default=None):
        try:
            with open(os.path.join(self.path, rel), "rb") as f:
                return json.load(f)
        except (OSError, ValueError):
            return default

    def write_json(self, rel, obj, pretty=False):
        write_atomic(os.path.join(self.path, rel), dumps(obj, pretty))


def index_assets(root, web_assets, exclude):
    """Content-addressed objects for every file of the web asset tree except the ones the
    packages ship; returns (version, list path, sha256, size, files, bytes)."""
    cache = root.read_json("state/hashcache.json", {}) or {}
    new_cache, files = {}, []
    web_assets = os.path.abspath(web_assets)
    if not os.path.isdir(web_assets):
        die(f"web asset tree {web_assets} missing")
    for dirpath, dirnames, filenames in os.walk(web_assets):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for fn in sorted(filenames):
            if fn.startswith(".") or fn.startswith("version.txt"):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, web_assets).replace(os.sep, "/")
            if rel in exclude or not path_ok(rel):
                continue
            st = os.stat(full)
            if not os.path.isfile(full):
                continue
            key = f"{st.st_dev}:{st.st_ino}:{st.st_size}:{st.st_mtime_ns}"
            digest = cache.get(rel, {}).get("k") == key and cache[rel].get("h") or sha256_file(full)
            new_cache[rel] = {"k": key, "h": digest}
            obj = os.path.join(root.objects, digest[:2], digest)
            if not os.path.exists(obj):
                os.makedirs(os.path.dirname(obj), exist_ok=True)
                tmp = obj + ".tmp"
                try:
                    if os.path.exists(tmp):
                        os.unlink(tmp)
                    os.link(full, tmp)  # same file system as the web tree: no extra disk
                except OSError as e:
                    if e.errno not in (errno.EXDEV, errno.EPERM, errno.EMLINK, errno.ENOTSUP):
                        raise
                    shutil.copyfile(full, tmp)
                    os.chmod(tmp, 0o644)
                os.replace(tmp, obj)
            files.append([rel, digest, st.st_size])
    root.write_json("state/hashcache.json", new_cache)
    files.sort(key=lambda f: f[0].encode())
    canon = dumps(files)
    version = hashlib.sha256(canon).hexdigest()[:16]
    listing = dumps({"format": FORMAT, "version": version, "files": files})
    rel_list = f"public/assets/{version}.json"
    if not os.path.exists(os.path.join(root.path, rel_list)):
        write_atomic(os.path.join(root.path, rel_list), listing)
    return {
        "version": version,
        "manifest": f"../assets/{version}.json",
        "sha256": hashlib.sha256(listing).hexdigest(),
        "size": len(listing),
        "objects": "../assets/objects/",
        "files": len(files),
        "bytes": sum(f[2] for f in files),
    }


def write_manifest(root, build, assets, grace_h):
    c12 = build["build"]["commit"][:12]
    platforms = {}
    for plat, p in build["platforms"].items():
        entry = {"downloads": dict(p.get("downloads", {}))}
        if p.get("update"):
            u = p["update"]
            entry["update"] = {"url": f"builds/{c12}/{u['file']}", "sha256": u["sha256"], "size": u["size"]}
        platforms[plat] = entry
    manifest = {
        "format": FORMAT,
        "channel": build["channel"],
        "published": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "build": build["build"],
        "outdated_grace_h": grace_h,
        "page": build.get("page") or DEFAULT_PAGE,
        "platforms": platforms,
        # the assets/ files the packages ship: not in the content set, never removed by a sync
        "assets": dict(assets, packaged=sorted(build.get("package_assets", []))),
    }
    # Optional by-path mirrors of the content (the Pages CDN, deploy/pages/assets_cdn.py) the launcher
    # downloads from alongside the objects (launcher/src/assets_sync.c). Unset: no "mirrors" key, and
    # launchers use their built-in default for assets.krunk.io; "" or "off": an empty list (none).
    mirrors = os.environ.get("KN_UPDATES_MIRRORS")
    if mirrors is not None:
        manifest["assets"]["mirrors"] = [] if mirrors.strip() in ("", "off") else mirrors.split()
    # last: a client never sees a manifest whose files are missing
    root.write_json(f"public/client/{build['channel']}.json", manifest, pretty=True)
    return manifest


def prune(root, keep):
    history = root.read_json("state/history.json", []) or []
    kept = history[-keep:] if keep > 0 else history
    builds = {h["commit12"] for h in kept}
    lists = {h["assets"] for h in kept}
    bdir = os.path.join(root.public, "client", "builds")
    for name in os.listdir(bdir):
        if name not in builds:
            shutil.rmtree(os.path.join(bdir, name), ignore_errors=True)
            log(f"pruned build {name}")
    adir = os.path.join(root.public, "assets")
    live = set()
    for name in os.listdir(adir):
        if not name.endswith(".json"):
            continue
        version = name[:-5]
        if version not in lists:
            os.unlink(os.path.join(adir, name))
            log(f"pruned asset list {version}")
            continue
        with open(os.path.join(adir, name), "rb") as f:
            live.update(e[1] for e in json.load(f)["files"])
    removed = 0
    for sub in os.listdir(root.objects):
        sdir = os.path.join(root.objects, sub)
        for obj in os.listdir(sdir):
            if obj not in live:
                os.unlink(os.path.join(sdir, obj))
                removed += 1
    if removed:
        log(f"removed {removed} unreferenced object(s)")
    root.write_json("state/history.json", kept)


def cmd_publish(a):
    incoming = os.path.abspath(a.incoming)
    build = None
    try:
        with open(os.path.join(incoming, "build.json"), "rb") as f:
            build = json.load(f)
    except (OSError, ValueError) as e:
        die(f"{incoming}/build.json: {e}")
    b = build.get("build", {})
    commit = str(b.get("commit", "")).lower()
    if build.get("format") != FORMAT or not HEX_RE.match(commit) or len(commit) < 12 or not isinstance(b.get("commit_time"), int):
        die("build.json: bad format/commit/commit_time")
    if not re.match(r"^[a-z0-9._-]+$", str(build.get("channel", ""))):
        die("build.json: bad channel")
    root = Root(a.root)
    c12 = commit[:12]
    current = root.read_json("state/current-build.json")
    if current and current["build"].get("commit_time", 0) > b["commit_time"] and not a.force:
        log(f"{current['build']['commit'][:12]} is newer than {c12}: not publishing (--force overrides)")
        shutil.rmtree(incoming, ignore_errors=True)
        return
    dest = os.path.join(root.public, "client", "builds", c12)
    os.makedirs(dest, exist_ok=True)
    for plat, p in build.get("platforms", {}).items():
        if not PLAT_RE.match(plat):
            die(f"bad platform id {plat}")
        u = p.get("update")
        if not u:
            continue
        name = u["file"]
        if not PKG_RE.match(name):
            die(f"bad package name {name}")
        src = os.path.join(incoming, "packages", name)
        target = os.path.join(dest, name)
        if os.path.exists(src):
            if sha256_file(src) != u["sha256"]:
                die(f"{name}: sha256 mismatch after upload")
            os.chmod(src, 0o644)
            os.replace(src, target)
        elif not (os.path.exists(target) and sha256_file(target) == u["sha256"]):
            die(f"{name}: missing")
    assets = index_assets(root, a.web_assets, set(build.get("package_assets", [])))
    manifest = write_manifest(root, build, assets, a.grace_h)
    root.write_json("state/current-build.json", build, pretty=True)
    history = [h for h in (root.read_json("state/history.json", []) or []) if h.get("commit12") != c12]
    history.append({"commit12": c12, "assets": assets["version"], "published": manifest["published"]})
    root.write_json("state/history.json", history)
    prune(root, a.keep)
    shutil.rmtree(incoming, ignore_errors=True)
    log(f"published {c12} ({b.get('describe', '?')}) on {build['channel']}: {len(build['platforms'])} platform(s), "
        f"assets {assets['version']} ({assets['files']} files, {assets['bytes'] / 1048576:.0f} MB)")


def cmd_assets(a):
    root = Root(a.root)
    build = root.read_json("state/current-build.json")
    if not build:
        log("nothing published yet: indexing only")
        assets = index_assets(root, a.web_assets, set())
        log(f"assets {assets['version']} ({assets['files']} files)")
        return
    assets = index_assets(root, a.web_assets, set(build.get("package_assets", [])))
    old = root.read_json(f"public/client/{build['channel']}.json", {}) or {}
    if old.get("assets", {}).get("version") == assets["version"]:
        log(f"assets unchanged ({assets['version']})")
        return
    manifest = write_manifest(root, build, assets, a.grace_h)
    # a new history entry (same build): the previous asset list stays for clients mid-sync
    history = root.read_json("state/history.json", []) or []
    history.append({"commit12": build["build"]["commit"][:12], "assets": assets["version"], "published": manifest["published"]})
    root.write_json("state/history.json", history)
    prune(root, a.keep)
    log(f"assets now {assets['version']} ({assets['files']} files, {assets['bytes'] / 1048576:.0f} MB)")


def cmd_status(a):
    root = Root(a.root)
    for name in sorted(os.listdir(os.path.join(root.public, "client"))):
        if name.endswith(".json"):
            m = root.read_json(f"public/client/{name}", {})
            b = m.get("build", {})
            log(f"{name}: {b.get('describe')} {str(b.get('commit', ''))[:12]} published {m.get('published')}, "
                f"assets {m.get('assets', {}).get('version')}, platforms {', '.join(sorted(m.get('platforms', {})))}")
    for h in root.read_json("state/history.json", []) or []:
        log(f"history: {h}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage", help="CI: stage this run's update packages + build.json")
    s.add_argument("--kn-build", required=True, help="the kn_build JSON line (tools/release/version-meta.sh)")
    s.add_argument("--packages", required=True, help="directory with krunknative-client-<plat>.<ext> (+ .sha256)")
    s.add_argument("--out", required=True)
    s.add_argument("--channel", default="latest")
    s.add_argument("--download-base", default="", help="human download links: <base>/<package> (e.g. the GitHub release)")
    s.add_argument("--page", default="", help="release page for humans")
    for name, helptext in (("publish", "kru: publish a staged build"), ("assets", "kru: re-index the game content"),
                           ("status", "print what is published")):
        q = sub.add_parser(name, help=helptext)
        if name == "publish":
            q.add_argument("incoming")
            q.add_argument("--force", action="store_true", help="publish even when an newer build is live")
        q.add_argument("--root", default=os.environ.get("KN_UPDATES_ROOT", DEFAULT_ROOT))
        q.add_argument("--web-assets", default=os.environ.get("KN_UPDATES_WEB_ASSETS", DEFAULT_WEB_ASSETS))
        q.add_argument("--keep", type=int, default=3, help="builds / asset lists kept (default 3)")
        q.add_argument("--grace-h", type=int, default=48, help="outdated_grace_h in the manifest (default 48)")
    a = p.parse_args()
    {"stage": cmd_stage, "publish": cmd_publish, "assets": cmd_assets, "status": cmd_status}[a.cmd](a)


if __name__ == "__main__":
    main()
