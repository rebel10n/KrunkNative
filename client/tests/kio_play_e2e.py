#!/usr/bin/env python3
"""The Games window's Play of a published community map and its favourite star on a krunker.io account
(servers_ui.c kio_play_map / fav_toggle, krunker_net.c krunker_net_join_map), end to end against the krunker.io stubs of
server/tests/krunker_account_e2e.py, with this test's own matchmaker and data client (api.krunker.io's
/webhooks/general/maps/config). Nothing leaves the machine; no kn_brain.

  python3 -I client/tests/kio_play_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7's map card Play opens /?play=<name>: seek-game with regionId "any" and dataQuery {v, i: name}; a game on the
map is joined. With none (NoServersMatchQuery) the seek is made again with no map, and once that game's socket is up
autoHostMap(name) asks getMapPreset (maps/config?mn=<name>, {da, fu}) and sends CUSTOM_SERVER with customMap = the
name, defaultSetts, the preset's modes and settings (numbers clamped to the account type's max) and krRewards while the
map has funds (deob-10.0.7.js:229213-229318, :225349-225453). Nothing is downloaded: the game server sends the map's
data with the game's init. Before this, a krunker.io account's Play went to kn_brain's download (/api/map-data), which
the gate refuses while krunker.io is the backend ("Game not found.").
The star sends FAVORITE_MAPMOD ["fvM", kind, id, !!sel] on the game socket (:228231-228236).
The client starts with a stored krunker.io session, joins the stub's game, goes back to the menu and:
  host   playMap "StubWorld" (no game on it): the map seek, the seek with no map, getMapPreset, CUSTOM_SERVER, and
         the hosted game ("cust" gameId) joined
  live   playMap "LiveWorld" (a game on it): that game joined, nothing hosted
  star   favoriteM 0 this 4242, twice: ["fvM", 0, 4242, true] then ["fvM", 0, 4242, false] on the official socket
Exit 0 when every check passed, 77 when it cannot run.
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
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-play-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
MAP = "StubWorld"   # no public game on it: hosted
LIVE = "LiveWorld"  # a public game on it: joined
LIVE_GAME = "TEST:liveworld"
MAP_ID = 4242
HOSTED = "TEST:stubhost"
# the map's config as the editor saves it (getMapPreset's da: JSON text): mode 1, two classes, a server size over the
# signed-in account's max (10 for a plain account; game_config.c maxPlayers) and one setting off its default
PRESET = {"modes": [1], "classes": [0, 2], "settings": {"maxPlayers": 99, "gameTime": 7}}
FUNDS = 50
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    detail = str(detail)
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail[:400]) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


class Services:
    """The matchmaker (seek-game as the stub's, with dataQuery.i answered: LIVE has a game, MAP none) and api.krunker.io's
    data client (the map preset webhook; anything else 404s, as an unknown route does)."""

    def __init__(self, port, ws_port):
        self.port = port
        self.seeks = []    # the seek-game queries
        self.presets = []  # the maps/config queries
        owner = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, status, obj):
                body = json.dumps(obj).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                u = urllib.parse.urlsplit(self.path)
                q = dict(urllib.parse.parse_qsl(u.query))
                if u.path == "/mm/seek-game":
                    owner.seeks.append(q)
                    try:
                        dq = json.loads(q.get("dataQuery") or "{}")
                    except ValueError:
                        dq = {}
                    if dq.get("i") == MAP:
                        self.reply(400, {"error": "NoServersMatchQuery"})
                        return
                    game = LIVE_GAME if dq.get("i") == LIVE else q.get("game") or kae.GAME_PICK
                    self.reply(200, {"host": "127.0.0.1", "port": ws_port, "gameId": game, "clientId": kae.CLIENT_ID,
                                     "data": {"i": "Kanji", "g": 0}})
                elif u.path == "/mm/game-list":
                    self.reply(200, {"success": True, "totalPlayerCount": 23, "games": kae.LOBBIES})
                elif u.path == "/webhooks/general/maps/config":
                    owner.presets.append(q)
                    if q.get("mn") == MAP:
                        self.reply(200, {"da": json.dumps(PRESET), "fu": FUNDS})
                    else:
                        self.reply(200, {})
                else:
                    self.reply(404, {"error": "not found"})

        self.httpd = ThreadingHTTPServer(("127.0.0.1", port), H)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def script(shots):
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "13000 respawn",
         "14400 down ESC", "14450 up ESC",
         '15000 call playMap "%s"' % MAP,
         "26000 shot " + shots["host"],
         '26500 call playMap "%s"' % LIVE,
         "36000 shot " + shots["live"],
         "36500 call favoriteM 0 0 %d" % MAP_ID,
         "37500 call favoriteM 0 0 %d" % MAP_ID,
         "39000 quit"]
    return "\n".join(s) + "\n"


def run_client(stub, svc, shots):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script(shots))
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # a stored krunker.io session (as kio_hostmap_e2e.py): signed in at start, refreshed against the stub's FRVR auth
    for key, value in (("kio_refresh_token", kae.REFRESH1), ("kio_username", kae.ACCOUNT_NAME)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, "client.log")
    base = "http://127.0.0.1:%d" % stub.http_port
    own = "http://127.0.0.1:%d" % svc.port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:9", "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": own + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_DATA_API": own,
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off",
                "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH})
    p = subprocess.Popen(["xvfb-run", "-a", "stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(240)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client finished its script", False, "timed out; log " + log)
    return read_log(log)


def map_query(q):
    try:
        return json.loads(q.get("dataQuery") or "{}").get("i")
    except ValueError:
        return None


def checks(text, stub, svc, customs, favs, shots):
    check("the official game is the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:2])
    # host: the map seek (regionId any, dataQuery.i), NoServersMatchQuery, then a seek with no map
    i_host = next((i for i, q in enumerate(svc.seeks) if map_query(q) == MAP), -1)
    host_seek = svc.seeks[i_host] if i_host >= 0 else {}
    after = svc.seeks[i_host + 1] if 0 <= i_host < len(svc.seeks) - 1 else {}
    check("Play seeks a game on the map: region \"any\", dataQuery {v, i: the map}",
          host_seek.get("region") == "any" and "v" in json.loads(host_seek.get("dataQuery") or "{}"),
          {k: host_seek.get(k) for k in ("region", "dataQuery")})
    check("with no game on it the seek goes on with no map (v10's seek again)",
          after and map_query(after) is None and after.get("region") not in (None, "any") and not after.get("game"),
          {k: after.get(k) for k in ("region", "dataQuery", "game")} if after else svc.seeks[-3:])
    check("getMapPreset asks the data client's maps/config by the map's name, once connected",
          [q.get("mn") for q in svc.presets] == [MAP], svc.presets[:4])
    check("nothing is downloaded from kn_brain (no /api/map-data, no community fetch)",
          "/api/map-data" not in text and "[community] " not in text, re.findall(r".*(?:map-data|\[community\]).*", text)[:4])
    config = customs[0][1] if customs and len(customs[0]) > 1 and isinstance(customs[0][1], dict) else {}
    check("CUSTOM_SERVER (\"custom\") goes once on the official socket with customMap = the map's name",
          len(customs) == 1 and config.get("customMap") == MAP and config.get("rawMapData") is None and config.get("maps") == [],
          [c[:2] for c in customs][:2])
    check("autoHostMap's fields: defaultSetts, public, override 0, no password, krRewards while the map has funds",
          config.get("defaultSetts") is True and config.get("private") is False and config.get("override") == 0
          and config.get("passCode") is None and config.get("krRewards") is True,
          {k: config.get(k) for k in ("defaultSetts", "private", "override", "passCode", "krRewards")})
    check("the preset's modes and classes", config.get("modes") == PRESET["modes"] and config.get("classes") == PRESET["classes"],
          {k: config.get(k) for k in ("modes", "classes")})
    check("the preset's settings, the server size clamped to the account type's max (10)",
          config.get("maxPlayers") == 10 and config.get("gameTime") == 7, {k: config.get(k) for k in ("maxPlayers", "gameTime")})
    check("the hosted game (\"cust\" gameId) is joined", any(q.get("game") == HOSTED for q in svc.seeks),
          [q.get("game") for q in svc.seeks])
    # live: the map seek's game is joined, nothing hosted
    live = [q for q in svc.seeks if map_query(q) == LIVE]
    check("Play of a map with a game on it joins that game and hosts nothing",
          len(live) == 1 and live[0].get("region") == "any" and any(LIVE_GAME in urllib.parse.unquote(p) for p in stub.ws_paths)
          and len(customs) == 1 and len(svc.presets) == 1, (live, stub.ws_paths[-2:], len(customs), len(svc.presets)))
    check("the star sends FAVORITE_MAPMOD in v10's order: [\"fvM\", 0, id, true] then [..., false]",
          favs == [["fvM", 0, MAP_ID, True], ["fvM", 0, MAP_ID, False]], favs)
    check("the screenshots were taken", all(os.path.exists(p) for p in shots.values()), shots)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_play_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port, own_port = kn_proto.free_ports(3)
    stub = kae.Stub(http_port, ws_port)
    stub.players = []
    svc = Services(own_port, ws_port)
    customs, favs = [], []

    async def on_custom(send, raw):  # the client's CUSTOM_SERVER [name, config, rid, rehost], answered as krunker.io does
        try:
            customs.append(kae.frame_args(raw))
        except Exception as e:  # noqa: BLE001 (a frame the decoder cannot read is a failure of the check)
            customs.append(["custom", "undecodable: %s" % e])
        await send("cust", "Game Created", HOSTED, False)  # customRoomResponse(message, gameId, teams)

    async def on_fav(send, raw):
        try:
            favs.append(kae.frame_args(raw))
        except Exception as e:  # noqa: BLE001
            favs.append(["fvM", "undecodable: %s" % e])

    stub.frame_handlers["custom"] = on_custom
    stub.frame_handlers["fvM"] = on_fav
    shots = {k: os.path.join(WORK, k + ".png") for k in ("host", "live")}
    try:
        stub.start()
        text = run_client(stub, svc, shots)
        checks(text, stub, svc, customs, favs, shots)
    finally:
        stub.stop()
        svc.stop()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
