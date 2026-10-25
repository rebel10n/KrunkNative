#!/usr/bin/env python3
"""Creators end to end in the headless client: the creator badge, a map's YouTube link and the
Live Streams cards' previews (social_ui.c, menu_ui.c, v10_menu.c against kn_brain).

  python3 -I services/brain/tests/creator_e2e.py <native build dir> <repo root>

kn_brain on sqlite with Live Streams pointed at a stub Twitch and a stub CDN (streams_test.py's
stubs: never the live Twitch API or CDN). Headless client_bin runs, one at a time, driven through
KRUNKNATIVE_UI_CMDS:

  1. a developer's hub on another player's profile: the Creator button gives v10's creator badge
     ("crt": players.creator_badge = 1), the profile then shows the purple verified mark and the
     button reads Uncreator; screenshot creator-profile.png
  2. the same hub on a published map with a YouTube link: the map page shows the YouTube button,
     whose click opens https://www.youtube.com/watch?v=<id> (KRUNKNATIVE_NO_BROWSER: logged);
     screenshot map-video.png
  3. the main menu signed in as the creator: the menu box has the creator mark
     (.menuClassPlayerCreator), the Live Streams overlay shows the stub's two live streams plus
     v10's promo card, and both previews come through the brain's /streams/thumb (the stub CDN);
     screenshot menu-streams.png

Screenshots stay in the work directory (printed) with KN_KEEP=1. 77 = skip (no client_bin /
kn_brain / websockets).
"""
import asyncio
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:
    print("python websockets missing: skip")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1])
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.getcwd()
BRAIN, CLIENT = os.path.join(BUILD, "services", "brain", "kn_brain"), os.path.join(BUILD, "client_bin")
if not (os.path.exists(BRAIN) and os.path.exists(CLIENT)):
    print("needs client_bin and kn_brain in %s: skip" % BUILD)
    sys.exit(77)
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
sys.path.insert(0, HERE)
from kn_proto import free_ports  # noqa: E402
import streams_test as stubs  # noqa: E402  (the stub Twitch and CDN; its main() does not run on import)

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-creator-e2e.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PORT, GAME_PORT = free_ports(2)
SECRET = "crte2e-%d" % os.getpid()
VIDEO = "dQw4w9WgXcQ"


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def q(sql, *args):
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def wait_db(sql, args, pred, timeout=12.0):
    end = time.time() + timeout
    while time.time() < end:
        rows = q(sql, *args)
        if pred(rows):
            return rows
        time.sleep(0.25)
    return q(sql, *args)


async def social(frames_for):
    """register the accounts and publish the map over raw social sockets"""
    out = {}
    for name, extra in frames_for:
        async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
            await ws.send(json.dumps(["a", 0, [name, "social_pass_1", None]]))
            while True:
                f = json.loads(await asyncio.wait_for(ws.recv(), 20))
                if f[0] == "a":
                    assert f[1] is None, f
                    out[name] = (f[2], f[5])
                    break
            for fr, label in extra:
                await ws.send(json.dumps(fr))
                while True:
                    f = json.loads(await asyncio.wait_for(ws.recv(), 20))
                    if f[0] == label:
                        break
    return out


