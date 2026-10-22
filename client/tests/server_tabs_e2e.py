#!/usr/bin/env python3
"""The Servers window's two tabs, one per backend (servers_ui.c), and the tab's "+ Host Game" (host_flow_ui.c).

  python3 -I client/tests/server_tabs_e2e.py BUILD_DIR      (from the repository root)

  - the tabs are krunk.io (Box Office's lobbies: KRUNKNATIVE_SERVER_LIST) and krunker.io (its official lobbies:
    a stub matchmaker's /game-list, KRUNKNATIVE_KRUNKER_MATCHMAKER); the default tab is the active backend's
    (krunk.io while no official krunker.io game is joined); switchTab 0 / 1 select them;
  - each tab is v10's layout of its own list: the map carousel of its published-map customs, Official Customs,
    Custom Games, then the regions. krunk.io never shows a krunker.io row and krunker.io never one of ours;
  - "+ Host Game" on a tab opens the Host Game window on that tab's backend: krunker.io's Start Game says it needs
    a krunker.io game first (no official connection; the session stays Box Office's), krunk.io's goes to the brain
    (no brain here: "Box Office servers are unavailable").
Exit code 0 when every check passed; the input-script log is kept in ~/.cache/kn-server-tabs-e2e-* on failure.
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
# the asset tree: this checkout's, else the main checkout's (a worktree may lack the large content: fonts, models)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets")) if os.path.isdir(os.path.join(d, "models"))),
    os.path.join(ROOT, "assets"))
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-server-tabs-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []

# Box Office's lobbies (the fixed list, KRUNKNATIVE_SERVER_LIST): two regions and three published-map customs
OURS = {"totalPlayerCount": 14, "games": [
    ["CHITOWN1:ab12c", "CH", 6, 8, {"i": "kanji", "g": 0, "host": "chi1.example", "port": 7777}, 300],
    ["CHITOWN2:cd34e", "CH", 3, 8, {"i": "sandstorm", "g": 1, "host": "chi2.example", "port": 7777}, 200],
    ["SV1:ef56g", "SV", 5, 8, {"i": "burg", "g": 0, "host": "sv1.example", "port": 7777}, 120],
    ["CHITOWN1:cc111", "CH", 4, 10, {"i": "Zombie_Bulwark", "g": 6, "c": 1, "cm": 114845, "h": "Zombie Bulwark Lobby"}, 0],
    ["CHITOWN2:cc222", "CH", 3, 10, {"i": "TowerWar_3", "g": 1, "c": 1, "cm": 148572}, 0],
    ["SV1:cc333", "SV", 2, 10, {"i": "Trench_War_H1", "g": 19, "c": 1, "cm": 158288}, 0],
]}

# krunker.io's /game-list (v10.js:211417-211436): [id, region, players, max, data, timeLeft]
KRUNKER = {"success": True, "totalPlayerCount": 21, "games": [
    ["FRA:aaaaa", "de-fra", 5, 8, {"i": "Burg", "g": 0}, 120],
    ["NY:bbbbb", "us-nj", 3, 8, {"i": "Sandstorm", "g": 1}, 200],
    ["NY:oc111", "us-nj", 6, 20, {"i": "Shipyard", "g": 2, "oc": 1, "ds": 1, "h": "Official Shipyard 24/7"}, 0],
    ["SV:cust1", "us-ca-sv", 2, 8, {"i": "Burg", "g": 0, "c": 1, "h": "Somebody's game"}, 0],
    ["FRA:cc222", "de-fra", 4, 10, {"i": "TowerWar_3", "g": 1, "c": 1, "cm": 148572}, 0],
    ["FRA:cc333", "de-fra", 3, 10, {"i": "Trench_War_H1", "g": 19, "c": 1, "cm": 158288}, 0],
    ["NY:cc444", "us-nj", 5, 10, {"i": "Zombie_Bulwark", "g": 6, "c": 1, "cm": 114845}, 0],
]}


class Stub(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if not self.path.startswith("/game-list"):
            self.send_error(404)
            return
        body = json.dumps(KRUNKER).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def fold(s):
    return re.sub(r"\s+", " ", s or "").strip()


# A request the client logged to a host that is not this test's stub: the finder (matchmaker.krunk.io by default),
# the brain, krunker.io. A test never reaches production; a hit here fails the run.
def production_sends(log):
    return [l.strip() for l in log.splitlines()
            if re.search(r"^\[(http|finder|krunker)\].*https?://(?!127\.0\.0\.1|localhost)", l)]


def run_native(script, stub_port, timeout=240):
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
                # the server finder is off: its default is matchmaker.krunk.io (production); a test never reaches it
                "KRUNKNATIVE_FINDER_URL": "off",
                # no brain here: the Box Office list is the fixed one, and its requests are refused
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:9", "KRUNKNATIVE_SERVER_LIST": server_list,
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % stub_port, "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io"})
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
        text = f.read()
    prod = production_sends(text)
    check(not prod, f"no request went to a production host ({prod[:2]})")
    return text


def group_name(s):
    """A group header's name: its text without the expand icon and the online count."""
    m = re.match(r"^(?:navigate_next|expand_more)?\s*(.*?)\s*\d+\s*online", fold(s))
    return m.group(1) if m else fold(s)


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return (int(m.group(1)), m.group(2)) if m else (-1, None)


