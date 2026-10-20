#!/usr/bin/env python3
"""The Custom Games window (window 10: Home / Popular / Featured / New / Favorites / My Games / Search / Editor / Sandbox)
opens on the tab a click names, keeps it, and its tab row sits inside the window (games_window_gen, servers_ui.c). The
social hub's Maps tab (client_bin --hub p=maps, social_ui.c) has the same Popular Categories tiles and is checked too.

  python3 -I client/tests/games_window_e2e.py BUILD_DIR      (from the repository root)

Three runs of a headless client_bin. A and B have a stub brain (KRUNKNATIVE_BRAIN_URL: /api/maps per tab, /api/search) and a
stub krunker.io data API (KRUNKNATIVE_KRUNKER_DATA_API: /webhooks/general/maps/list per tab):
  A (Box Office):
    - a Popular Categories tile on Home opens the Search tab on its category: the tab row shows Search, the search
      answers the brain's cards, the window is not scrolled, and its tab row sits below the window's top edge (before the
      fix the tile called switchTab(7), Editor, with "No Games Found!", and the Home tab's scroll offset pushed the tab
      row above the window's top edge);
    - a Popular card's Play (playMap, not in the catalogue: "Game not found") keeps the window on Popular;
  B (krunker.io as the source, a krunker.io account: test hook knV10OfficialSession):
    - krunker.io refuses Popular's list (403; Home shows the same list and asks first, so the first two requests are
      refused): the tab shows "No Games found!" and no bundled catalogue (commit 820e14f4); Featured's answer shows its list;
    - Popular, shown again, asks krunker.io again and shows its list (before the fix a refused tab stayed empty);
    - back on Box Office, Popular shows Box Office's list, not krunker.io's (before the fix the tabs kept one source's rows).
  C (the social hub, --hub p=maps: the page's socket is a stub that answers its page requests, /api/maps and /api/search
  are the brain stub's; /api/search filters its rows by name and category):
    - a Popular Categories tile (Action) opens the Search tab with the category selected: the brain is asked
      /api/search?type=map&val=&cat=19 (before the fix the tile sent no category, so every map came back), and the Search
      tab lists Action's map only;
    - the Search tab's box still searches by its text (Rooftop, with the select on All: cat=-1).
Set KN_GAMES_WINDOW_SHOTS=DIR to write the window's frames there (before / after PNGs). Exit code 0 when every check
passed; the input-script log is kept in ~/.cache/kn-games-window-e2e-* on failure.
"""
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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# the asset tree: this checkout's, else the main checkout's (a worktree may lack the large content)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets")) if os.path.isdir(os.path.join(d, "models"))),
    os.path.join(ROOT, "assets"))
SHOTS = os.environ.get("KN_GAMES_WINDOW_SHOTS", "")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-games-window-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []

# Box Office's lobbies (the fixed list, KRUNKNATIVE_SERVER_LIST): the bundled Popular Now games
OURS = {"totalPlayerCount": 60, "games": [
    ["CHITOWN1:aaa1", "CH", 23, 40, {"i": "Rooftop Parkour", "g": 0, "c": 1, "cm": 1001}, 0],
    ["CHITOWN1:aaa3", "CH", 9, 10, {"i": "Mini Burg", "g": 0, "c": 1, "cm": 1002}, 0],
    ["SV1:aaa4", "SV", 7, 8, {"i": "1v1 Arena", "g": 0, "c": 1, "cm": 1003}, 0],
]}


def map_row(name, players, category, map_id):
    return {"map_name": name, "creatorname": "tester", "mod_url": "", "map_id": map_id, "map_pl": players, "map_votes": 3,
            "category": category, "map_image": "", "fav": False, "modes": [], "map_date": "2026-01-01T00:00:00Z"}


