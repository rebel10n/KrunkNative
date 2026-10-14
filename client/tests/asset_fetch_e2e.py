#!/usr/bin/env python3
"""Native on-demand game content fetch end-to-end (client/include/asset_fetch.h,
client/src/net/asset_fetch.c, client/src/asset_fetch_client.c).

  python3 -I client/tests/asset_fetch_e2e.py BUILD_DIR      (run from the repository root; make asset-fetch-test)

A private assets dir under ~/.cache mirrors the repository's assets/ with symlinks, except the
airdrop's files, which are deleted: models/adrp_0.obj, textures/adrp_0.png, textures/adrp_1.png,
sound/adrop_0.mp3, sound/adrop_1.mp3, and the package overlay icons img/streaks/package/<n>.png (a
private ui/ without them: the page falls back to the asset tree). Its content manifest (.kn-assets.json, the
launcher's format) lists them with their real size and sha256. Two stub asset origins on ports from
kn_proto.free_ports(): origin A answers 404 to everything, origin B serves the real files (slowly,
so the crate is seen missing first). A headless offline client on Sandstorm spawns an airdrop through
the Object Manager (input script `om entity 16`) in front of the camera:
  1. fetched: every deleted file in use is downloaded (A, then B), verified and written into the private
     dir (byte-identical, no partial files); the crate is invisible while its model is downloading
     and drawn once it arrived (frame check against a frame before the spawn); the landed drop's
     overlay icon is fetched for the page; one log line each
  1b. texture: the landed crate's texture arrives late: nothing is drawn with three.js's empty
     texture (transparent material), then the texture pops in (the same GL texture re-uploaded)
  2. refused: B serves other bytes for the crate's model and texture: refused (checksum), nothing
     written, the crate stays invisible, the client keeps running to its last scripted frame
  3. 404 on both origins: the model is logged once as given up (no request storm: A and B are
     asked once each), the game keeps running
  4. off: the same run on a content dir without a manifest asks no origin at all
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device
(KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
"""
import hashlib
import http.server
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
import kn_proto  # noqa: E402
from kn_png import read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-asset-fetch-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ICONS = [f"img/streaks/package/{i}.png" for i in (0, 1, 2, 3, 5, 6, 7, 8)]
DELETED = ["models/adrp_0.obj", "textures/adrp_0.png", "textures/adrp_1.png", "sound/adrop_0.mp3", "sound/adrop_1.mp3"] + ICONS
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def source(rel):
    """the real file of an asset path: the repository's assets/, else (an icon) its ui/"""
    for root in (os.path.join(REPO, "assets"), os.path.join(REPO, "ui")):
        p = os.path.join(root, rel)
        if os.path.isfile(p):
            return p
    return None


def mirror(src, dst, remove):
    """dst mirrors src with symlinks; the directories on the way to a removed path are real
    directories (their other entries linked), the removed paths are left out"""
    os.makedirs(dst, exist_ok=True)
    tops = {}
    for rel in remove:
        head, _, rest = rel.partition("/")
        tops.setdefault(head, []).append(rest)
    for name in sorted(os.listdir(src)):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if name in tops:
            rests = [r for r in tops[name] if r]
            if rests and os.path.isdir(s):
                mirror(os.path.realpath(s), d, rests)
            continue  # a removed file
        os.symlink(os.path.realpath(s), d)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def make_assets(name, manifest=True):
    """the private content dir and ui dir for one run"""
    assets = os.path.join(WORK, name, "assets")
    ui = os.path.join(WORK, name, "ui")
    mirror(os.path.join(REPO, "assets"), assets, [r for r in DELETED if r not in ICONS])
    # the icons: ui/ lacks them and so does the content tree (they are fetched into the content dir)
    mirror(os.path.join(REPO, "ui"), ui, ICONS)
    img = os.path.join(assets, "img")
    if os.path.islink(img):  # a real directory, so the icons land in the private dir
        os.unlink(img)
        mirror(os.path.join(REPO, "assets", "img"), img, [r[len("img/"):] for r in ICONS])
    if manifest:
        files = sorted([rel, sha256(source(rel)), os.path.getsize(source(rel))] for rel in DELETED)
        with open(os.path.join(assets, ".kn-assets.json"), "w") as f:
            json.dump({"format": 1, "version": "e2e", "files": files}, f)
    return assets, ui


class Origin:
    """a stub asset origin: GET /assets/<path>; mode per path: "serve", "bad", "404" (default)"""

    def __init__(self, port, modes):
        self.modes, self.holds, self.hits = modes, {}, []
        origin = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_GET(self):
                rel = self.path.split("?", 1)[0]
                rel = rel[len("/assets/"):] if rel.startswith("/assets/") else ""
                origin.hits.append(rel)
                mode = origin.modes.get(rel, "404")
                hold = origin.holds.get(rel)
                if hold:  # held back until the test releases it (the "pending" frame was written)
                    hold.wait(60)
                body = None
                if mode == "serve" and source(rel):
                    with open(source(rel), "rb") as f:
                        body = f.read()
                elif mode == "bad" and source(rel):
                    body = b"x" * os.path.getsize(source(rel))  # right size, other bytes
                if body is None:
                    body = b"not found"
                    self.send_response(404)
                else:
                    self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{port}/assets/"

    def close(self):
        self.server.shutdown()
        self.server.server_close()


