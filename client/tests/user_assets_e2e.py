#!/usr/bin/env python3
"""A community map's Krunker user assets, fetched on demand and drawn (shared/include/user_asset_path.h,
client/src/net/asset_fetch.c user assets, the map's load-time prefetch in client/src/prefab.c).

  python3 -I client/tests/user_assets_e2e.py BUILD_DIR    (run from the repository root)
  KN_E2E_ASSETS=<dir>: the game content to mirror (default ./assets)

The fixture map (client/tests/data/user_assets_map.json) has a wall textured with the user asset
900001 ("at": its texture.png) and a CUSTOM_ASSET 900002 (model.obj + model.png), neither in the asset
tree: the files come from two stub hosts on 127.0.0.1 given as KRUNKNATIVE_USER_ASSET_ORIGINS, "krunker"
(user-assets.krunker.io's place: it answers 403 to everything, as Cloudflare does to some networks) and
"mirror" (the brain's /api/user-assets/ place, services/brain/user_assets.c). A private content dir under
~/.cache mirrors the game content with symlinks (no content manifest: the user-asset fetch alone runs).
Each run is a headless offline client on the fixture map, a pinned camera on the wall and the asset, the
missing-texture checker painted cyan (KRUNKNATIVE_MISSING_TEXTURE), one shot:
  1. fetched: the files are asked from krunker first (403: taken out after its first answers), then the
     mirror, written into user-assets/<id>/ byte for byte; the shot shows the wall's orange texture and
     the model's green one, and no placeholder pixel; one "[assets] fetched" line per file
  2. missing: both hosts answer 404: the files are given up and logged, the wall shows the placeholder
     (what Krunker shows for a texture its host no longer serves)
  3. block page: the mirror answers 200 with an HTML page: refused, nothing written, the placeholder shows
  4. off: KRUNKNATIVE_USER_ASSETS=0 asks no host at all
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
"""
import http.server
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
from kn_png import read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
SRC_ASSETS = os.environ.get("KN_E2E_ASSETS") or os.path.join(REPO, "assets")
MAP = os.path.join(REPO, "client", "tests", "data", "user_assets_map.json")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-user-assets-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ORANGE, GREEN = (230, 90, 20), (40, 200, 60)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def png(w, h, rgb):
    """a solid w x h RGB PNG"""
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def cube_obj(size):
    """a cube of edge `size` with UVs and normals (OBJ)"""
    h = size / 2
    v = [(-h, -h, -h), (h, -h, -h), (h, h, -h), (-h, h, -h), (-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]
    faces = [(1, 2, 3, 4), (5, 8, 7, 6), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 8, 4), (5, 1, 4, 8)]
    out = ["v %g %g %g" % p for p in v] + ["vt 0 0", "vt 1 0", "vt 1 1", "vt 0 1"]
    for f in faces:
        out.append("f %d/1 %d/2 %d/3" % (f[0], f[1], f[2]))
        out.append("f %d/1 %d/3 %d/4" % (f[0], f[2], f[3]))
    return ("\n".join(out) + "\n").encode()


FILES = {
    "900001/texture.png": png(8, 8, ORANGE),
    "900002/model.obj": cube_obj(24),
    "900002/model.png": png(8, 8, GREEN),
}


class Host:
    """a stub user-asset host: GET <prefix><id>/<file>; mode "serve", "404", "403" or "html" for all"""

    def __init__(self, prefix, mode):
        self.prefix, self.mode, self.hits = prefix, mode, []
        host = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_GET(self):
                rel = self.path.split("?", 1)[0]
                rel = rel[len(host.prefix):] if rel.startswith(host.prefix) else ""
                host.hits.append(rel)
                status, body = 404, b"not found"
                if host.mode == "403":
                    status, body = 403, b"<!DOCTYPE html><html><title>Krunker.io</title>Access denied</html>"
                elif host.mode == "html":
                    status, body = 200, b"<!DOCTYPE html><html><body>blocked</body></html>"
                elif host.mode == "serve" and rel in FILES:
                    status, body = 200, FILES[rel]
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d%s" % (self.server.server_address[1], prefix)

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def make_assets(name):
    """a private content dir: every entry of the game content linked, user-assets/ a real directory"""
    assets = os.path.join(WORK, name, "assets")
    os.makedirs(os.path.join(assets, "user-assets"))
    for entry in sorted(os.listdir(SRC_ASSETS)):
        if entry == "user-assets" or entry.startswith("."):
            continue
        os.symlink(os.path.realpath(os.path.join(SRC_ASSETS, entry)), os.path.join(assets, entry))
    ua = os.path.join(SRC_ASSETS, "user-assets")
    if os.path.isdir(ua):
        for entry in os.listdir(ua):
            if not entry.startswith("9000"):
                os.symlink(os.path.realpath(os.path.join(ua, entry)), os.path.join(assets, "user-assets", entry))
    return assets


SCRIPT = "0 play\n1500 shot {shot}\n1700 quit\n"


