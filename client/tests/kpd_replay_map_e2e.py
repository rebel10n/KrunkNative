#!/usr/bin/env python3
"""A K.P.D replay on a map the client lacks (client/src/net/demo_play.c, doc/v10/replays.md).

  python3 -I client/tests/kpd_replay_map_e2e.py BUILD_DIR      (repository root)

v10.0.7's initGame fetches a map the client has not loaded (GET /maps/<id>, deob-10.0.7.js:226167) and applies the
INIT once it is in; a failed fetch is dropped silently and injectInit's 20 s wait runs out (:201831-201845). A Box
Office demo names its map by MAP_INFO's hash, and a live client would ask the game server for a missing one
(MAP_REQUEST), which no replay has. So the replay fetches it from the brain (GET /api/map-data?name=, as Play does).

A Sandstorm demo is recorded first (server/tests/kpd_demo_e2e.py), then its MAP_INFOs are rewritten to name a map
no client ships (another hash and name). A stub brain answers /api/map-data with that map's JSON. Two runs of a
headless client_bin --demo:
  1. the stub has the map: the replay fetches it, builds that map's world and plays, with no map-download wait
  2. the stub answers 404: the replay says so, waits out v10's 20 s and plays on
77 = skip (no client_bin or server_bin). Logs are kept in ~/.cache/kn-replay-map-* on failure.
"""
import http.server
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.parse

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
SERVER = os.path.join(BUILD, "server_bin")
CLIENT = os.path.join(BUILD, "client_bin")
for b in (SERVER, CLIENT):
    if not os.path.exists(b):
        print("kpd_replay_map_e2e: %s missing: skip" % b)
        sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
import kn_demo  # noqa: E402
from kn_proto import free_ports  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-replay-map-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
FETCHED, MISSING = "ReplayFetch%d" % STAMP, "ReplayMissing%d" % STAMP
MAP_INFO = 25
SOURCE_MAP = os.path.join(ROOT, "assets", "maps", "official", "02_sandstorm.json")
results = []
asked = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + str(detail)) if detail and not ok else ""), flush=True)


def record_demo():
    keep = os.path.join(WORK, "demos")
    r = subprocess.run([sys.executable, "-I", "server/tests/kpd_demo_e2e.py", BUILD, "Sandstorm", keep], cwd=ROOT,
                       capture_output=True, text=True, timeout=300, env=dict(os.environ, TMPDIR=WORK))
    files = sorted(f for f in os.listdir(keep) if f.endswith(".kndemo")) if os.path.isdir(keep) else []
    check("server_bin recorded a Sandstorm demo (kpd_demo_e2e)", r.returncode == 0 and files, r.stdout[-400:])
    return open(os.path.join(keep, files[0]), "rb").read() if files else None


def retarget(data, name, hash_):
    """The demo with every MAP_INFO naming map `name` (hash `hash_`): NetMapInfoPacket is i32 index, u32 hash, u32
    size, char name[32]."""
    version, meta, records = kn_demo.parse(data)
    meta_bytes = json.dumps(meta).encode()
    out = bytearray(kn_demo.MAGIC + bytes([version]) + struct.pack("<I", len(meta_bytes)) + meta_bytes)
    changed = 0
    for t, kind, frames in records:
        body = bytearray()
        for ptype, payload in frames:
            if ptype == MAP_INFO and len(payload) >= 44:
                index, _, size = struct.unpack_from("<iII", payload, 0)
                payload = struct.pack("<iII32s", index, hash_, size, name.encode()[:31]) + payload[44:]
                changed += 1
            body += bytes([ptype, len(payload) >> 8, len(payload) & 255]) + payload
        out += struct.pack("<IBI", t, kind, len(body)) + body
    return bytes(out), changed


