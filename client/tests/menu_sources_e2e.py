#!/usr/bin/env python3
"""The main menu's Popular Now and Live Streams follow the Server Type (client/include/menu_sources.h; v10_menu.c,
servers_ui.c).

  python3 -I client/tests/menu_sources_e2e.py BUILD_DIR      (from the repository root)

Two stubs: Box Office's brain (KRUNKNATIVE_BRAIN_URL: GET /streams/top answers one stream, "box_office_live"; its
lobby list is the fixed KRUNKNATIVE_SERVER_LIST, three custom games) and krunker.io (one server for its matchmaker,
KRUNKNATIVE_KRUNKER_MATCHMAKER /game-list with three custom games, and its data API, KRUNKNATIVE_KRUNKER_DATA_API
/streams/top?cb=2 answering "kio_live"). Then, with the Server Type set through the settings handler
(setSetting knSeekServers, what Settings > Quickplay's row calls):
  - Box Office servers (the default): Popular Now shows Box Office's three maps by players and none of krunker.io's;
    the Live Streams overlay shows the brain's stream;
  - krunker.io servers: Popular Now is hidden (krunker.io's custom games are never in it); the overlay shows
    krunker.io's stream;
  - Both: Popular Now is hidden too; the overlay shows the brain's stream;
  - back to Box Office with krunker.io's list now cached: Box Office's games again, the brain's stream.
Before the fix the Box Office menu summed in krunker.io's custom games ("41 playing" counts), the krunker.io and Both
types showed them in Popular Now, and the streams were the brain's for every Server Type.

Each card's picture: the answer's thumbnailUrl is a JPEG on a third stub (the CDN: /previews/<name>-440x248.jpg). The
first card's <img src> is that URL, as v10's markup sets it (v10.js:146254) on every Server Type, the CDN is asked for
it, and the picture is drawn in a frame of the page. The cache is cold (KRUNKNATIVE_REMOTE_IMAGE_CACHE): the picture
arrives after the page's first paint, and the page must repaint to show it. The client never asks the brain's
/streams/thumb for a picture (the stub counts such requests).

Each answer has three streams, so the overlay has three cards: they sit in one row inside the grid's content box and
the grid is no taller than them, on every Server Type. A horizontal scrollbar under the cards would make the grid
taller, and v10's overlay has none (its width is its cards', v10.js:146254).

Exit code 0 when every check passed; the input-script log is kept in ~/.cache/kn-menu-sources-e2e-* on failure.
"""
import base64
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
from kn_png import read_rgba  # noqa: E402  (the frames client_bin writes: stdlib PNG)

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# the asset tree: this checkout's, else the main checkout's (a worktree may lack the large content)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets")) if os.path.isdir(os.path.join(d, "models"))),
    os.path.join(ROOT, "assets"))
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-menu-sources-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []

# Box Office's lobbies (the fixed list, KRUNKNATIVE_SERVER_LIST): three custom games (Rooftop Parkour 23, Mini Burg
# 9 + 5 = 14, 1v1 Arena 7) and one public game (not counted: not custom)
OURS = {"totalPlayerCount": 60, "games": [
    ["CHITOWN1:aaa1", "CH", 23, 40, {"i": "Rooftop Parkour", "g": 0, "c": 1, "cm": 1001}, 0],
    ["CHITOWN1:aaa3", "CH", 9, 10, {"i": "Mini Burg", "g": 0, "c": 1, "cm": 1002}, 0],
    ["CHITOWN2:aaa5", "CH", 5, 10, {"i": "Mini Burg", "g": 0, "c": 1, "cm": 1002}, 0],
    ["SV1:aaa4", "SV", 7, 8, {"i": "1v1 Arena", "g": 0, "c": 1, "cm": 1003}, 0],
    ["SV1:off1", "SV", 6, 8, {"i": "burg", "g": 0, "host": "sv1.example", "port": 7777}, 300],
]}