# Box Office's brain: one list per tab (index: Popular 0, Featured 1, New 2, Favorites 3, My Games 4) and the search answer
BRAIN_TABS = {
    0: [map_row("Rooftop Parkour", 900, "Parkour", 1001), map_row("Mini Burg", 700, "Action", 1002)],
    1: [map_row("Featured Burg", 500, "Action", 2001)],
    2: [map_row("Brand New Map", 40, "Puzzle", 3001)],
    3: [map_row("Fav Map", 20, "Action", 4001)],
    4: [map_row("My Map", 5, "Action", 5001)],
}
# krunker.io's lists (index 0 Popular, 1 Featured); index 0 is refused twice first (Stub.refuse)
KIO_TABS = {0: [map_row("Krunk Plaza", 300, "Action", 6001)], 1: [map_row("Skyline Lounge", 200, "Action", 6002)]}
# config.mapCats (k_map_cats in social_ui.c): a Popular Categories tile searches its name's index (Raid: 13)
MAP_CATS = ["Other", "Parkour", "Puzzle", "Trading", "Training", "Social", "Horror", "Adventure", "Battle Royale", "Realism",
            "Roleplay", "Rotation", "Racing", "Raid", "Remake", "NCR", "Zombies", "Simulation", "Relaxation", "Action", "Tycoon",
            "Mini-Games", "Single-player", "Education"]
# the hub's search answers: one map in each of two categories (the test clicks Action's tile)
HUB_SEARCH_ROWS = [map_row("Rooftop Parkour", 900, "Parkour", 1001), map_row("Mini Burg", 700, "Action", 1002)]


class Stub(BaseHTTPRequestHandler):
    """One route table per stub; `refuse` counts the first requests of each tab index that get a 403 (Home shares
    Popular's list, index 0: its request, then Popular's own). `searches` keeps every /api/search path asked."""
    tabs = {}
    refuse = {}
    seen = {}
    search_rows = None  # None: /api/search answers Popular's list for any search (the Games window's stub)
    searches = []
    lock = threading.Lock()

    def social_frames(self):
        """The hub's socket, the minimal WebSocket the native client speaks: a page request ["r", view, key, ...] is answered
        ["0", view, key, []] (io_0: the view shows, and its list loads from /api/maps); other frames are read and not answered."""
        try:
            while True:
                head = self.rfile.read(2)
                if len(head) < 2 or (head[0] & 0x0F) == 0x8:  # closed
                    return
                length = head[1] & 0x7F
                if length == 126:
                    length = struct.unpack(">H", self.rfile.read(2))[0]
                elif length == 127:
                    length = struct.unpack(">Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if head[1] & 0x80 else b"\0\0\0\0"
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self.rfile.read(length)))
                frame = json.loads(payload) if (head[0] & 0x0F) == 0x1 and payload else []
                if frame and frame[0] == "r":
                    reply = json.dumps(["0", frame[1], frame[2], []]).encode()
                    self.wfile.write(bytes([0x81, len(reply)]) if len(reply) < 126 else bytes([0x81, 126]) + struct.pack(">H", len(reply)))
                    self.wfile.write(reply)
                    self.wfile.flush()
        except (OSError, ValueError):
            return

    def search_answer(self, query):
        """The Search tab's rows: with search_rows, the name (val) and the category (cat, an index into MAP_CATS; none or
        -1 = every category) filter them; a search with neither answers every row."""
        if self.search_rows is None:
            return self.tabs.get(0, [])
        val = (query.get("val") or [""])[0].lower()
        cat = int((query.get("cat") or ["-1"])[0] or -1)
        return [r for r in self.search_rows if val in r["map_name"].lower() and (cat < 0 or r["category"] == MAP_CATS[cat])]

    def do_GET(self):  # noqa: N802
        url = urlparse(self.path)
        query = parse_qs(url.query)
        index = int((query.get("index") or ["0"])[0])
        body = None
        if url.path.startswith("/social/ws"):  # the hub's social socket (backend_native.c): the 101, then its frames
            self.send_response(101, "Switching Protocols")
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.end_headers()
            self.social_frames()
            return
        if url.path.startswith("/api/maps"):
            body = {"success": True, "index": index, "data": self.tabs.get(index, [])}
        elif url.path.startswith("/api/search"):
            with self.lock:
                self.searches.append(self.path)
            body = {"success": True, "data": self.search_answer(query)}
        elif url.path.startswith("/webhooks/general/maps/list"):  # krunker_kio_webhook_url: the data API's Games list
            with self.lock:
                self.seen[index] = self.seen.get(index, 0) + 1
                refused = self.seen[index] <= self.refuse.get(index, 0)
            if refused:
                self.send_error(403)
                return
            body = {"success": True, "data": self.tabs.get(index, [])}
        elif url.path.startswith("/game-list"):
            body = {"success": True, "totalPlayerCount": 0, "games": []}
        elif url.path.startswith("/streams/top"):
            body = {"success": True, "data": {"streams": [], "promo": False, "dropCampaigns": []}}
        if body is None:
            self.send_error(404)
            return
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def shot(name, t):
    """A page capture (frames: the page included) at time t, when KN_GAMES_WINDOW_SHOTS is set."""
    return f"{t} frames {os.path.join(SHOTS, name)} 1\n" if SHOTS else ""