# Sandstorm, offline: stand at a fixed spot (open sky), look at the floor ahead and spawn an airdrop
# there (it falls 500 units at 0.05 / ms: lands 10 s later), turn so it lands right of the gun.
# Shots: before it is in view, landed while its model is still downloading (origin B holds the model
# back until the "pending" frame is written), after the model arrived. Fixed 60 fps steps at a 60 fps cap: script time ~ wall time.
SCRIPT = """0 play
300 pose -105 48 86 3.14159 0
500 look 0 -0.45
2000 om entity 16
2500 look 0.45 0.35
2600 shot {base}
13000 shot {pending}
17500 shot {after}
17600 state
18000 quit
"""
def run_client(name, assets, ui, origins, release=None, timeout=150):
    """release: an Event set once the "pending" frame is on disk (the held file may go then)"""
    shots = {k: os.path.join(WORK, f"{name}-{k}.png") for k in ("base", "pending", "after")}
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(SCRIPT.format(**shots))
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_ASSET_FETCH",
                        "KRUNKNATIVE_ASSET_FETCH_CHECKOUT")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "960x540",
                "KRUNKNATIVE_FIXED_DELTA": "16.6667", "KRUNKNATIVE_FRAME_CAP": "60",
                "KRUNKNATIVE_ASSETS_DIR": assets, "KRUNKNATIVE_UI_DIR": ui, "KRUNKNATIVE_ASSET_ORIGINS": origins})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(REPO, BUILD, "client_bin"), "--offline", "--map", "sandstorm"],
                             stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True,
                             cwd=os.path.join(WORK, name))
        procs.append(p)
        deadline = time.time() + timeout
        while p.poll() is None:
            if time.time() > deadline:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
                break
            if release is not None and not release.is_set() and os.path.exists(shots["pending"]):
                time.sleep(0.2)  # the frame is written
                release.set()
            time.sleep(0.05)
    if release is not None:
        release.set()
    return read(log).splitlines(), shots, p.returncode


def region_diff(a, b, box):
    """mean absolute RGB difference of two same-size PNGs inside box (x, y, w, h)"""
    try:
        wa, ha, ca, ra = read_rgba(a)
        wb, hb, cb, rb = read_rgba(b)
    except Exception:
        return -1.0
    if (wa, ha) != (wb, hb):
        return -1.0
    x0, y0, w, h = box
    total, n = 0, 0
    for y in range(y0, y0 + h, 2):
        for x in range(x0, x0 + w, 2):
            pa, pb = ra[y][x * ca:x * ca + 3], rb[y][x * cb:x * cb + 3]
            total += sum(abs(pa[k] - pb[k]) for k in range(3))
            n += 3
    return total / n if n else -1.0


CRATE = (580, 255, 120, 45)  # the landed crate's top at 960x540, right of the gun's sight


def state_line(lines):
    return next((l for l in lines if "[input_script] state" in l), None)


