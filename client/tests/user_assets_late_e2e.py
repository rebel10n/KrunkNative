#!/usr/bin/env python3
"""Community maps' user assets that arrive late, and video textures (client/src/user_asset.c
user_asset_arrived, client/src/map_video.c).

  python3 -I client/tests/user_assets_late_e2e.py BUILD_DIR    (run from the repository root)
  KN_E2E_ASSETS=<dir>: the game content to mirror (default ./assets)

The fixture map (client/tests/data/user_assets_late_map.json): a CUSTOM_ASSET 900002 (an OBJ cube whose
vertices sit 100 units above its origin: loadMesh re-centres it on its bounding box), a CUSTOM_ASSET
900003g (a GLTF cube, blue), a wall with the video asset "900004v" and one with "900005v". One stub host
(KRUNKNATIVE_USER_ASSET_ORIGINS, the mirror's place) serves them; a headless offline client looks at them
from a pinned camera with the missing-texture checker painted cyan.

  1. late: the host holds the two models back until the map is built and the load-time wait
     (KRUNKNATIVE_ASSET_WAIT_MS=300) has run out; a shot before they are released shows neither, a
     shot after shows both: the GLTF built into the running map, the OBJ re-centred on its real
     geometry (in view, not 100 units up)
  2. video: 900004v's frame sidecar (video.mp4.knv, made here with ffmpeg: one red second, one blue
     second, looped) plays on its wall: shots over 3 s show red and blue there; 900005v has no
     sidecar (404) but a texture.png: its wall shows that (orange)
  Both also cover a new round on the same map: the offline game's INIT unloads the startup map and loads
  it again without rebuilding it (game_init's map_reset), which used to drop every GLTF map asset and
  would have deleted the video textures.
Prints PASS/FAIL lines; exit code 0 only when everything passed; 77 without game content. The video
checks need ffmpeg (skipped, with a line saying so, without it).
"""
import base64
import http.server
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
from kn_png import read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
SRC_ASSETS = os.environ.get("KN_E2E_ASSETS") or os.path.join(REPO, "assets")
MAP = os.path.join(REPO, "client", "tests", "data", "user_assets_late_map.json")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-user-assets-late-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
GREEN, BLUE_GLTF, ORANGE = (40, 200, 60), (40, 80, 230), (230, 90, 20)
RED_V, BLUE_V = (230, 30, 30), (30, 30, 230)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def png(w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def cube_obj(size, lift):
    """a cube of edge `size` whose centre sits `lift` units above the origin (OBJ)"""
    h = size / 2
    v = [(-h, -h + lift, -h), (h, -h + lift, -h), (h, h + lift, -h), (-h, h + lift, -h),
         (-h, -h + lift, h), (h, -h + lift, h), (h, h + lift, h), (-h, h + lift, h)]
    faces = [(1, 2, 3, 4), (5, 8, 7, 6), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 8, 4), (5, 1, 4, 8)]
    out = ["v %g %g %g" % p for p in v] + ["vt 0 0", "vt 1 0", "vt 1 1", "vt 0 1"]
    for f in faces:
        out.append("f %d/1 %d/2 %d/3" % (f[0], f[1], f[2]))
        out.append("f %d/1 %d/3 %d/4" % (f[0], f[2], f[3]))
    return ("\n".join(out) + "\n").encode()


def cube_gltf(size, rgb):
    """a glTF 2.0 cube (embedded buffer, flat faces with normals), base colour rgb"""
    h = size / 2
    faces = [((0, 0, 1), [(-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]),
             ((0, 0, -1), [(h, -h, -h), (-h, -h, -h), (-h, h, -h), (h, h, -h)]),
             ((1, 0, 0), [(h, -h, h), (h, -h, -h), (h, h, -h), (h, h, h)]),
             ((-1, 0, 0), [(-h, -h, -h), (-h, -h, h), (-h, h, h), (-h, h, -h)]),
             ((0, 1, 0), [(-h, h, h), (h, h, h), (h, h, -h), (-h, h, -h)]),
             ((0, -1, 0), [(-h, -h, -h), (h, -h, -h), (h, -h, h), (-h, -h, h)])]
    pos, nrm, idx = [], [], []
    for n, quad in faces:
        base = len(pos)
        pos += quad
        nrm += [n] * 4
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    pb = b"".join(struct.pack("<3f", *p) for p in pos)
    nb = b"".join(struct.pack("<3f", *n) for n in nrm)
    ib = b"".join(struct.pack("<H", i) for i in idx)
    buf = pb + nb + ib
    doc = {
        "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [c / 255 for c in rgb] + [1], "metallicFactor": 0}}],
        "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(buf).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(pb)},
                        {"buffer": 0, "byteOffset": len(pb), "byteLength": len(nb)},
                        {"buffer": 0, "byteOffset": len(pb) + len(nb), "byteLength": len(ib)}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
                       "min": [-h, -h, -h], "max": [h, h, h]},
                      {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5123, "count": len(idx), "type": "SCALAR"}],
    }
    return json.dumps(doc).encode()


