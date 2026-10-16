#!/usr/bin/env python3
"""Remote images in the native HTML engine and the server browser's custom games carousel, end to end
(client/include/remote_images.h, client/src/html/engine/image.h "remote images", servers_ui.c carousel).

  python3 -I server/tests/remote_images_e2e.py BUILD_DIR      (from the repository root)

One Python stub (a port from kn_proto.free_ports()) is both Krunker's matchmaker (GET /game-list: five
custom games on published maps, c + cm, one map twice) and user-assets (GET /m<cm>/thumb.png: the real
thumbnails of tools/uidiff/remote/, one 404, one HTML page instead of an image). The client asks
KRUNKNATIVE_KRUNKER_MATCHMAKER / KRUNKNATIVE_KRUNKER_USER_ASSETS = the stub: nothing leaves the machine.

  1 cold cache: the Servers window shows v10's carousel (three sets of the 5 slides), every thumbnail is
    asked for once on the media lane, the good ones land in the disk cache and pop in ("[image] ...:
    arrived"), the 404 and the HTML page are refused with their reason and never cached; the carousel's
    next button rotates the slides by three (v10's PureJSCarousel), prev rotates them back
  2 warm cache, a new process: the thumbnails come from the disk cache, no request reaches the stub for
    them, and none waits for an arrival
  3 a slide's click searches the list for its game (windows[1].searchList(name))
  4 the main menu's Popular Now (v10 fetchMatches): krunker.io's custom games (their list asked 3 s after the
    menu's first draw), top 3 maps by players, and a card joins the busiest custom game on its map with room (servers_ui_popular_official)

Each run is one headless offline client_bin (KRUNKNATIVE_HEADLESS=1, KRUNKNATIVE_AUDIO_BACKEND=null) driven
by an input script. Exit 0 when every check passed; logs stay in ~/.cache/kn-remote-images-e2e-* on failure.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-remote-images-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PORT = kn_proto.free_ports(1)[0]
URL = "http://127.0.0.1:%d" % PORT
GOOD = [114845, 148572, 115530, 158288, 101668]
MISSING, NOT_IMAGE = 404404, 777
GAMES = {"success": True, "totalPlayerCount": 82, "games": [
    ["FRA:uezfe", "de-fra", 24, 40, {"i": "Zombie_Bulwark", "g": 6, "c": 1, "cm": 114845}, 0],
    ["SIN:gf196", "sgp", 23, 40, {"i": "TowerWar_3", "g": 1, "c": 1, "cm": 148572}, 0],
    ["FRA:3aeqz", "de-fra", 10, 10, {"i": "ParkourIceBlocks", "g": 4, "c": 1, "cm": 115530}, 0],
    ["FRA:f9qel", "de-fra", 9, 10, {"i": "Trench_War_H1", "g": 25, "c": 1, "cm": 158288}, 0],
    ["SV:wyjwr", "us-ca-sv", 4, 10, {"i": "Trench_War_H1", "g": 19, "c": 1, "cm": 158288}, 0],
    ["BRZ:dnaj7", "brz", 6, 10, {"i": "VIETNAM_WAR", "g": 0, "c": 1, "cm": 101668, "ds": 1}, 0],
    ["NY:gone1", "us-nj", 2, 10, {"i": "Gone_Map", "g": 0, "c": 1, "cm": MISSING}, 0],
    ["NY:html1", "us-nj", 1, 10, {"i": "Html_Map", "g": 0, "c": 1, "cm": NOT_IMAGE}, 0],
    ["NY:pub01", "us-nj", 7, 8, {"i": "Kanji", "g": 0}, 240],
    # an official custom: the Official Customs group comes first, so searchList's group 1 is Custom Games (v10)
    ["FRA:oc777", "de-fra", 12, 30, {"i": "Shipyard", "g": 2, "oc": 1, "ds": 1, "h": "Official Shipyard 24/7"}, 0],
]}
hits = []
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(("PASS  " if ok else "FAIL  ") + name + (("  -- " + str(detail)) if detail != "" and not ok else ""))


class Stub(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        hits.append(path)
        status, ctype, body = 404, "text/plain", b"not found"
        m = re.fullmatch(r"/m(\d+)/thumb\.png", path)
        if path == "/game-list":
            status, ctype, body = 200, "application/json", json.dumps(GAMES).encode()
        elif m and int(m.group(1)) in GOOD:
            with open(os.path.join(ROOT, "tools/uidiff/remote/m%s-thumb.png" % m.group(1)), "rb") as f:
                status, ctype, body = 200, "image/png", f.read()
        elif m and int(m.group(1)) == NOT_IMAGE:
            status, ctype, body = 200, "text/html", b"<html><body>ACCESS DENIED</body></html>"
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def run(name, script, cache_dir, timeout=120):
    spath = os.path.join(WORK, name + ".script")
    with open(spath, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL") and not k.lower().endswith("_proxy")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": spath, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": URL, "KRUNKNATIVE_KRUNKER_USER_ASSETS": URL, "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_REMOTE_IMAGE_CACHE": cache_dir})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished within {timeout} s", False)
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def probe(log, label):
    m = re.search(r"\[input_script\] text t=\d+ @" + re.escape(label) + r" n=(\d+) checked=(\d) value=(.*?) text=(.*)", log)
    return {"n": int(m.group(1)), "value": m.group(3), "text": m.group(4).strip()} if m else {"n": -1, "value": "", "text": ""}


def nodes(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @" + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


def thumb_hits(since=0):
    return [h for h in hits[since:] if h.endswith("/thumb.png")]


# (input scripts read "#" as a comment: ids go as [id=...]) the container holds clones, the slides, clones: with 7 slides the first original is child 8
FIRST = "[id=carousel-infinite] .purejscarousel-slides-container > .slide:nth-child(8) .slideName"


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("remote_images_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    httpd = ThreadingHTTPServer(("127.0.0.1", PORT), Stub)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    cache_dir = os.path.join(WORK, "remote-images")
    try:
        # ---- 1: cold cache
        log = run("cold", """