def main():
    pa, pb = kn_proto.free_ports(2)
    modes_a, modes_b = {}, {}
    a, b = Origin(pa, modes_a), Origin(pb, modes_b)
    origins = f"{a.url} {b.url}"
    try:
        # 1. fetched: A 404s, B serves (the model slowly, so the crate is seen missing first)
        for rel in DELETED:
            modes_b[rel] = "serve"
        b.holds["models/adrp_0.obj"] = threading.Event()
        assets, ui = make_assets("fetched")
        lines, shots, rc = run_client("fetched", assets, ui, origins, release=b.holds["models/adrp_0.obj"])
        text = "\n".join(lines)
        check("fetched: the fetch is on for the content dir", "[assets] on-demand fetch for" in text)
        icons = [rel for rel in ICONS if os.path.isfile(os.path.join(assets, rel))]
        # sound/adrop_1.mp3 (the claim) is only asked for when somebody claims the drop
        for rel in [r for r in DELETED if r not in ICONS and r != "sound/adrop_1.mp3"] + icons:
            p = os.path.join(assets, rel)
            ok = os.path.isfile(p) and not os.path.islink(p) and sha256(p) == sha256(source(rel))
            check(f"fetched: {rel} written and verified", ok)
            check(f"fetched: {rel} logged once", len(re.findall(r"\[assets\] fetched " + re.escape(rel) + " ", text)) == 1)
        check("fetched: the landed drop's overlay icon was fetched for the page", len(icons) >= 1, str(icons))
        check("fetched: the page's missing icon loads once it arrived",
              icons and re.search(r"\[image\] " + re.escape(icons[0]) + r": arrived", text), str(icons))
        parts = [f for _, _, fs in os.walk(assets, followlinks=False) for f in fs if f.endswith(".part")]
        check("fetched: no partial files", not parts, str(parts[:3]))
        wanted = set(DELETED) - set(ICONS) - {"sound/adrop_1.mp3"} | set(icons)
        check("fetched: origin A was asked first (404), B served", wanted <= set(a.hits) and wanted <= set(b.hits),
              f"A {sorted(set(a.hits))} B {sorted(set(b.hits))}")
        check("fetched: nothing asked that was not needed", "sound/adrop_1.mp3" not in a.hits + b.hits)
        check("fetched: one request per file and origin", len(a.hits) == len(set(a.hits)) and len(b.hits) == len(set(b.hits)),
              f"A {len(a.hits)} B {len(b.hits)}")
        d_pending = region_diff(shots["base"], shots["pending"], CRATE)
        d_after = region_diff(shots["base"], shots["after"], CRATE)
        check("fetched: the crate is not drawn while its model downloads", 0 <= d_pending < 2.0, f"diff {d_pending:.2f}")
        check("fetched: the crate is drawn once it arrived", d_after > 6.0, f"diff {d_after:.2f}")
        check("fetched: the client ran its script", state_line(lines) and rc == 0, f"rc {rc}")
        a_hits_1, b_hits_1 = list(a.hits), list(b.hits)

        # 1b. the landed texture (adrp_1, loaded with the drop like v7's secMat) is held back: the
        # landed crate shows nothing (three.js's empty texture on a transparent material), then the
        # texture pops in (the same GL texture re-uploaded)
        a.hits.clear()
        b.hits.clear()
        b.holds = {"textures/adrp_1.png": threading.Event()}
        assets, ui = make_assets("texture")
        lines, shots, rc = run_client("texture", assets, ui, origins, release=b.holds["textures/adrp_1.png"])
        d_pending = region_diff(shots["base"], shots["pending"], CRATE)
        d_after = region_diff(shots["base"], shots["after"], CRATE)
        check("texture: adrp_1 is asked for with the drop, before its landing", "textures/adrp_1.png" in b.hits)
        check("texture: nothing is drawn while the landed texture downloads", 0 <= d_pending < 2.0, f"diff {d_pending:.2f}")
        check("texture: the texture pops in", d_after > 6.0, f"diff {d_after:.2f}")
        check("texture: one request per file and origin", b.hits.count("textures/adrp_1.png") == 1, str(b.hits))

        # 2 + 3. refused (checksum) and 404 on both origins; the game keeps running
        a.hits.clear()
        b.hits.clear()
        modes_b.clear()
        b.holds = {}
        modes_b["textures/adrp_0.png"] = "bad"
        modes_b["textures/adrp_1.png"] = "bad"
        modes_b["sound/adrop_0.mp3"] = "serve"
        # models/adrp_0.obj: 404 on both
        assets, ui = make_assets("broken")
        lines, shots, rc = run_client("broken", assets, ui, origins)
        text = "\n".join(lines)
        for rel in ("textures/adrp_0.png", "textures/adrp_1.png"):
            check(f"refused: {rel} (other bytes) not written", not os.path.lexists(os.path.join(assets, rel)))
            check(f"refused: {rel} logged as refused", re.search(r"\[assets\] " + re.escape(rel) + r": refused .*does not match", text))
        given_up = re.findall(r"\[assets\] cannot fetch models/adrp_0\.obj ", text)
        check("404: the model is given up with one log line", len(given_up) == 1, f"{len(given_up)} lines")
        check("404: asked once per origin", a.hits.count("models/adrp_0.obj") == 1 and b.hits.count("models/adrp_0.obj") == 1,
              f"A {a.hits.count('models/adrp_0.obj')} B {b.hits.count('models/adrp_0.obj')}")
        check("404: no file", not os.path.lexists(os.path.join(assets, "models/adrp_0.obj")))
        check("broken: the other files still arrive", os.path.isfile(os.path.join(assets, "sound/adrop_0.mp3")))
        d_after = region_diff(shots["base"], shots["after"], CRATE)
        check("broken: no crate is drawn (its model never arrived)", 0 <= d_after < 2.0, f"diff {d_after:.2f}")
        check("broken: the game kept running to its last scripted frame", state_line(lines) and rc == 0 and os.path.isfile(shots["after"]),
              f"rc {rc}")

        # 4. off: no manifest, no fetch, no request
        a.hits.clear()
        b.hits.clear()
        assets, ui = make_assets("off", manifest=False)
        lines, shots, rc = run_client("off", assets, ui, origins)
        text = "\n".join(lines)
        check("off: without a content manifest nothing is fetched", not a.hits and not b.hits and "[assets] fetched" not in text,
              f"A {len(a.hits)} B {len(b.hits)}")
        check("off: says why", "on-demand fetch off: no content manifest" in text)
        check("off: the game runs", state_line(lines) and rc == 0, f"rc {rc}")
        del a_hits_1, b_hits_1
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        a.close()
        b.close()
    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    if not failed and not os.environ.get("KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"work dir: {WORK}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
