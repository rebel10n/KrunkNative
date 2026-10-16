#!/usr/bin/env python3
"""The chat (#chatUIHolder, #chatUI) is up whenever the game is not loading, and the in-game HUD only
in a game (client/src/ui/hud_ui.c hud_chat_wanted, hud_v10.c hud_v10_tick, hud_apply_visibility).

  python3 -I server/tests/chat_ingame_gate_e2e.py BUILD_DIR      (run from the repository root)

A scratch kn_brain (sqlite) registers an account and finds one brain-linked server_bin (a lobby);
headless client_bin runs on the v10 page. The Quick Match at launch joins the lobby, and the menu
stays up with no game entered (Play is not pressed):
  1. a signed-in account: the chat is hidden while the launch's join loads (the first frame) and up
     on the lobby's idle menu from the join on, every frame after; the in-game HUD stays hidden
  2. a guest: the same
  3. F5 from that menu: the relaunched client rejoins the lobby; its first frames (loading) have no
     chat, then the chat is up on the menu, through Play, and the game shows the chat and the HUD
  4. F5 from an offline game: the game is back, with the chat and the HUD up
  5. a signed-in account in the offline game (no reload): the chat and the HUD show from the spawn on
Each probe is `vis` (html_dom_is_shown: rendered, not just display:block) on each frame.
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device
(KRUNKNATIVE_HEADLESS=1: EGL) and sqlite. Exit 77 when kn_brain or the database is unavailable.
"""
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_chatgate_%d" % os.getpid()
SECRET = "kn-chatgate-%d" % os.getpid()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-chatgate-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
# a worktree lacks the untracked font / asset files: the main checkout's (as hud_chat_e2e.py does)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
if not os.path.isfile(os.path.join(ASSETS, "css", "fonts", "font2.ttf")):
    ASSETS = "/home/user/src/KrunkNative/assets"
results = []
procs = []
PORT_NODE = kn_proto.free_ports(1)[0]
BRAIN_URL = [""]  # the scratch brain, set once it listens

# the probes of every sample: the chat, the Old Scoreboard holder, the in-game HUD
PROBES = "chatUIHolder chatUI chatList menuHolder inGameUI weapHolder signedInHeaderBar"


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def node(brain_port, node_id, game_port):
    """a brain-linked server_bin: the public FFA lobby the launch's Quick Match joins"""
    log = os.path.join(WORK, node_id + ".log")
    gs = os.path.join(WORK, node_id + ".json")
    cfg = {"node_id": node_id, "name": "chat gate " + node_id, "region": "local", "listen_port": game_port,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0,
           "idle_kick_seconds": 0, "ruleset": "v10", "finder_url": "http://127.0.0.1:%d" % brain_port}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    return p


def lobbies(port):
    code, body = http(port, "/mm/game-list?hostname=krunknative")
    return sorted(g[0] for g in body.get("games", [])) if code == 200 else []


def samples(text, label):
    """(t, {id: 0|1|-}) of every `vis @<label><t>` probe line, in order"""
    out = []
    for m in re.finditer(r"\[input_script\] vis t=(\d+) @(%s\S*) (.*)" % re.escape(label), text):
        out.append((int(m.group(1)), dict(kv.split("=", 1) for kv in m.group(3).split())))
    return out


def vis_lines(prefix, t_max, step, ids=PROBES):
    return [f"{t} vis @{prefix}{t} {ids}" for t in range(0, t_max + 1, step)]


def client(name, script, relaunch=None, account=None, args=("--offline", "--map", "kanji"), secs=150):
    """one headless client_bin run; returns its log. account: (username, token) saved as the signed-in session"""
    path = os.path.join(WORK, name + ".script")
    rpath = os.path.join(WORK, name + "-relaunch.script")
    for file, lines in ((path, script), (rpath, relaunch or script)):
        with open(file, "w") as f:  # the input script wants its events in time order
            f.write("\n".join(sorted(lines, key=lambda l: int(l.split()[0]))) + "\n")
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    if account:
        for key, value in (("krunker_username", account[0]), ("krunker_token", account[1])):
            with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
                f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path,
                "KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH": rpath, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_ASSETS_DIR": ASSETS,
                "KRUNKNATIVE_BRAIN_URL": BRAIN_URL[0], "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_FINDER_URL": BRAIN_URL[0] + "/servers"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", *args], cwd=ROOT, stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        try:
            p.wait(secs)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timed out, log {log}")
    return read_log(log)


def fields(text, label, t_from=0, t_to=10**9):
    """the samples of `label` (a sample's id prefix) inside [t_from, t_to]"""
    return [(t, f) for t, f in samples(text, label) if t_from <= t <= t_to]


def hidden(f):
    return f.get("chatUIHolder") == "0" and f.get("chatUI") == "0"


def shown(f):
    return f.get("chatUIHolder") == "1" and f.get("chatUI") == "1"


