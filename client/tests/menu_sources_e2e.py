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
  - krunker.io servers: Popular Now shows krunker.io's three maps and none of ours; the overlay shows krunker.io's;
  - Both: both lists summed (krunker.io's two busiest, then our busiest); the brain's stream;
  - back to Box Office with krunker.io's list now cached: ours only again.
Before the fix the Box Office menu summed both lists (krunker.io's "41 playing" counts on a Box Office menu) and the
streams were the brain's for every Server Type. Exit code 0 when every check passed; the input-script log is kept in
~/.cache/kn-menu-sources-e2e-* on failure.
"""
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


def streams_answer(username):
    return {"success": True, "data": {"streams": [
        {"username": username, "title": "Live from " + username, "viewerCount": 1250, "thumbnailUrl": "", "languageCode": "en"}],
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


class Brain(Counting):
    routes = {"/streams/top": streams_answer("box_office_live")}
    hits = {}


class Krunker(Counting):
    routes = {"/game-list": KRUNKER, "/streams/top": streams_answer("kio_live")}
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
                "KRUNKNATIVE_KRUNKER_DATA_API": "http://127.0.0.1:%d" % kio_port, "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io"})
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


def grep(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=\S+ n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ " + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def phase(t, tag):
    """The checks of one Server Type at time t: the rail's text, which list's names it holds, the overlay."""
    return ("%d text @%s_rail [id=subLogoButtons] .popRailCards\n"
            "%d grep @%s_ours Rooftop [id=subLogoButtons] .popRailName\n"
            "%d grep @%s_kio Krunk_Plaza [id=subLogoButtons] .popRailName\n"
            "%d vis @%s_overlay knStreams\n"
            "%d text @%s_stream [id=knStreams] .stream-username\n"
            % (t, tag, t + 10, tag, t + 20, tag, t + 30, tag, t + 40, tag))


def main():
    brain = ThreadingHTTPServer(("127.0.0.1", 0), Brain)
    kio = ThreadingHTTPServer(("127.0.0.1", 0), Krunker)
    for s in (brain, kio):
        threading.Thread(target=s.serve_forever, daemon=True).start()

    # Box Office servers (the default Server Type): checked after the 3 s at which Popular Now would ask krunker.io's
    # list (POPULAR_KIO_DELAY_MS) had it counted it; then each other type through the settings handler
    script = "0 menu\n" + phase(5000, "bo")
    script += "5200 call setSetting \"knSeekServers\" \"official\"\n" + phase(8000, "kio")
    script += "8200 call setSetting \"knSeekServers\" \"both\"\n" + phase(11000, "both")
    script += "11200 call setSetting \"knSeekServers\" \"kn\"\n" + phase(14000, "bo2")
    script += "14300 quit\n"
    log = run_native(script, brain.server_address[1], kio.server_address[1])
    brain.shutdown()
    kio.shutdown()

    # Box Office servers: Box Office's three maps by players, none of krunker.io's, and krunker.io not asked for them
    n, rail = text(log, "@bo_rail")
    check(n == 1 and rail == "Rooftop Parkour 23 playing Mini Burg 14 playing 1v1 Arena 7 playing",
          f"Box Office servers: Popular Now is Box Office's games by players ({rail!r})")
    check(grep(log, "@bo_kio") == 0, "Box Office servers: none of krunker.io's maps in Popular Now")
    check(grep(log, "@bo_ours") == 1, "Box Office servers: Box Office's busiest map is a card")
    check(vis(log, "@bo_overlay").get("knStreams") == "1", "Box Office servers: the Live Streams overlay shows")
    _, stream = text(log, "@bo_stream")
    check(stream == "box_office_live", f"Box Office servers: the streams are the brain's ({stream!r})")

    # krunker.io servers: krunker.io's three maps, none of ours, krunker.io's streams
    n, rail = text(log, "@kio_rail")
    check(n == 1 and rail == "Krunk_Plaza 41 playing TowerWar_3 31 playing Littletown 21 playing",
          f"krunker.io servers: Popular Now is krunker.io's custom games by players ({rail!r})")
    check(grep(log, "@kio_ours") == 0, "krunker.io servers: none of Box Office's maps in Popular Now")
    check(vis(log, "@kio_overlay").get("knStreams") == "1", "krunker.io servers: the Live Streams overlay shows")
    _, stream = text(log, "@kio_stream")
    check(stream == "kio_live", f"krunker.io servers: the streams are krunker.io's, v10's host ({stream!r})")
    check(Krunker.hits.get("/streams/top", 0) >= 1, "krunker.io servers: krunker.io's /streams/top was asked")
    check(Brain.hits.get("/streams/top", 0) >= 1, "Box Office servers: the brain's /streams/top was asked")

    # Both: the two lists summed (krunker.io's two busiest, then our busiest); the brain's streams
    n, rail = text(log, "@both_rail")
    check(n == 1 and rail == "Krunk_Plaza 41 playing TowerWar_3 31 playing Rooftop Parkour 23 playing",
          f"Both: Popular Now sums both lists ({rail!r})")
    _, stream = text(log, "@both_stream")
    check(stream == "box_office_live", f"Both: the streams are the brain's while there is one ({stream!r})")

    # Box Office again, krunker.io's list now cached: ours only
    n, rail = text(log, "@bo2_rail")
    check(n == 1 and rail == "Rooftop Parkour 23 playing Mini Burg 14 playing 1v1 Arena 7 playing",
          f"Box Office servers again: Box Office's games only, krunker.io's cached list not counted ({rail!r})")
    check(grep(log, "@bo2_kio") == 0, "Box Office servers again: none of krunker.io's maps")
    _, stream = text(log, "@bo2_stream")
    check(stream == "box_office_live", f"Box Office servers again: the brain's streams ({stream!r})")

    if failures:
        print(f"menu_sources_e2e: {len(failures)} failed (logs in {WORK})")
        sys.exit(1)
    shutil.rmtree(WORK, ignore_errors=True)
    print("menu_sources_e2e: all checks passed")


if __name__ == "__main__":
    main()