def nodes(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ " + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


def grep(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=\S+ n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


def host_steps(t, tab):
    """From the tab's "+ Host Game": the Host Game window, the map, Start Game (v10's button in the tab row), the
    message read back, and the Servers window again. Returns the time after the last step."""
    script = ("%d clickid hostGameBtn\n%d call knHostFlow \"map\" \"kanji\"\n%d clickid startServBtn\n%d text @%s_msg [id=hostGameMsg]\n"
              "%d call showWindow 2\n" % (t, t + 300, t + 400, t + 600, tab, t + 800))
    return script, t + 1000


def main():
    stub = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=stub.serve_forever, daemon=True).start()
    port = stub.server_address[1]

    # krunk.io is the default tab (no official krunker.io game): its groups, then krunker.io's tab
    # krunk.io is the default tab (no official krunker.io game): its groups, then + Host Game on it
    script = ("0 menu\n300 call showWindow 2\n2500 grep @default_krunk krunk.io .bigMenTabA\n"
              "2600 text @krunk_first [id=serverHolder] .setHed\n"
              "2700 nodes @krunk_kio [id=serverHolder] .kioHed\n2710 nodes @krunk_kiorow [id=serverHolder] .kioRow\n"
              "2720 call setExpandedRegion 0\n2728 nodes @krunk_rows [id=serverHolder] .settNameIn\n2730 nodes @krunk_slides .purejscarousel .slide\n"
              "2740 grep @krunk_custom Chicago [id=serverHolder]\n"
              "2745 text @krunk_cnt [id=playerCnt]\n")
    part, t = host_steps(2800, "krunk")  # + Host Game on krunk.io: the Box Office step (no brain here)
    script += part
    # krunker.io's tab (switchTab 1): its own groups, then + Host Game on it
    script += ("%d call windows.1.switchTab 1\n%d grep @default_kio krunker.io .bigMenTabA\n"
               "%d text @kio_first [id=serverHolder] .setHed\n"
               "%d nodes @kio_kio [id=serverHolder] .kioHed\n%d call setExpandedRegion 1\n"
               "%d nodes @kio_kiorow [id=serverHolder] .kioRow\n%d nodes @kio_slides .purejscarousel .slide\n"
               "%d grep @kio_custom Somebody [id=serverHolder]\n%d grep @kio_chi Chicago [id=serverHolder]\n"
               "%d text @kio_cnt [id=playerCnt]\n"
               % (t, t + 50, t + 100, t + 120, t + 140, t + 160, t + 170, t + 180, t + 190, t + 195))
    part, t = host_steps(t + 300, "kio")  # + Host Game on krunker.io: its Start Game needs a krunker.io game first
    script += part
    # back to krunk.io: its own count again (the krunker.io count is not kept on the krunk.io tab)
    script += "%d call windows.1.switchTab 0\n%d text @krunk_back [id=playerCnt]\n" % (t + 100, t + 150)
    script += "%d quit\n" % (t + 200)
    log = run_native(script, port)
    stub.shutdown()

    # tabs: the default is krunk.io (the active backend is Box Office's)
    check(grep(log, "@default_krunk") == 1, "the default tab is krunk.io (the session's backend)")
    _, first = text(log, "@krunk_first")
    check(group_name(first) == "Custom Games", f"krunk.io: the groups start with Custom Games ({first!r})")
    check(nodes(log, "@krunk_kio") == 0, "krunk.io: no krunker.io group (Official Customs / Custom Games of krunker.io)")
    check(nodes(log, "@krunk_kiorow") == 0, "krunk.io: no krunker.io row")
    check(nodes(log, "@krunk_rows") >= 3, f"krunk.io: our rows are listed ({nodes(log, '@krunk_rows')})")
    check(nodes(log, "@krunk_slides") >= 3, f"krunk.io: the map carousel of our published-map customs ({nodes(log, '@krunk_slides')})")
    check(grep(log, "@krunk_custom") >= 1, "krunk.io: Chicago's group holds our lobby")
    # the In game counter is the tab's website's: Box Office's 14 (the brain's total) on krunk.io, krunker.io's 21 on krunker.io
    _, cnt_k = text(log, "@krunk_cnt")
    check(fold(cnt_k) == "14 In Game", f"krunk.io: the In Game count is Box Office's own 14 ({fold(cnt_k)!r})")

    # krunker.io's tab: its own groups only, Official Customs first, then Custom Games, then the regions
    check(grep(log, "@default_kio") == 1, "switchTab 1: the krunker.io tab is selected")
    _, kfirst = text(log, "@kio_first")
    check(group_name(kfirst) == "Official Customs", f"krunker.io: the groups start with Official Customs ({kfirst!r})")
    check(nodes(log, "@kio_kio") >= 3, f"krunker.io: its groups carry kioHed ({nodes(log, '@kio_kio')})")
    check(nodes(log, "@kio_kiorow") >= 4, f"krunker.io: its Custom Games rows are listed ({nodes(log, '@kio_kiorow')})")
    check(nodes(log, "@kio_slides") >= 3, f"krunker.io: the map carousel of its published-map customs ({nodes(log, '@kio_slides')})")
    check(grep(log, "@kio_custom") >= 1, "krunker.io: its custom game is in its Custom Games group")
    check(grep(log, "@kio_chi") == 0, "krunker.io: none of our (Chicago) rows")
    _, cnt_o = text(log, "@kio_cnt")
    check(fold(cnt_o) == "21 In Game", f"krunker.io: the In Game count is krunker.io's own 21 ({fold(cnt_o)!r})")
    _, cnt_back = text(log, "@krunk_back")
    check(fold(cnt_back) == "14 In Game", f"switching back to krunk.io swaps the count back to 14 ({fold(cnt_back)!r})")

    # + Host Game: the window's backend is the tab's
    _, msg_k = text(log, "@krunk_msg")
    # no brain here: the request went to the brain (/mm/host-game) and its refusal shows v10's generic line
    check("/mm/host-game" in log and any(s in fold(msg_k or "") for s in ("Box Office servers are unavailable", "Could not reach the matchmaker",
          "Creating Game", "Please try again")),
          f"krunk.io's Start Game goes to the brain ({fold(msg_k)!r})")
    _, msg_o = text(log, "@kio_msg")
    check("Join a krunker.io game first" in fold(msg_o or ""),
          f"krunker.io's Start Game says it needs a krunker.io game first, not a silent failure ({fold(msg_o)!r})")
    check("custom game: request sent" not in log, "no krunker.io custom game was sent")
    check("joining official game" not in log, "the session backend did not change (no krunker.io game joined)")

    if failures:
        print(f"server_tabs_e2e: {len(failures)} failed (logs in {WORK})")
        sys.exit(1)
    shutil.rmtree(WORK, ignore_errors=True)
    print("server_tabs_e2e: all checks passed")


if __name__ == "__main__":
    main()