# krunker.io's /game-list (v10.js:211417-211436): three custom games (Krunk_Plaza 41, TowerWar_3 31, Littletown 21)
# and a public lobby
KRUNKER = {"success": True, "totalPlayerCount": 137, "games": [
    ["FRA:aaaaa", "de-fra", 5, 8, {"i": "Burg", "g": 0}, 120],
    ["FRA:cc111", "de-fra", 41, 60, {"i": "Krunk_Plaza", "g": 24, "c": 1, "cm": 160001}, 0],
    ["SIN:cc222", "sgp", 31, 40, {"i": "TowerWar_3", "g": 1, "c": 1, "cm": 148572}, 0],
    ["NY:cc333", "us-nj", 21, 30, {"i": "Littletown", "g": 0, "c": 1, "cm": 160002}, 0],
]}


# a real 88x50 JPEG (streams_test.py's stub CDN preview): every card's picture
THUMB = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAoHBwgHBgoICAgLCgoLDhgQDg0NDh0VFhEYIx8lJCIfIiEmKzcvJik0KSEiMEExNDk7Pj4+JS5ESUM8SDc9Pjv/"
    "2wBDAQoLCw4NDhwQEBw7KCIoOzs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozs7Ozv/wAARCAAyAFgDASIAAhEBAxEB/8QAFwAB"
    "AQEBAAAAAAAAAAAAAAAAAAMCBf/EABUQAQEAAAAAAAAAAAAAAAAAAAAS/8QAFgEBAQEAAAAAAAAAAAAAAAAAAAUE/8QAFREBAQAAAAAAAAAAAAAAAAAAABP/"
    "2gAMAwEAAhEDEQA/AOBJKkkrVVKKckqSSVIpySpJJUinJKkklSKckqSSVIpyKSFSKskqSSl1V4pySpJJUinJKkklSKckqSSVIpySpJJUinIpIVIqSSpJKXVX"
    "inJKkklSKckqSSVIpySpJJUinJKkklSKcikhUi2AxtgAAAAAAAAAD//Z")


def picture(cdn_port, name):
    return "http://127.0.0.1:%d/previews/%s-440x248.jpg" % (cdn_port, name)


def streams_answer(username, cdn_port):
    """Three live streams (the overlay's three cards), each with its picture on the stub CDN; the first is `username`."""
    names = [username, username + "_2", username + "_3"]
    return {"success": True, "data": {"streams": [
        {"username": name, "title": "Live from " + name, "viewerCount": views, "thumbnailUrl": picture(cdn_port, name),
         "languageCode": "en"} for name, views in zip(names, (1250, 12, 7))],
        "promo": False, "dropCampaigns": []}}


class Counting(BaseHTTPRequestHandler):
    """Answers `routes` (path prefix -> JSON) and counts every request by path prefix."""
    routes = {}
    hits = {}
    lock = threading.Lock()

    def do_GET(self):  # noqa: N802
        for prefix, body in self.routes.items():
            if self.path.startswith(prefix):
                with self.lock:
                    self.hits[prefix] = self.hits.get(prefix, 0) + 1
                data = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
        self.send_error(404)

    def log_message(self, *args):
        pass


class Cdn(BaseHTTPRequestHandler):
    """The stub CDN: the cards' pictures (/previews/<name>-440x248.jpg). Counts every request by path."""
    hits = {}
    lock = threading.Lock()

    def do_GET(self):  # noqa: N802
        if not (self.path.startswith("/previews/") and self.path.endswith("-440x248.jpg")):
            self.send_error(404)
            return
        with self.lock:
            self.hits[self.path] = self.hits.get(self.path, 0) + 1
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(THUMB)))
        self.end_headers()
        self.wfile.write(THUMB)

    def log_message(self, *args):
        pass


class Brain(Counting):
    routes = {}  # main() sets them (the answers name the CDN's port)
    hits = {}


class Krunker(Counting):
    routes = {}
    hits = {}


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def fold(s):
    return re.sub(r"\s+", " ", s or "").strip()