class Brain(http.server.BaseHTTPRequestHandler):
    """/api/map-data?name=<FETCHED>: the published map's JSON (services/brain/creations.c h_map_data); 404 otherwise."""
    map_text = b""

    def do_GET(self):  # noqa: N802 - http.server's name
        url = urllib.parse.urlparse(self.path)
        if url.path == "/api/map-data":
            name = urllib.parse.parse_qs(url.query).get("name", [""])[0]
            asked.append(name)
            if name == FETCHED:
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(self.map_text)))
                self.end_headers()
                self.wfile.write(self.map_text)
                return
        body = b'{"error":"not_found"}'
        self.send_response(404)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def run_client(demo_path, brain_url, label, frames, timeout):
    log = os.path.join(WORK, "client-%s.log" % label)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY") and (not k.startswith("KRUNKNATIVE_") or k == "KRUNKNATIVE_EGL_DEVICE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg-" + label),
                "XDG_STATE_HOME": os.path.join(WORK, "state-" + label), "XDG_CACHE_HOME": os.path.join(WORK, "cache-" + label),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_REPLAY_LOG": "1", "KRUNKNATIVE_BRAIN_URL": brain_url,
                "KRUNKNATIVE_CAPTURE_FRAMES": str(frames), "KRUNKNATIVE_SCREENSHOT": os.path.join(WORK, label + ".png"),
                "KRUNKNATIVE_EGL_DEVICE": os.environ.get("KRUNKNATIVE_EGL_DEVICE", "mesa"),
                # nothing reaches production: no server finder, krunker.io's hosts are a dead local port
                "KRUNKNATIVE_FINDER_URL": "off", "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:9",
                "KRUNKNATIVE_KRUNKER_SOCIAL": "http://127.0.0.1:9/social", "KRUNKNATIVE_KRUNKER_EDITOR": "http://127.0.0.1:9/editor"})
    with open(log, "w") as out:
        p = subprocess.Popen([CLIENT, "--brain", brain_url, "--finder", "off", "--demo", demo_path], cwd=ROOT, stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    httpd = None
    try:
        with open(SOURCE_MAP) as f:
            source = json.load(f)
        source["name"] = FETCHED  # another map JSON: its hash is no map the client ships
        Brain.map_text = json.dumps(source, separators=(",", ":")).encode()
        (brain_port,) = free_ports(1)
        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", brain_port), Brain)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        brain_url = "http://127.0.0.1:%d" % brain_port

        demo = record_demo()
        if not demo:
            return
        fetch_demo, changed = retarget(demo, FETCHED, 0x5EED0001)
        check("the demo's MAP_INFOs name the missing map", changed >= 1, changed)
        missing_demo, _ = retarget(demo, MISSING, 0x5EED0002)
        paths = {}
        for label, data in (("fetch", fetch_demo), ("missing", missing_demo)):
            paths[label] = os.path.join(WORK, label + ".kndemo")
            with open(paths[label], "wb") as f:
                f.write(data)

        log = run_client(paths["fetch"], brain_url, "fetch", 90, 120)
        check("the replay started", re.search(r"^\[replay\] demo fetch: \d+ records", log, re.M), "log in " + WORK)
        check("it fetches the map it lacks", "[replay] fetching map %s" % FETCHED in log)
        check("the brain was asked for it by name", FETCHED in asked, asked)
        check("the map arrived", "[replay] map %s fetched" % FETCHED in log)
        check("the replay's world is that map", "[replay] world: %s" % FETCHED in log, re.findall(r"\[replay\] world: .*", log))
        check("no map download was waited out", "map download timed out" not in log and "did not load" not in log)
        check("the playhead moved", re.search(r"^\[replay\] \d+ / \d+ ms", log, re.M))

        log = run_client(paths["missing"], brain_url, "missing", 60, 150)
        check("a map the brain lacks is asked for", MISSING in asked, asked)
        check("the replay says it cannot be fetched", "[replay] map %s cannot be fetched" % MISSING in log)
        check("v10's wait runs out and the replay plays on", re.search(r"^\[replay\] \d+ / \d+ ms", log, re.M), "log in " + WORK)
    except Exception as e:  # noqa: BLE001
        check("exception", False, repr(e))
    finally:
        if httpd:
            httpd.shutdown()
    ok = results and all(results)
    print("%s: %d/%d" % ("PASS" if ok else "FAIL", sum(results), len(results)))
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