def run_native(script, brain_port, kio_port, tag, timeout=240, args=("--offline", "--map", "kanji")):
    server_list = os.path.join(WORK, f"servers-{tag}.json")
    with open(server_list, "w") as f:
        json.dump(OURS, f)
    path = os.path.join(WORK, f"native-{tag}.script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, f"native-{tag}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg-" + tag),
                "XDG_STATE_HOME": os.path.join(WORK, "state-" + tag), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS,
                # the brain stub (an explicit URL counts as available); the Box Office lobbies are the fixed list
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_SERVER_LIST": server_list,
                # krunker.io's stub: its data API (the Games tabs' lists); its matchmaker is not asked for these
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % kio_port,
                "KRUNKNATIVE_KRUNKER_DATA_API": "http://127.0.0.1:%d" % kio_port, "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(False, f"client_bin ({tag}) finished within {timeout} s")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def fold(s):
    return re.sub(r"\s+", " ", s or "").strip()


def text(log, label):
    """The first match's text (icon ligatures included: the Search tab reads "search")."""
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return (int(m.group(1)), fold(m.group(2))) if m else (-1, None)


def value_of(log, label):
    """The first match's value (a select's selected option value, an input's text); None when not logged."""
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=(.*?) text=", log)
    return m.group(2) if m and int(m.group(1)) > 0 else None


def grep_hits(log, label):
    """How many of the selector's matches' text contain the word (-1 when the command did not run)."""
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=.*? n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


def rect_of(log, label, element):
    """An element's box (window coordinates, CSS px scaled to the window): x, y, w, h; None when not drawn."""
    m = re.search(r"\[input_script\] rect t=\d+ " + re.escape(label) + r" .*?" + re.escape(element) + r"=([^\s]+)", log)
    if not m or m.group(1) == "-":
        return None
    parts = m.group(1).split(",")
    return [float(p) for p in parts[:4]] if len(parts) >= 4 else None


def rects_of(log, label):
    """The first match's box of a class selector (rects): x, y, w, h; None when nothing matches."""
    m = re.search(r"\[input_script\] rects t=\d+ " + re.escape(label) + r" n=(\d+) (\S+)", log)
    if not m or int(m.group(1)) < 1 or "@" not in m.group(2):
        return None
    parts = m.group(2).split("@", 1)[1].split(",")
    return [float(p) for p in parts[:4]] if len(parts) >= 4 else None


def scroll_of(log, label, element):
    m = re.search(r"\[input_script\] scroll t=\d+ " + re.escape(label) + r" .*?" + re.escape(element) + r"=([^\s]+)", log)
    if not m or m.group(1) == "-":
        return None
    return float(m.group(1))


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ " + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def tab_below_window_top(log, box_label, tabs_label):
    """The tab row's top minus the window's top, in window pixels (v10 pads the window's top with 20 CSS px)."""
    box = rect_of(log, box_label, "menuWindow")
    tabs = rects_of(log, tabs_label)
    if not box or not tabs:
        return None
    return tabs[1] - box[1]


def box_a(t):
    """Box Office: the Home tab scrolled down, a Popular Categories tile, the Popular tab, a Popular card's Play."""
    return ('0 menu\n'
            f'{t} call showGames\n'
            f'{t + 1000} text @home [id=windowHolder] .bigMenTabA\n'
            f'{t + 1100} setscroll windowCHolder 5000\n'
            f'{t + 1200} setscroll menuWindow 5000\n'
            f'{t + 1300} clicksel .mapLICat\n'
            f'{t + 2200} text @cat_tab [id=windowHolder] .bigMenTabA\n'
            f'{t + 2210} grep @cat_hits Rooftop [id=mapList]\n'
            f'{t + 2220} rect @cat_box menuWindow\n'
            f'{t + 2230} rects @cat_tabs .bigMenTabs\n'
            f'{t + 2240} scroll @cat_scroll windowCHolder menuWindow\n'
            + shot("category", t + 2250) +
            f'{t + 2500} call windows.9.switchTab 1\n'
            f'{t + 3000} text @pop_tab [id=windowHolder] .bigMenTabA\n'
            f'{t + 3100} clicksel .hideOnHost\n'
            f'{t + 3700} text @card_tab [id=windowHolder] .bigMenTabA\n'
            f'{t + 3710} vis @card_win windowHolder\n'
            + shot("after-popular-card", t + 3720) +
            f'{t + 3800} quit\n')


def box_b(t):
    """krunker.io as the source: Popular (first answer refused), Featured, Popular again, then Box Office's Popular."""
    return ('0 menu\n'
            f'{t} call knV10OfficialSession 1\n'
            f'{t + 100} call showGames\n'
            f'{t + 1000} call windows.9.switchTab 1\n'
            f'{t + 2000} text @b_pop_tab [id=windowHolder] .bigMenTabA\n'
            f'{t + 2010} grep @b_refused found [id=mapList]\n'
            f'{t + 2020} call windows.9.switchTab 2\n'
            f'{t + 3000} grep @b_featured Skyline [id=mapList]\n'
            f'{t + 3010} call windows.9.switchTab 1\n'
            f'{t + 4000} grep @b_popular Plaza [id=mapList]\n'
            + shot("krunker-popular", t + 3990) +
            f'{t + 4010} call closWind\n'
            f'{t + 4100} call knV10OfficialSession 0\n'
            f'{t + 4200} call showGames\n'
            f'{t + 5200} grep @bo_popular Rooftop [id=mapList]\n'
            f'{t + 5210} grep @bo_stale Plaza [id=mapList]\n'
            + shot("box-office-popular", t + 5250) +
            f'{t + 5300} quit\n')


def box_c(t):
    """Box Office's social hub (--hub p=maps): the first Popular Categories tile (Action; the row scrolls sideways, the
    rest are off the window) opens the Search tab on its category; the Search box then searches by its text (the tab is
    shown again from Home, so its select is on All). The tile's centre is at window point 226,409 (the 1280x720 window)."""
    return ('0 menu\n'
            f'{t} waitfor .mapLICat\n'
            f'{t + 100} click 226 409\n'
            f'{t + 1500} text @h_tab [id=mapsHolder] .bigMenTabA\n'
            f'{t + 1510} text @h_cat [id=mpCat]\n'
            f'{t + 1520} grep @h_burg Burg [id=searchResultsMA]\n'
            f'{t + 1530} grep @h_rooftop Rooftop [id=searchResultsMA]\n'
            + shot("hub-category", t + 1540) +
            f'{t + 2000} call windows.0.switchTab 0\n'
            f'{t + 2050} call windows.0.switchTab 6\n'
            f'{t + 2100} clickid mpSrch\n'
            f'{t + 2200} type Rooftop\n'
            f'{t + 2300} clicksel .searchListBtn\n'
            f'{t + 3300} text @t_cat [id=mpCat]\n'
            f'{t + 3310} grep @t_rooftop Rooftop [id=searchResultsMA]\n'
            f'{t + 3320} grep @t_burg Burg [id=searchResultsMA]\n'
            + shot("hub-search-box", t + 3330) +
            f'{t + 3400} quit\n')


def main():
    if SHOTS:
        os.makedirs(SHOTS, exist_ok=True)
    brain = ThreadingHTTPServer(("127.0.0.1", 0), type("BrainStub", (Stub,), {"tabs": BRAIN_TABS}))
    hub_brain = ThreadingHTTPServer(("127.0.0.1", 0), type("HubBrainStub", (Stub,), {
        "tabs": BRAIN_TABS, "search_rows": HUB_SEARCH_ROWS, "searches": []}))
    kio = ThreadingHTTPServer(("127.0.0.1", 0), type("KioStub", (Stub,), {"tabs": KIO_TABS, "refuse": {0: 2}, "seen": {}}))
    kio_seen = kio.RequestHandlerClass.seen
    hub_searches = hub_brain.RequestHandlerClass.searches
    for s in (brain, hub_brain, kio):
        threading.Thread(target=s.serve_forever, daemon=True).start()

    log_a = run_native(box_a(2000), brain.server_address[1], kio.server_address[1], "a")
    log_b = run_native(box_b(2000), brain.server_address[1], kio.server_address[1], "b")
    log_c = run_native(box_c(2000), hub_brain.server_address[1], kio.server_address[1], "c",
                       args=("--offline", "--hub", "p=maps"))
    brain.shutdown()
    hub_brain.shutdown()
    kio.shutdown()

    # A: the Popular Categories tile opens Search on its category (not Editor, which index 7 was)
    n, tab = text(log_a, "@cat_tab")
    check(tab == "search", f"a category tile opens the Search tab ({tab!r})")
    check(grep_hits(log_a, "@cat_hits") >= 1, "the Search tab answers the brain's cards for the category")
    gap = tab_below_window_top(log_a, "@cat_box", "@cat_tabs")
    check(gap is not None and gap >= 8, f"the tab row sits below the window's top edge after a category ({gap})")
    check(scroll_of(log_a, "@cat_scroll", "menuWindow") == 0.0,
          f"the new tab's list starts at the top (window scroll {scroll_of(log_a, '@cat_scroll', 'menuWindow')})")

    # A: a Popular card keeps the window on its tab
    _, pop = text(log_a, "@pop_tab")
    check(pop == "Popular", f"the Popular tab is open before the card click ({pop!r})")
    _, card = text(log_a, "@card_tab")
    check(card == "Popular", f"a Popular card's Play keeps the Games window on Popular ({card!r})")
    check(vis(log_a, "@card_win").get("windowHolder") == "1", "the Games window stays open after a Popular card's Play")

    # B: krunker.io's lists: a refused first answer is shown as "No Games found!" (no catalogue), then asked again
    _, b_tab = text(log_b, "@b_pop_tab")
    check(b_tab == "Popular", f"krunker.io: the Popular tab is open ({b_tab!r})")
    check(grep_hits(log_b, "@b_refused") == 1, "krunker.io: a refused Popular list shows No Games found! (no bundled catalogue)")
    check(grep_hits(log_b, "@b_featured") == 1, f"krunker.io: Featured shows krunker.io's list (asked {kio_seen.get(1, 0)}x)")
    check(kio_seen.get(0, 0) >= 3 and grep_hits(log_b, "@b_popular") == 1,
          f"krunker.io: Popular, shown again, asks krunker.io again and shows its list (asked {kio_seen.get(0, 0)}x)")
    check(grep_hits(log_b, "@bo_popular") == 1, "Box Office again: Popular shows Box Office's list")
    check(grep_hits(log_b, "@bo_stale") == 0, "Box Office again: none of krunker.io's rows are kept in the tab")

    # C: the hub's Popular Categories tile asks the brain for its category (before the fix: no cat, every map)
    asked = [parse_qs(urlparse(p).query, keep_blank_values=True) for p in hub_searches]
    _, h_tab = text(log_c, "@h_tab")
    check(h_tab is not None and "search" in h_tab, f"hub: a category tile opens the Search tab ({h_tab!r})")
    check(any(q.get("type") == ["map"] and q.get("val") == [""] and q.get("cat") == ["19"] for q in asked),
          f"hub: the tile's search asks the brain for its category (Action, cat=19), no name (searches: {hub_searches})")
    check(value_of(log_c, "@h_cat") == "19", f"hub: the category select shows Action ({value_of(log_c, '@h_cat')!r})")
    check(grep_hits(log_c, "@h_burg") >= 1, "hub: the Search tab lists the category's map (Mini Burg)")
    check(grep_hits(log_c, "@h_rooftop") == 0, "hub: the other categories' maps are not listed (Rooftop Parkour)")
    check(any(q.get("val") == ["Rooftop"] and q.get("cat") == ["-1"] for q in asked),
          "hub: the Search box still searches by its text (cat -1: the select is All)")
    check(value_of(log_c, "@t_cat") == "-1" and grep_hits(log_c, "@t_rooftop") >= 1 and grep_hits(log_c, "@t_burg") == 0,
          "hub: the text search lists Rooftop Parkour and not Mini Burg")

    if failures:
        print(f"games_window_e2e: {len(failures)} failed (logs in {WORK})")
        sys.exit(1)
    shutil.rmtree(WORK, ignore_errors=True)
    print("games_window_e2e: all checks passed")


if __name__ == "__main__":
    main()