0 menu
300 call showWindow 2
4000 nodes @slides [id=carousel-infinite] .slide
4050 nodes @originals [id=carousel-infinite] .purejscarousel-slide:not(.purejscarousel-slide-clone)
4100 text @first """ + FIRST + """
4200 call knCarousel 1
5000 text @next """ + FIRST + """
5100 call knCarousel -1
5900 text @back """ + FIRST + """
6000 quit
""", cache_dir)
        check("1: the carousel has v10's three sets of the 7 custom games on published maps (two of them without a picture)",
              nodes(log, "slides") == 21 and nodes(log, "originals") == 7, (nodes(log, "slides"), nodes(log, "originals")))
        first = probe(log, "first")["text"]
        check("1: most players first: Zombie_Bulwark (24)", first == "Zombie_Bulwark", first)
        nxt, back = probe(log, "next")["text"], probe(log, "back")["text"]
        # 24, 23, 13 (Trench_War_H1: 9 + 4 summed), then ParkourIceBlocks (10): three on
        check("1: next slides three on, to ParkourIceBlocks", nxt == "ParkourIceBlocks", nxt)
        check("1: prev slides back", back == "Zombie_Bulwark", back)
        got = sorted(set(thumb_hits()))
        want = sorted("/m%d/thumb.png" % cm for cm in GOOD + [MISSING, NOT_IMAGE])
        check("1: every thumbnail was asked for, each once", got == want and len(thumb_hits()) == len(want), thumb_hits())
        arrived = re.findall(r"\[image\] (\S+/thumb\.png): arrived", log)
        check("1: the five real thumbnails arrived and were loaded", len(set(arrived)) == 5, arrived)
        check("1: the 404 is refused with its status", re.search(r"\[remote-image\] \S+/m%d/thumb\.png failed: HTTP 404" % MISSING, log),
              re.findall(r"\[remote-image\].*", log)[:6])
        check("1: an HTML page is not an image", re.search(r"\[remote-image\] \S+/m%d/thumb\.png failed: not an image" % NOT_IMAGE, log),
              re.findall(r"\[remote-image\].*", log)[:6])
        files = sorted(os.listdir(cache_dir)) if os.path.isdir(cache_dir) else []
        check("1: the disk cache holds the five images, nothing for the failures, no temporary file",
              len(files) == 5 and all(f.endswith(".img") for f in files), files)

        # ---- 2: warm cache, a new process
        since = len(hits)
        log = run("warm", """
0 menu
300 call showWindow 2
3000 nodes @slides [id=carousel-infinite] .slide
3500 quit
""", cache_dir)
        warm = [h for h in thumb_hits(since) if any(h == "/m%d/thumb.png" % cm for cm in GOOD)]
        check("2: the cached thumbnails are not asked for again", not warm, warm)
        check("2: none waited for an arrival (read from the disk cache)", not re.search(r"\[image\] \S+/m(%s)/thumb\.png: arrived" %
                                                                                         "|".join(map(str, GOOD)), log))
        check("2: the carousel is there", nodes(log, "slides") == 21, nodes(log, "slides"))

        # ---- 3: a slide's click searches the list for it
        log = run("search", """
0 menu
300 call showWindow 2
3000 call windows.1.searchList "TowerWar_3"
3300 text @search [id=serverSearch]
3400 nodes @rows [id=serverHolder] .settNameIn
3500 quit
""", cache_dir)
        s = probe(log, "search")
        check("3: searchList(name) fills the search field", s["value"] == "TowerWar_3", s)
        check("3: ... and opens the Custom Games group with that game", nodes(log, "rows") >= 1, nodes(log, "rows"))

        # ---- 4: Popular Now
        log = run("popular", """
0 menu
6000 nodes @cards .popRailCard
6050 text @top .popRailCard .popRailName
6100 call knV10PopularPlay 0
7500 quit
""", cache_dir)
        top = probe(log, "top")["text"]
        check("4: Popular Now lists krunker.io's custom maps, three of them, the busiest first", nodes(log, "cards") == 3 and top == "Zombie_Bulwark",
              (nodes(log, "cards"), top))
        check("4: a card joins the busiest custom game on that map (an official lobby, in-client)",
              "[mm] Popular Now Zombie_Bulwark: krunker.io's custom game FRA:uezfe" in log
              and "[krunker] official lobby FRA:uezfe: joining in-client" in log, re.findall(r"\[(?:mm|krunker)\] (?:Popular|official lobby).*", log)[:3])
    finally:
        httpd.shutdown()
    ok = all(results)
    print("%d/%d passed" % (sum(1 for r in results if r), len(results)))
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs: " + WORK)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