def in_game(text, label):
    return [t for t, f in samples(text, label) if f.get("inGameUI") == "1"]


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("chat_ingame_gate_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    if not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("chat_ingame_gate_e2e: no kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("chat_ingame_gate_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local",
                                  "--brain.id=chatgate-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        BRAIN_URL[0] = "http://127.0.0.1:%d" % port
        node(port, "CHATGATE", PORT_NODE)
        check("the brain lists the lobby", wait_for(lambda: len(lobbies(port)) == 1, 25) is not None,
              str(lobbies(port)))
        user = "chatgate%d" % (os.getpid() % 100000)
        st, reg = http(port, "/auth/register", {"username": user, "password": "pw-chatgate-%d" % os.getpid()})
        token = reg.get("token", "") if isinstance(reg, dict) else ""
        check("an account is registered", st == 200 and token, str(reg)[:200])
        if not token:
            return 1
        account = (user, token)
        scenarios(account)
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    print(f"\n{sum(results)}/{len(results)} passed  (logs: {WORK})")
    return 0 if results and all(results) else 1


def check_idle_menu(case, text, frames):
    """the chat is hidden while the join loads (the first frame) and up on the idle menu from the join on"""
    up = next((i for i, (_, f) in enumerate(frames) if shown(f)), None)
    check(f"{case}: the chat is hidden while the join loads, up on the lobby's idle menu after it (every frame)",
          frames and hidden(frames[0][1]) and up is not None and all(shown(f) for _, f in frames[up:]) and not in_game(text, "m"),
          f"first frame hidden {bool(frames) and hidden(frames[0][1])}, up from frame {up} of {len(frames)}, "
          f"{sum(shown(f) for _, f in frames)} shown, in game {in_game(text, 'm')[:1]}")


def scenarios(account):
    joined = f"connected to server 127.0.0.1:{PORT_NODE}"
    # 1: a signed-in account: the launch's Quick Match joins the lobby, the menu stays up (no Play)
    menu = ["0 boot"] + vis_lines("m", 12000, 250) + ["12100 quit"]
    t = client("account-menu", menu, account=account, args=())
    m = fields(t, "m")
    check("1: the account is signed in and the client joined the lobby", "[account] logged in as" in t and joined in t,
          f"{len(m)} frames")
    check_idle_menu("1", t, m)

    # 2: a guest: the same lobby and menu
    t = client("guest-menu", menu, account=None, args=())
    m = fields(t, "m")
    check("2: the guest joined the lobby (no sign-in)", joined in t and "[account] logged in" not in t, "")
    check_idle_menu("2", t, m)

    # 3: F5 from the lobby's menu: the relaunch rejoins the lobby; until Play, its frames have no chat
    # the join takes 2 to 8 s of game time: F5 comes after it, Play after the rejoin's join
    first = ["0 boot"] + vis_lines("f", 12000, 500) + ["12000 down F5", "12050 up F5"]
    relaunch = ["0 boot"] + vis_lines("r", 14000, 250) + ["10000 play", "14100 quit"]
    t = client("reload-lobby", first + ["15000 quit"], relaunch=relaunch, account=account, args=())
    r = fields(t, "r")
    check("3: F5 relaunched the client and it rejoined the lobby (the same game id)",
          "[reload] restart: F5: relaunch" in t and "resume after the restart: game " in t and t.count(joined) >= 2,
          f"{len(r)} relaunched frames")
    back = in_game(t, "r")
    check("3: the rejoined client is in the game after Play", back and back[-1] >= 3500, f"in-game frames from {back[:1]}")
    if back:
        pre = [(tt, f) for tt, f in r if tt < back[0]]
        up = next((i for i, (_, f) in enumerate(pre) if shown(f)), None)
        check("3: the rejoin's loading frames have no chat, the idle menu after the join has it (every frame to Play)",
              pre and hidden(pre[0][1]) and up is not None and all(shown(f) for _, f in pre[up:]),
              f"first frame hidden {bool(pre) and hidden(pre[0][1])}, up from frame {up} of {len(pre)}")
        post = [(tt, f) for tt, f in r if tt >= back[0] and f.get("inGameUI") == "1"]
        check("3: the chat and the in-game HUD show once the game is back", post and all(shown(f) for _, f in post),
              f"{sum(shown(f) for _, f in post)} of {len(post)} frames")

    # 4: F5 from an offline game: the relaunch resumes the game; its loading frames have no chat
    first = ["0 play", "400 vis @pre chatUIHolder chatUI inGameUI"] + ["1000 down F5", "1050 up F5"]
    relaunch = ["0 boot"] + vis_lines("r", 4000, 100) + ["4100 quit"]
    t = client("reload-game", first + ["8000 quit"], relaunch=relaunch, account=account)
    r = fields(t, "r")
    back = in_game(t, "r")
    check("4: F5 relaunched the offline game and the game is back", "[reload] restart: F5: relaunch" in t and back,
          f"in-game frames {len(back)}")
    if back:
        # the offline relaunch shows no loading screen (the menu is up for ~0.1 s before the game is back):
        # the chat stays up there, so only the game's frames are checked
        post = [(tt, f) for tt, f in r if tt >= back[0] and f.get("inGameUI") == "1"]
        check("4: the chat and the in-game HUD show once the game is back", post and all(shown(f) for _, f in post),
              f"{sum(shown(f) for _, f in post)} of {len(post)} frames")

    # 5: a signed-in account in the offline game (no reload): the chat and the HUD from the spawn on
    t = client("account-game", ["0 play"] + vis_lines("g", 2500, 500) + ["2600 quit"], account=account)
    g = fields(t, "g", 500)
    check("5: the chat and the in-game HUD show in the game (signed-in account)",
          g and all(shown(f) and f.get("inGameUI") == "1" for _, f in g),
          f"{sum(shown(f) and f.get('inGameUI') == '1' for _, f in g)} of {len(g)} frames")


if __name__ == "__main__":
    sys.exit(main())