def run_native(script, brain_port, kio_port, timeout=240):
    server_list = os.path.join(WORK, "servers.json")
    with open(server_list, "w") as f:
        json.dump(OURS, f)
    path = os.path.join(WORK, "native.script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, "native.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS,
                # the brain stub (an explicit URL counts as available): its streams; the Box Office lobbies are the fixed list
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_SERVER_LIST": server_list,
                # krunker.io's stub: its matchmaker (the lobby list) and its data API (the streams)
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % kio_port,
                "KRUNKNATIVE_KRUNKER_DATA_API": "http://127.0.0.1:%d" % kio_port, "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io",
                # the pictures are fetched afresh every run (a cached file would skip the CDN stub)
                "KRUNKNATIVE_REMOTE_IMAGE_CACHE": os.path.join(WORK, "remote-images")})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(False, f"client_bin finished within {timeout} s")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def text(log, label):
    """The first match's text (the cards' material icon ligature, play_arrow, left out)."""
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return (int(m.group(1)), fold(re.sub(r"play_arrow", " ", m.group(2)))) if m else (-1, None)


def nodes(log, label):
    """How many elements the selector matches (-1 when the command did not run)."""
    m = re.search(r"\[input_script\] nodes t=\d+ " + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


def grep(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=\S+ n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ " + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def attr_src(log, label):
    """The first card's picture (its <img src>; "-" when there is none)."""
    m = re.search(r"\[input_script\] attr t=\d+ " + re.escape(label) + r" n=\d+ src=(\S*)", log)
    return m.group(1) if m else None


def phase(t, tag):
    """The checks of one Server Type at time t: the rail's text and card count, which list's names it holds, the overlay,
    the first card's picture (its src, its box, and a frame of the page: the picture is drawn)."""
    return ("%d text @%s_rail [id=subLogoButtons] .popRailCards\n"
            "%d nodes @%s_cards .popRailCard\n"
            "%d grep @%s_ours Rooftop [id=subLogoButtons] .popRailName\n"
            "%d grep @%s_kio Krunk_Plaza [id=subLogoButtons] .popRailName\n"
            "%d vis @%s_overlay knStreams\n"
            "%d text @%s_stream [id=knStreams] .stream-username\n"
            "%d attr @%s_thumb src [id=knStreamThumb0]\n"
            "%d rect @%s_box knStreamThumb0\n"
            "%d frames %s 1\n"
            "%d rects @%s_grid [id=knStreams] .streams-grid\n"
            "%d rects @%s_cards [id=knStreams] .stream-card\n"
            "%d rect @%s_ovbox knStreams\n"
            % (t, tag, t + 5, tag, t + 10, tag, t + 20, tag, t + 30, tag, t + 40, tag, t + 45, tag,
               t + 46, tag, t + 47, os.path.join(WORK, tag),
               t + 100, tag, t + 100, tag, t + 100, tag))


def boxes(log, label):
    """A rects probe: (match count, [(x, y, w, h)]) in window pixels, or None when the command did not run."""
    m = re.search(r"\[input_script\] rects t=\d+ " + re.escape(label) + r" n=(\d+)((?: \S+@\S+)*)", log)
    if not m:
        return None
    return int(m.group(1)), [tuple(float(v) for v in tok.split("@", 1)[1].split(",")) for tok in m.group(2).split()]


def rect_box(log, label):
    """A rect probe (label, then the page id): (x, y, w, h, offsetWidth, offsetHeight), the box in window pixels then the
    layout size; or None."""
    m = re.search(r"\[input_script\] rect t=\d+ " + re.escape(label) + r" [^=\s]+=([\d.]+),([\d.]+),([\d.]+),([\d.]+),([\d.]+),([\d.]+)", log)
    return tuple(float(v) for v in m.groups()) if m else None


def overlay_fits(log, tag):
    """(ok, detail): the three cards sit in one row inside the grid's content box (the grid's 4 px padding-right is not
    card space), and the grid is no taller than its cards. A horizontal scrollbar under the cards is a strip that makes
    the grid taller, and cards past the content box are what makes one appear."""
    g, c, ov = boxes(log, "@%s_grid" % tag), boxes(log, "@%s_cards" % tag), rect_box(log, "@%s_ovbox" % tag)
    if not g or not c or not ov or g[0] != 1 or c[0] != 3 or len(g[1]) != 1 or len(c[1]) != 3 or not ov[4]:
        return False, f"the probes did not run (grid {g}, cards {c}, overlay {ov})"
    gx, _, gw, gh = g[1][0]
    k = ov[2] / ov[4]  # window pixels per layout pixel (the page is scaled to the window)
    one_row = all(abs(y - c[1][0][1]) < 0.6 for _, y, _, _ in c[1])
    inside = all(x + w <= gx + gw - 4 * k + 0.6 for x, _, w, _ in c[1])
    no_strip = gh <= c[1][0][3] + 0.6
    return one_row and inside and no_strip, (f"one row {one_row}, inside the content box {inside}, "
                                             f"grid {gh:.1f} px for cards {c[1][0][3]:.1f} px")


def picture_drawn(log, tag):
    """The first card's picture is on the page: in the frame at its box, the upper part (above the viewer badge) is not
    the flat placeholder. A card with no picture, or one whose picture never repaints, reads as one flat colour."""
    m = re.search(r"\[input_script\] rect t=\d+ @%s_box knStreamThumb0=([\d.]+),([\d.]+),([\d.]+),([\d.]+)" % tag, log)
    frames = sorted(f for f in os.listdir(WORK) if f.startswith(tag + "_") and f.endswith(".png")) if os.path.isdir(WORK) else []
    if not m or not frames:
        return False
    x, y, bw, bh = (float(v) for v in m.groups())
    w, h, channels, rows = read_rgba(os.path.join(WORK, frames[0]))
    x0, x1 = max(0, int(x + 0.1 * bw)), min(w, int(x + 0.9 * bw))
    y0, y1 = max(0, int(y + 0.05 * bh)), min(h, int(y + 0.45 * bh))
    values = []
    for py in range(y0, y1, 2):
        row = rows[py]
        for px in range(x0, x1, 2):
            values.extend(row[px * channels:px * channels + 3])
    if len(values) < 30:
        return False
    mean = sum(values) / len(values)
    return (sum((v - mean) ** 2 for v in values) / len(values)) ** 0.5 > 8.0


def main():
    cdn = ThreadingHTTPServer(("127.0.0.1", 0), Cdn)
    cdn_port = cdn.server_address[1]
    # /streams/thumb is answered too, so a request for it is counted (the client asks for pictures from their thumbnailUrl)
    Brain.routes = {"/streams/top": streams_answer("box_office_live", cdn_port),
                    "/streams/thumb": {"success": True, "data": {"url": "", "image": ""}}}
    Krunker.routes = {"/game-list": KRUNKER, "/streams/top": streams_answer("kio_live", cdn_port)}
    brain = ThreadingHTTPServer(("127.0.0.1", 0), Brain)
    kio = ThreadingHTTPServer(("127.0.0.1", 0), Krunker)
    for s in (cdn, brain, kio):
        threading.Thread(target=s.serve_forever, daemon=True).start()

    # Box Office servers (the default Server Type) first; then each other type through the settings handler
    script = "0 menu\n" + phase(5000, "bo")
    script += "5200 call setSetting \"knSeekServers\" \"official\"\n" + phase(8000, "kio")
    script += "8200 call setSetting \"knSeekServers\" \"both\"\n" + phase(11000, "both")
    script += "11200 call setSetting \"knSeekServers\" \"kn\"\n" + phase(14000, "bo2")
    script += "14300 quit\n"
    log = run_native(script, brain.server_address[1], kio.server_address[1])
    brain.shutdown()
    kio.shutdown()
    cdn.shutdown()

    # Box Office servers: Box Office's three maps by players, none of krunker.io's
    n, rail = text(log, "@bo_rail")
    check(n == 1 and rail == "Rooftop Parkour 23 playing Mini Burg 14 playing 1v1 Arena 7 playing",
          f"Box Office servers: Popular Now is Box Office's games by players ({rail!r})")
    check(nodes(log, "@bo_cards") == 3, "Box Office servers: Popular Now has three cards")
    check(grep(log, "@bo_kio") == 0, "Box Office servers: none of krunker.io's maps in Popular Now")
    check(grep(log, "@bo_ours") == 1, "Box Office servers: Box Office's busiest map is a card")
    check(vis(log, "@bo_overlay").get("knStreams") == "1", "Box Office servers: the Live Streams overlay shows")
    _, stream = text(log, "@bo_stream")
    check(stream == "box_office_live", f"Box Office servers: the streams are the brain's ({stream!r})")
    check(Brain.hits.get("/streams/top", 0) >= 1, "Box Office servers: the brain's /streams/top was asked")

    # krunker.io servers: Popular Now is hidden (no card, none of either list's maps); the overlay shows krunker.io's streams
    check(nodes(log, "@kio_cards") == 0, "krunker.io servers: Popular Now is hidden, no card")
    check(grep(log, "@kio_kio") == 0, "krunker.io servers: krunker.io's custom games are not in Popular Now")
    check(grep(log, "@kio_ours") == 0, "krunker.io servers: none of Box Office's maps in Popular Now")
    check(vis(log, "@kio_overlay").get("knStreams") == "1", "krunker.io servers: the Live Streams overlay shows")
    _, stream = text(log, "@kio_stream")
    check(stream == "kio_live", f"krunker.io servers: the streams are krunker.io's, v10's host ({stream!r})")
    check(Krunker.hits.get("/streams/top", 0) >= 1, "krunker.io servers: krunker.io's /streams/top was asked")

    # Both: Popular Now hidden too; the overlay shows the brain's streams while there is one
    check(nodes(log, "@both_cards") == 0, "Both: Popular Now is hidden, no card")
    check(grep(log, "@both_kio") == 0, "Both: krunker.io's custom games are not in Popular Now")
    check(vis(log, "@both_overlay").get("knStreams") == "1", "Both: the Live Streams overlay shows")
    _, stream = text(log, "@both_stream")
    check(stream == "box_office_live", f"Both: the streams are the brain's while there is one ({stream!r})")

    # Box Office again, krunker.io's list cached: Box Office's games only, the brain's streams
    n, rail = text(log, "@bo2_rail")
    check(n == 1 and rail == "Rooftop Parkour 23 playing Mini Burg 14 playing 1v1 Arena 7 playing",
          f"Box Office servers again: Box Office's games only ({rail!r})")
    check(grep(log, "@bo2_kio") == 0, "Box Office servers again: none of krunker.io's maps")
    _, stream = text(log, "@bo2_stream")
    check(stream == "box_office_live", f"Box Office servers again: the brain's streams ({stream!r})")

    # The first card's picture on every Server Type: its <img src> is the answer's thumbnailUrl (v10's markup), the CDN
    # serves it, and the engine has it ("[image] <url>: arrived" is the picture reaching the page: it draws)
    bo_pic, kio_pic = picture(cdn_port, "box_office_live"), picture(cdn_port, "kio_live")
    for tag, want, name in (("bo", bo_pic, "Box Office servers"), ("kio", kio_pic, "krunker.io servers"),
                            ("both", bo_pic, "Both"), ("bo2", bo_pic, "Box Office servers again")):
        src = attr_src(log, "@%s_thumb" % tag)
        check(src == want, f"{name}: the first card's picture is its thumbnailUrl ({src!r})")
        path = want[want.index("/previews/"):]
        check(Cdn.hits.get(path, 0) >= 1, f"{name}: the CDN's picture was fetched")
        check(("[image] %s: arrived" % want) in log, f"{name}: the picture reached the engine")
        check(picture_drawn(log, tag), f"{name}: the picture is drawn on the card (a frame of the page)")

    # The three cards fit the overlay on every Server Type: no horizontal scrollbar under them
    for tag, name in (("bo", "Box Office servers"), ("kio", "krunker.io servers"), ("both", "Both"), ("bo2", "Box Office servers again")):
        ok, detail = overlay_fits(log, tag)
        check(ok, f"{name}: the three cards fit the Live Streams overlay, no horizontal scrollbar ({detail})")
    check(Brain.hits.get("/streams/thumb", 0) == 0, "the client never asks the brain's /streams/thumb for a picture")

    if failures:
        print(f"menu_sources_e2e: {len(failures)} failed (logs in {WORK})")
        sys.exit(1)
    shutil.rmtree(WORK, ignore_errors=True)
    print("menu_sources_e2e: all checks passed")


if __name__ == "__main__":
    main()