def knv_from_ffmpeg():
    """video.mp4.knv: 2 fps, 2 frames per colour (red, blue), 32x32, made with ffmpeg; None without it"""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return None
    frames = []
    for rgb in (RED_V, RED_V, BLUE_V, BLUE_V):
        out = subprocess.run([ffmpeg, "-v", "error", "-f", "lavfi", "-i", "color=c=0x%02x%02x%02x:s=32x32:d=1" % rgb,
                              "-frames:v", "1", "-q:v", "2", "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1"],
                             capture_output=True)
        if out.returncode or not out.stdout.startswith(b"\xff\xd8"):
            return None
        frames.append(out.stdout)
    table = 32 + 4 * (len(frames) + 1)
    offs, o = [], table
    for f in frames:
        offs.append(o)
        o += len(f)
    offs.append(o)
    head = b"KNVF" + struct.pack("<I", 1) + struct.pack("<4H", 32, 32, 32, 32) + struct.pack("<4I", 2, 1, len(frames), 2000)
    return head + struct.pack("<%dI" % len(offs), *offs) + b"".join(frames)


FILES = {
    "900002/model.obj": cube_obj(24, 100),
    "900002/model.png": png(8, 8, GREEN),
    "900003/model.gltf": cube_gltf(24, BLUE_GLTF),
    "900005/texture.png": png(8, 8, ORANGE),
}
HELD = {"900002/model.obj", "900003/model.gltf"}


class Host:
    """the stub mirror: GET /api/user-assets/<id>/<file>; HELD files wait for `release`"""

    def __init__(self):
        self.hits, self.release = [], threading.Event()
        host = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_GET(self):
                rel = self.path.split("?", 1)[0]
                rel = rel[len("/api/user-assets/"):] if rel.startswith("/api/user-assets/") else ""
                host.hits.append(rel)
                if rel in HELD:
                    host.release.wait(60)
                body = FILES.get(rel)
                self.send_response(200 if body is not None else 404)
                body = body if body is not None else b"not found"
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d/api/user-assets/" % self.server.server_address[1]

    def close(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()


def make_assets(name):
    assets = os.path.join(WORK, name, "assets")
    os.makedirs(os.path.join(assets, "user-assets"))
    for entry in sorted(os.listdir(SRC_ASSETS)):
        if entry == "user-assets" or entry.startswith("."):
            continue
        os.symlink(os.path.realpath(os.path.join(SRC_ASSETS, entry)), os.path.join(assets, entry))
    return assets


def near(px, rgb, tol=0.25):
    r, g, b = px
    R, G, B = rgb
    if max(r, g, b) < 30:
        return False
    s = max(r, g, b) / max(R, G, B)
    return all(abs(c - C * s) <= tol * max(R, G, B) * s for c, C in zip((r, g, b), (R, G, B)))


def count(shot, rgb, box=None):
    try:
        w, h, ch, rows = read_rgba(shot)
    except Exception:
        return -1
    x0, y0, x1, y1 = box or (0, 0, w, h)
    n = 0
    for y in range(y0, min(y1, h)):
        row = rows[y]
        for x in range(x0, min(x1, w)):
            if near(tuple(row[x * ch:x * ch + 3]), rgb):
                n += 1
    return n


def run(name, script, host_url, release_after_shot=None):
    assets = make_assets(name)
    shots = {}
    lines = []
    for line in script.strip().splitlines():
        t, cmd, *rest = line.split()
        if cmd == "shot":
            shots[rest[0]] = os.path.join(WORK, f"{name}-{rest[0]}.png")
            lines.append(f"{t} shot {shots[rest[0]]}")
        else:
            lines.append(line)
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_ASSET_FETCH", "KRUNKNATIVE_USER_ASSETS",
                        "KN_FETCH_PROXY", "https_proxy", "HTTPS_PROXY", "http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "XDG_CACHE_HOME": os.path.join(WORK, f"{name}-cache"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "960x540",
                "KRUNKNATIVE_FIXED_DELTA": "16.6667", "KRUNKNATIVE_FRAME_CAP": "60", "KRUNKNATIVE_HIDE_UI": "1",
                "KRUNKNATIVE_NO_FOG": "1", "KRUNKNATIVE_CAMERA": "0,24,70,-0.1,0", "KRUNKNATIVE_MISSING_TEXTURE": "00ffff",
                "KRUNKNATIVE_ASSET_WAIT_MS": "300", "KRUNKNATIVE_ASSETS_DIR": assets, "KRUNKNATIVE_USER_ASSET_ORIGINS": host_url})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(REPO, BUILD, "client_bin"), "--offline", MAP],
                             stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True,
                             cwd=os.path.join(WORK, name))
        released = False
        deadline = 120
        import time
        start = time.time()
        while p.poll() is None:
            if time.time() - start > deadline:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
                break
            if release_after_shot and not released and os.path.exists(shots[release_after_shot]):
                time.sleep(0.2)
                release_after_shot_event.set()
                released = True
            time.sleep(0.05)
    with open(log, errors="replace") as f:
        return f.read(), shots