class Client:
    """client_bin (args), headless, signed in as name (its token in the config directory)."""

    def __init__(self, name, token, tag, args):
        self.tag = tag
        self.cmds = os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w").write(name)
        open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w").write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % PORT,
                    "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                    "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % PORT})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT] + args, stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)

    def text(self):
        try:
            return open(self.log, errors="replace").read()
        except OSError:
            return ""

    def cmd(self, line):
        open(self.cmds, "a").write(line + "\n")

    def mark(self):
        return len(self.text())

    def wait_line(self, pattern, mark, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, self.text()[mark:])
            if m:
                return m
            time.sleep(0.15)
        return None

    def rect(self, elem_id, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("rect " + elem_id)
            r = self.wait_line(r"#%s rect ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)" % re.escape(elem_id), m, 2.0)
            if r:
                return float(r.group(3)), float(r.group(4))
            time.sleep(0.2)
        return None

    def visible(self, elem_id, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            r = self.rect(elem_id, 2.0)
            if r and r[0] > 0 and r[1] > 0:
                return True
            time.sleep(0.3)
        return False

    def text_of(self, selector, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("textof " + selector)
            r = self.wait_line(r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(selector), m, 2.0)
            if r:
                return r.group(1)
            time.sleep(0.2)
        return None

    def count(self, selector, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("count " + selector)
            r = self.wait_line(r"\[ui_cmds\] count %s = (\d+)" % re.escape(selector), m, 2.0)
            if r:
                return int(r.group(1))
            time.sleep(0.2)
        return None

    def wait_count(self, selector, want, timeout=30.0):
        end = time.time() + timeout
        n = None
        while time.time() < end:
            n = self.count(selector, 4.0)
            if n == want:
                return n
            time.sleep(0.5)
        return n

    def click(self, elem_id):
        self.cmd("clickid " + elem_id)
        time.sleep(0.6)

    def ui(self, calls):
        self.cmd("ui " + calls)
        time.sleep(0.8)

    def shot(self, name):
        path = os.path.join(WORK, "%s.png" % name)
        m = self.mark()
        self.cmd("shot " + path)
        end = time.time() + 15
        while time.time() < end and not os.path.exists(path):
            time.sleep(0.2)
        time.sleep(0.3)
        return path if os.path.exists(path) else None

    def close(self):
        self.cmd("quit")
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(5)
        self.out.close()


def game_node():
    """one brain-linked v10 server_bin (the menu needs a lobby, else it shows Lost Connection)"""
    gs = os.path.join(WORK, "node.json")
    cfg = {"node_id": "CRTE2E", "name": "creator e2e", "region": "local", "listen_port": GAME_PORT,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % PORT,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0,
           "idle_kick_seconds": 0, "ruleset": "v10", "finder_url": "http://127.0.0.1:%d" % PORT}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    return subprocess.Popen([os.path.join(BUILD, "server_bin"), "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                            stdout=open(os.path.join(WORK, "node.log"), "w"), stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)


def lobby_listed():
    import urllib.request
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/mm/game-list?hostname=krunknative" % PORT, timeout=5) as r:
            return bool(json.loads(r.read().decode()).get("games"))
    except Exception:  # noqa: BLE001
        return False


def png_ok(path):
    return bool(path) and os.path.getsize(path) > 10000


def main():
    tw = stubs.Twitch()
    cdn = stubs.Cdn(256 * 1024)
    tw.data = [
        {"id": "1", "user_login": "big_streamer", "title": "Parkour all day", "viewer_count": 1250, "type": "live", "language": "en",
         "thumbnail_url": "https://static-cdn.jtvnw.net/previews-ttv/live_user_big_streamer-{width}x{height}.jpg"},
        {"id": "2", "user_login": "zeta_one", "title": "Zombie Labs", "viewer_count": 12, "type": "live", "language": "en",
         "thumbnail_url": "https://static-cdn.jtvnw.net/previews-ttv/live_user_zeta_one-{width}x{height}.jpg"},
    ]
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
               "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": SECRET, "region": "local"},
               "finder": {"poll_interval_s": 1},
               "streams": {"enabled": True, "cache_s": 30,
                           "twitch": {"auth_url": tw.url + "/oauth2/token", "api_url": tw.url + "/helix"},
                           "thumbs": {"cdn_url": cdn.url}},
               "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                            "password_iterations": 1000, "email": {"required": False},
                            "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, open(cfg, "w"))
    env = {k: v for k, v in os.environ.items() if not k.startswith("KN_STREAMS_")}
    env.update(KN_STREAMS_TWITCH_CLIENT_ID=stubs.CLIENT_ID, KN_STREAMS_TWITCH_CLIENT_SECRET=stubs.SECRET)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                             start_new_session=True)
    client = None
    node = None
    try:
        for _ in range(300):
            try:
                if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, open(os.path.join(WORK, "brain.log")).read()):
                    break
            except OSError:
                pass
            time.sleep(0.1)
        node = game_node()
        end = time.time() + 30
        while time.time() < end and not lobby_listed():
            time.sleep(0.5)
        check(lobby_listed(), "the game node's lobby is listed")
        stamp = str(int(time.time()) % 100000)
        dev_n, tgt_n, map_n = "ce2edev%s" % stamp, "ce2ecrt%s" % stamp, "CreatorMap%s" % stamp
        map_json = json.dumps({"name": map_n, "objects": [{"p": [0, 0, 0], "s": [10, 10, 10]}], "spawns": [[0, 10, 0, 0, 0, 0]]})
        acc = asyncio.run(social([(dev_n, []),
                                  (tgt_n, [(["uploadM", map_json, None, "A creator's map",
                                             "https://www.youtube.com/watch?v=%s&t=10" % VIDEO, 0], "pubMR")])]))
        dev, tgt = acc[dev_n][0], acc[tgt_n][0]
        q("UPDATE players SET player_developer = 1 WHERE player_id = ?", dev)
        q("UPDATE players SET player_score = 50000 WHERE player_id IN (?, ?)", tgt, dev)
        check(q("SELECT map_video FROM published_maps WHERE map_name = ?", map_n) == [(VIDEO,)], "the published map keeps the video id")

        print("1. the developer gives the creator badge in the hub:")
        client = Client(dev_n, acc[dev_n][1], "hub", ["--hub", "p=profile&q=" + tgt_n])
        check(client.wait_line(r"\[hub\] enter", 0, 30.0) is not None, "the hub entered its page (p=profile)")
        check(client.visible("creatorBtn", 20.0), "a developer sees the Creator button")
        check(client.text_of("creatorBtn") == "Creator", "it reads Creator (no badge yet)")
        check(client.count("#creatorBadge") == 0, "no creator mark on the profile yet")
        client.click("creatorBtn")
        check(client.visible("confBtn"), "Creator opens the confirm popup")
        client.click("confBtn")
        rows = wait_db("SELECT creator_badge FROM players WHERE player_id = ?", (tgt,), lambda r: r and r[0][0] == 1)
        check(rows and rows[0][0] == 1, "players.creator_badge = 1 (crt reached kn_brain)")
        check("SUCCESS" in (client.text_of(".responsePop") or ""), "the SUCCESS popup shows")
        client.ui("clearPops()")
        time.sleep(1.5)
        check(client.text_of("creatorBtn") == "Uncreator", "after Creator the button reads Uncreator (the profile reloaded)")
        check(client.count("#creatorBadge") == 1, "the profile shows the purple creator mark")
        p = client.shot("creator-profile")
        check(png_ok(p), "screenshot: %s" % p)

        print("2. the map page links its YouTube video:")
        client.ui("updateWindow('map','%s')" % map_n)
        check(client.visible("mapVidLink", 20.0), "the map page shows the YouTube button (#mapVidLink)")
        check("Watch on YouTube" in (client.text_of("mapVidLink") or ""), "it reads Watch on YouTube")
        p = client.shot("map-video")
        check(png_ok(p), "screenshot: %s" % p)
        m = client.mark()
        client.click("mapVidLink")
        r = client.wait_line(r"open \(disabled by KRUNKNATIVE_NO_BROWSER\): (\S+)", m, 10.0)
        check(r is not None and r.group(1) == "https://www.youtube.com/watch?v=%s" % VIDEO,
              "a click opens the video's watch page (%s)" % (r.group(1) if r else "nothing opened"))
        client.ui("updateMapTab(1, false)")
        check(not client.visible("mapVidLink", 2.0), "the Servers tab hides the video link")
        client.ui("updateMapTab(0, false)")
        check(client.visible("mapVidLink", 6.0), "About shows it again")
        client.close()
        client = None

        print("3. the main menu: the creator mark and the Live Streams previews:")
        # no Auto Seek on Launch: this run checks the menu, not a game (a failed seek would cover it
        # with the disconnect screen and hide #knStreams)
        os.makedirs(os.path.join(WORK, "menu-cfg", "krunknative"), exist_ok=True)
        open(os.path.join(WORK, "menu-cfg", "krunknative", "knSeekOnLaunch.json"), "w").write("false")
        client = Client(tgt_n, acc[tgt_n][1], "menu", [])
        n = client.wait_count(".menuClassPlayerCreator", 1, 60.0)
        check(n == 1, "the signed-in creator's menu box has the creator mark (%s)" % n)
        check(client.visible("knStreams", 40.0), "the Live Streams overlay shows")
        n = client.wait_count(".stream-card", 3, 30.0)
        check(n == 3, "two live stream cards plus v10's promo card (%s)" % n)
        n = client.wait_count(".promo-card", 1, 5.0)
        check(n == 1, "one promo card (%s)" % n)
        n = client.wait_count("#knStreams img[src^='http']", 2, 30.0)
        check(n == 2, "both cards ask the brain's /streams/thumb for the preview (%s)" % n)
        check(cdn.count("/previews-ttv/live_user_big_streamer-440x248.jpg") >= 1 and
              cdn.count("/previews-ttv/live_user_zeta_one-440x248.jpg") >= 1, "the brain fetched both previews from the (stub) CDN")
        time.sleep(1.0)
        p = client.shot("menu-streams")
        check(png_ok(p), "screenshot: %s" % p)
        client.close()
        client = None
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        if client:
            client.close()
        if node and node.poll() is None:
            node.terminate()
            try:
                node.wait(10)
            except subprocess.TimeoutExpired:
                node.kill()
        brain.terminate()
        try:
            brain.wait(10)
        except subprocess.TimeoutExpired:
            brain.kill()
        log.close()
        tw.srv.shutdown()
        cdn.srv.shutdown()
    print("creator e2e: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)) + ", files in %s" % WORK)
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
