#!/usr/bin/env python3
"""assets_cdn.py end to end against a local updates store (no Cloudflare): the split over two
Pages projects (deploy/asset-shards.json format), the per-project trees, version.txt, _headers,
the default project's _redirects, --dry-run, the limits and --current.

    python3 -I deploy/pages/assets_cdn_test.py
Standard library only; everything lives in a temporary directory and two loopback servers.
"""
import hashlib
import http.server
import json
import os
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def serve(directory):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=directory, **k)

        def log_message(self, *a):
            pass
    s = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
    s.daemon_threads = True
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return s


def publish(root, tree, packaged, version):
    """The updates store kn-updates.py writes (indexed files as objects, packaged ones by path)."""
    files = []
    for path, data in sorted(tree.items()):
        sha = hashlib.sha256(data).hexdigest()
        files.append([path, sha, len(data)])
        obj = os.path.join(root, "updates", "assets", "objects", sha[:2], sha)
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        with open(obj, "wb") as f:
            f.write(data)
    listing = json.dumps({"format": 1, "version": "list1", "files": files}).encode()
    with open(os.path.join(root, "updates", "assets", "list1.json"), "wb") as f:
        f.write(listing)
    manifest = {"format": 1, "assets": {"version": "list1", "manifest": "../assets/list1.json", "objects": "../assets/objects/",
                                        "sha256": hashlib.sha256(listing).hexdigest(), "packaged": sorted(packaged)}}
    os.makedirs(os.path.join(root, "updates", "client"), exist_ok=True)
    with open(os.path.join(root, "updates", "client", "latest.json"), "w") as f:
        json.dump(manifest, f)
    for path, data in packaged.items():
        dst = os.path.join(root, "assets", path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
    with open(os.path.join(root, "assets", "version.txt"), "w") as f:
        f.write(version + "\n")


def run(*args):
    p = subprocess.run([sys.executable, "-I", os.path.join(HERE, "assets_cdn.py"), *args], capture_output=True, text=True,
                       timeout=120)
    return p.returncode, p.stdout, p.stderr


def tree_of(d):
    return sorted(os.path.relpath(os.path.join(r, n), d) for r, _ds, fs in os.walk(d) for n in fs)


def main():
    cache = os.path.expanduser("~/.cache")
    work = tempfile.mkdtemp(prefix="kn-assets-cdn-test-", dir=cache if os.path.isdir(cache) else None)
    servers = []
    try:
        origin_dir = os.path.join(work, "origin")
        tree = {"textures/previews/a.png": b"preview a", "textures/previews/b.png": b"preview b",
                "textures/hats/h.png": b"hat", "textures/hatsX/x.png": b"not hats",
                "textures/weapons/skins/w_1.png": b"skin", "models/m.obj": b"v 0 0 0",
                "sound/s.mp3": b"ID3", "textures/previews/big/huge.png": b"excluded"}
        packaged = {"shaders/basic.vert": b"void main(){}", "img/icon.png": b"PNG"}
        publish(origin_dir, tree, packaged, "v1abc")
        origin = serve(origin_dir)
        servers.append(origin)
        o_url = "http://127.0.0.1:%d/" % origin.server_address[1]
        pages = os.path.join(work, "pages")  # what the pages.dev names serve after a deploy (for --current)
        os.makedirs(pages)
        pages_srv = serve(pages)
        servers.append(pages_srv)
        p_url = "http://127.0.0.1:%d/" % pages_srv.server_address[1]
        cfg = {"live": False, "max_files": 7, "download": "https://dl.example/", "kru_only": ["/updates/"],
               "exclude": ["textures/previews/big/"],
               "shards": [{"name": "a", "project": "p-a", "url": "https://assets.example/", "pages_dev": p_url + "a/"},
                          {"name": "b", "project": "p-b", "url": "https://assets1.example/", "pages_dev": p_url + "b/",
                           "prefixes": ["textures/previews/", "textures/hats/"]}]}
        cfg_path = os.path.join(work, "shards.json")
        with open(cfg_path, "w") as f:
            json.dump(cfg, f)
        out = os.path.join(work, "out")
        objects = os.path.join(work, "objects")

        print("1  dry run: counts per shard, nothing written")
        code, _o, err = run(out, "--config", cfg_path, "--origin", o_url, "--objects", objects, "--dry-run")
        check(code == 0, "exit 0 (%s)" % err.strip()[-300:])
        check("shard a (p-a" in err and "shard b (p-b" in err, "both shards reported")
        check("shard a (p-a, https://assets.example/): 7 files" in err, "a: 4 files + 2 packaged + version.txt (%s)" %
              [l for l in err.splitlines() if "shard a" in l])
        check("shard b (p-b, https://assets1.example/): 4 files" in err, "b: 3 files + version.txt")
        check("excluded textures/previews/big/" in err and "1 files" in err, "the excluded prefix counted")
        check(not os.path.exists(out), "nothing staged")

        print("2  stage both shards")
        code, _o, err = run(out, "--config", cfg_path, "--origin", o_url, "--objects", objects)
        check(code == 0, "exit 0 (%s)" % err.strip()[-300:])
        a, b = tree_of(os.path.join(out, "a")), tree_of(os.path.join(out, "b"))
        check(a == sorted(["_headers", "_redirects", "assets/version.txt", "assets/img/icon.png", "assets/shaders/basic.vert",
                           "assets/models/m.obj", "assets/sound/s.mp3", "assets/textures/hatsX/x.png",
                           "assets/textures/weapons/skins/w_1.png"]), "a: everything else + packaged (%s)" % a)
        check(b == sorted(["_headers", "assets/version.txt", "assets/textures/hats/h.png", "assets/textures/previews/a.png",
                           "assets/textures/previews/b.png"]), "b: its directories, no _redirects (%s)" % b)
        with open(os.path.join(out, "b", "assets", "textures", "hats", "h.png"), "rb") as f:
            check(f.read() == b"hat", "content verified and in place")
        for s in ("a", "b"):
            with open(os.path.join(out, s, "assets", "version.txt")) as f:
                check(f.read().strip() == "v1abc", "%s: the origin's version.txt" % s)
        with open(os.path.join(out, "a", "_redirects")) as f:
            red = [l for l in f.read().splitlines() if l and not l.startswith("#")]
        check(red[0] == "/assets/textures/previews/big/* https://dl.example/assets/textures/previews/big/:splat 302",
              "excluded: 302 to the download host, before the shard rule (%s)" % red[:1])
        check("/assets/textures/previews/* https://assets1.example/assets/textures/previews/:splat 301" in red and
              "/assets/textures/hats/* https://assets1.example/assets/textures/hats/:splat 301" in red, "b's directories: 301")
        check(red[-1] == "/updates/* https://dl.example/updates/:splat 302", "the updates store: 302 to kru")
        with open(os.path.join(out, "a", "_headers")) as f:
            check("Access-Control-Allow-Origin: *" in f.read(), "_headers: CORS")

        print("3  --current: changed until both shards have the version")
        code, o, _e = run(out, "--config", cfg_path, "--origin", o_url, "--current")
        check(code == 0 and o.strip() == "changed", "nothing deployed: changed")
        shutil.copytree(os.path.join(out, "a"), os.path.join(pages, "a"))
        code, o, _e = run(out, "--config", cfg_path, "--origin", o_url, "--current")
        check(o.strip() == "changed", "only a deployed: changed")
        shutil.copytree(os.path.join(out, "b"), os.path.join(pages, "b"))
        code, o, _e = run(out, "--config", cfg_path, "--origin", o_url, "--current")
        check(o.strip() == "same", "both deployed: same")

        print("4  limits")
        code, _o, err = run(out, "--config", cfg_path, "--origin", o_url, "--dry-run", "--max-files", "6")
        check(code != 0 and "still over the Pages limits" in err and "a: 7 files (max 6)" in err,
              "a shard over --max-files fails, naming it (%s)" % err.strip().splitlines()[-1:])
        check("warning: shard a" not in err, "at max_files: no warning")
        cfg["max_files"] = 6
        with open(cfg_path, "w") as f:
            json.dump(cfg, f)
        code, _o, err = run(out, "--config", cfg_path, "--origin", o_url, "--dry-run")
        check(code == 0 and "warning: shard a has 7 files, over the 6" in err, "over the file's max_files: a warning only")
        code, _o, err = run(out, "--config", cfg_path, "--origin", o_url, "--dry-run", "--exclude", "models/")
        check("excluded models/" in err and "shard a (p-a, https://assets.example/): 6 files" in err,
              "--exclude adds a prefix on no shard")
    finally:
        for s in servers:
            s.shutdown()
        shutil.rmtree(work, ignore_errors=True)
    print("assets_cdn_test: %s" % ("FAILED: " + "; ".join(FAILS) if FAILS else "all passed"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