release_after_shot_event = threading.Event()

# screen boxes (960x540, camera at (0, 24, 70) looking -Z): the OBJ at x -45, the GLTF at x +45, the
# video walls behind them
OBJ_BOX, GLTF_BOX = (120, 150, 420, 420), (540, 150, 840, 420)
VIDEO_BOX, FALLBACK_BOX = (0, 0, 480, 200), (480, 0, 960, 200)


def main():
    global release_after_shot_event
    if not os.path.isfile(os.path.join(REPO, BUILD, "client_bin")):
        print(f"no {BUILD}/client_bin")
        return 1
    if not os.path.isdir(os.path.join(SRC_ASSETS, "textures")):
        print(f"{SRC_ASSETS}: no game content (KN_E2E_ASSETS): skipped")
        return 77
    knv = knv_from_ffmpeg()
    if knv:
        FILES["900004/video.mp4.knv"] = knv
    else:
        print("ffmpeg not found: the video checks are skipped")
    host = Host()
    release_after_shot_event = host.release
    try:
        script = """0 play
800 shot before
1500 shot after0
2200 shot after1
2900 shot after2
3600 shot after3
3800 quit"""
        log, shots = run("late", script, host.url, release_after_shot="before")
        check("late: the held models were asked for during the build", "900003/model.gltf" in host.hits and "900002/model.obj" in host.hits,
              str(host.hits))
        g0, o0 = count(shots["before"], BLUE_GLTF, GLTF_BOX), count(shots["before"], GREEN, OBJ_BOX)
        check("late: before the release neither model shows", g0 < 50 and o0 < 50, f"gltf {g0} obj {o0}, {shots['before']}")
        g1 = max(count(shots[k], BLUE_GLTF, GLTF_BOX) for k in ("after0", "after1", "after2", "after3"))
        o1 = max(count(shots[k], GREEN, OBJ_BOX) for k in ("after0", "after1", "after2", "after3"))
        check("late: the GLTF model is built into the running map", g1 > 1500, f"{g1} blue pixels")
        check("late: the OBJ model shows, re-centred on its geometry (in view)", o1 > 1500, f"{o1} green pixels")
        cyan = count(shots["after3"], (0, 255, 255))
        check("late: no placeholder pixel", cyan == 0, f"{cyan}")
        if knv:
            reds = [count(shots[k], RED_V, VIDEO_BOX) for k in ("after0", "after1", "after2", "after3")]
            blues = [count(shots[k], BLUE_V, VIDEO_BOX) for k in ("after0", "after1", "after2", "after3")]
            check("video: the sidecar plays (red and blue frames over 3 s)", max(reds) > 2000 and max(blues) > 2000, f"red {reds} blue {blues}")
            check("video: one log line", log.count("[video] 900004: 4 frames 32x32 at 2 fps") >= 1)
        orange = count(shots["after3"], ORANGE, FALLBACK_BOX)
        check("video: no sidecar, texture.png shows", orange > 2000 and "900005: no video frames, showing its texture.png" in log,
              f"{orange} orange pixels")
    finally:
        host.close()
    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    if failed:
        print(f"logs and shots in {WORK}")
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