def run_client(name, assets, origins, extra_env=None, timeout=120):
    shot = os.path.join(WORK, f"{name}.png")
    script = os.path.join(WORK, f"{name}.script")
    with open(script, "w") as f:
        f.write(SCRIPT.format(shot=shot))
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_ASSET_FETCH",
                        "KRUNKNATIVE_ASSET_FETCH_CHECKOUT", "KRUNKNATIVE_USER_ASSETS", "https_proxy", "HTTPS_PROXY",
                        "http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "XDG_CACHE_HOME": os.path.join(WORK, f"{name}-cache"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "960x540",
                "KRUNKNATIVE_FIXED_DELTA": "16.6667", "KRUNKNATIVE_HIDE_UI": "1", "KRUNKNATIVE_NO_FOG": "1",
                "KRUNKNATIVE_CAMERA": "0,24,60,-0.1,0", "KRUNKNATIVE_MISSING_TEXTURE": "00ffff",
                "KRUNKNATIVE_ASSETS_DIR": assets, "KRUNKNATIVE_USER_ASSET_ORIGINS": origins})
    env.update(extra_env or {})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(REPO, BUILD, "client_bin"), "--offline", MAP],
                             stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True,
                             cwd=os.path.join(WORK, name))
        try:
            p.wait(timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read(), shot


def near(px, rgb):
    """the hue of rgb, lit: channel ratios within 25%"""
    r, g, b = px
    R, G, B = rgb
    if max(r, g, b) < 30:
        return False
    s = max(r, g, b) / max(R, G, B)
    return all(abs(c - C * s) <= 0.25 * max(R, G, B) * s for c, C in zip((r, g, b), (R, G, B)))


def count(shot):
    """(placeholder, orange, green) pixels of a shot"""
    try:
        w, h, ch, rows = read_rgba(shot)
    except Exception:
        return -1, -1, -1
    cyan = orange = green = 0
    for row in rows:
        for x in range(w):
            px = tuple(row[x * ch:x * ch + 3])
            r, g, b = px
            if g > 40 and r < 0.2 * g and abs(g - b) < 0.12 * g:
                cyan += 1
            elif near(px, ORANGE):
                orange += 1
            elif near(px, GREEN):
                green += 1
    return cyan, orange, green


def written(assets):
    return {rel: open(os.path.join(assets, "user-assets", rel), "rb").read()
            for rel in FILES if os.path.isfile(os.path.join(assets, "user-assets", rel))}


def main():
    if not os.path.isfile(os.path.join(REPO, BUILD, "client_bin")):
        print(f"no {BUILD}/client_bin")
        return 1
    if not os.path.isdir(os.path.join(SRC_ASSETS, "textures")):
        print(f"{SRC_ASSETS}: no game content (KN_E2E_ASSETS): skipped")
        return 77
    hosts = []
    try:
        # 1. fetched: krunker refuses (403), the mirror serves
        krunker, mirror = Host("/", "403"), Host("/api/user-assets/", "serve")
        hosts += [krunker, mirror]
        assets = make_assets("fetched")
        log, shot = run_client("fetched", assets, f"{krunker.url} {mirror.url}")
        got = written(assets)
        check("fetched: every file written byte for byte", got == FILES, str(sorted(got)))
        check("fetched: no partial files left", not any(".part" in f for _, _, fs in os.walk(os.path.join(assets, "user-assets")) for f in fs))
        check("fetched: the refusing host was taken out after its first answers", 1 <= len(krunker.hits) <= 2, str(krunker.hits))
        check("fetched: the mirror was asked once per file", sorted(mirror.hits) == sorted(FILES), str(mirror.hits))
        for rel in FILES:
            check(f"fetched: one log line for {rel}", log.count(f"[assets] fetched user-assets/{rel} from {mirror.url}") == 1)
        cyan, orange, green = count(shot)
        check("fetched: no placeholder pixel", cyan == 0, f"{cyan} placeholder pixels, {shot}")
        check("fetched: the wall shows its texture", orange > 5000, f"{orange} orange pixels")
        check("fetched: the model shows its texture", green > 300, f"{green} green pixels")
        krunker.close()
        mirror.close()

        # 2. missing everywhere
        krunker, mirror = Host("/", "404"), Host("/api/user-assets/", "404")
        hosts += [krunker, mirror]
        assets = make_assets("missing")
        log, shot = run_client("missing", assets, f"{krunker.url} {mirror.url}")
        check("missing: nothing written", not written(assets))
        check("missing: the texture is given up and logged", "cannot fetch user-assets/900001/texture.png" in log)
        cyan, orange, green = count(shot)
        check("missing: the wall shows the placeholder", cyan > 5000 and orange < 100, f"{cyan} placeholder, {orange} orange")
        check("missing: each host asked once per file", sorted(krunker.hits) == sorted(set(krunker.hits)) and
              sorted(mirror.hits) == sorted(set(mirror.hits)), f"{krunker.hits} {mirror.hits}")
        krunker.close()
        mirror.close()

        # 3. a block page answered 200
        krunker, mirror = Host("/", "403"), Host("/api/user-assets/", "html")
        hosts += [krunker, mirror]
        assets = make_assets("blockpage")
        log, shot = run_client("blockpage", assets, f"{krunker.url} {mirror.url}")
        check("block page: refused, nothing written", not written(assets) and "not a texture.png" in log)
        cyan, _, _ = count(shot)
        check("block page: the wall shows the placeholder", cyan > 5000, f"{cyan} placeholder pixels")
        krunker.close()
        mirror.close()

        # 4. off
        krunker, mirror = Host("/", "serve"), Host("/api/user-assets/", "serve")
        hosts += [krunker, mirror]
        assets = make_assets("off")
        log, shot = run_client("off", assets, f"{krunker.url} {mirror.url}", {"KRUNKNATIVE_USER_ASSETS": "0"})
        check("off: no host asked", not krunker.hits and not mirror.hits, f"{krunker.hits} {mirror.hits}")
        check("off: nothing written", not written(assets))
    finally:
        for h in hosts:
            try:
                h.close()
            except Exception:
                pass
    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    if failed:
        print(f"logs and shots in {WORK}")
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
