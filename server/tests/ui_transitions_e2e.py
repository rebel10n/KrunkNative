#!/usr/bin/env python3
"""UI state transitions end-to-end: what the player sees of every state-managed piece of the
page before and after ESC / the menu, death, the end screen, spectating, menu windows, map and
mode changes, disconnect / reconnect, Lost Connection and offline <-> online.

  python3 -I server/tests/ui_transitions_e2e.py BUILD_DIR [scenario ...]   (from the repository root)

Each scenario is one headless client_bin run (KRUNKNATIVE_HEADLESS=1, EGL; KRUNKNATIVE_EGL_DEVICE
picks the GPU) driven by an input script (client/src/input_script.c): keys, clicks on page
elements (`clickid`), console lines (`con`) and `vis @label <ids>` probes, which log whether
each element is shown (html_dom_is_shown: rendered, visible, not transparent, non-empty;
`:preview` / `:spray` are the GL-drawn class preview and spray wheel). The expectations follow
Krunker v7.1.4 (src/libs/UI.js toggleMenu / toggleControlUI / toggleGameUI / hideUI /
showEndScreen / toggleEndScreen, index.js) where it defines them. Online scenarios run
server_bin on free ports (kn_proto.free_ports).

Prints PASS/FAIL per probe and a matrix (transition x element); exit code 0 only when every
probe passed. Logs are kept in ~/.cache/kn-ui-transitions-* on failure.
"""
import concurrent.futures
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ONLY = set(sys.argv[2:])
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-ui-transitions-", dir=CACHE if os.path.isdir(CACHE) else None)
LOCK = threading.Lock()
results = []   # (scenario, label, element, expected, got, ok)
procs = []

ESC = "down ESC\n{t2} up ESC"  # one press (the release 50 ms later)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def expand(script):
    """`T esc` -> a press and release of ESC; `T check @label id=1 id=0 id=?` -> `T vis @label ids`
    plus the expectations ({label: {id: '1'|'0'|'?'}}, in order)."""
    lines, expects = [], []
    for raw in script.strip().splitlines():
        raw = raw.strip()
        if not raw or raw.startswith("#"):
            continue
        t, cmd, *rest = raw.split()
        if cmd == "esc":
            lines += [f"{t} down ESC", f"{float(t) + 50:g} up ESC"]
        elif cmd == "tab":  # hold Tab for the next 400 ms
            lines += [f"{t} down TAB"]
        elif cmd == "check":
            label = rest[0].lstrip("@")
            want = dict(item.split("=", 1) for item in rest[1:])
            expects.append((label, want))
            lines.append(f"{t} vis @{label} " + " ".join(want))
        else:
            lines.append(raw)
    return "\n".join(lines) + "\n", expects


def run_client(name, args, script, timeout=150, on_line=None, env_extra=None, storage=None, require=None):
    """client_bin headless with the expanded input script; returns its log lines. `storage`: saved
    page values (localStorage: XDG_CONFIG_HOME/krunknative/<key>.json) to start with. `require`:
    (label, element, value) the run needs to say anything (else its probes are skipped, not failed)."""
    text, expects = expand(script)
    if storage:
        folder = os.path.join(WORK, f"{name}-cfg", "krunknative")
        os.makedirs(folder, exist_ok=True)
        for key, value in storage.items():
            with open(os.path.join(folder, f"{key}.json"), "w") as f:
                f.write(value)
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(text)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_UPDATES_URL": "off"})
    env.update(env_extra or {})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    with LOCK:
        procs.append(p)
    deadline = time.time() + timeout
    seen = 0
    while p.poll() is None and time.time() < deadline:
        time.sleep(0.2)
        if on_line:
            lines = read(log).splitlines()
            for line in lines[seen:]:
                on_line(line)
            seen = len(lines)
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        record(name, "finished", "client_bin", "exit", "timeout", False)
    lines = read(log).splitlines()
    got = {}
    order = []
    for line in lines:
        m = re.search(r"\[input_script\] vis t=\d+ @(\S+)(.*)", line)
        if m:
            got[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split())
            order.append(m.group(1))
    if require and got.get(require[0], {}).get(require[1]) != require[2]:
        print(f"SKIP  {name}: {require[0]} {require[1]}={got.get(require[0], {}).get(require[1])} (needs {require[2]})", flush=True)
        return None
    for label, want in expects:
        have = got.get(label)
        for elem, value in want.items():
            if have is None:
                record(name, label, elem, value, "no probe", value == "?")
            else:
                record(name, label, elem, value, have.get(elem, "-"), value in ("?", have.get(elem)))
    return lines


def record(scenario, label, elem, want, got, ok):
    with LOCK:
        results.append((scenario, label, elem, want, got, bool(ok)))


def start_server(name, extra=()):
    (port,) = free_ports(1)
    log = os.path.join(WORK, f"{name}-server.log")
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port), *extra], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    with LOCK:
        procs.append(p)
    time.sleep(1.0)
    return p, port


def crash(p):
    """The server goes away at once (SIGTERM would drain the round first)."""
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


def stop(p):
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)


# The page parts a playing (alive, menu closed) FFA player sees, and the menu.
HUD = "inGameUI=1 topLeftMain=1 weapHolder=1 bottomRight=1 chatUI=1 chatList=1 menuHolder=0"
MENU = "menuHolder=1 instructionHolder=1 inGameUI=0 chatUI=1 :preview=1 endUI=0 deathUIHolder=0"
RESULTS = "endUI=1 endInfo=1 matchVoteHolder=1 endMidHolder=1 endTabbedView=1 victorySub=1 menuHolder=0 inGameUI=0 chatUI=1"


# ---------------------------------------------------------------------------------- scenarios

def sc_endscreen():
    """End screen (FFA): ESC brings the menu over the results, ESC brings them back (UI.toggleEndScreen),
    also after a menu window and through the Play click; the next round gives the HUD back."""
    run_client("endscreen", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 check @alive {HUD}
        1500 con end
        12000 check @end {RESULTS} spectateUI=0 deathUIHolder=0
        12500 esc
        13500 check @end-esc {MENU}
        14000 esc
        15000 check @end-esc-esc {RESULTS}
        15500 esc
        16000 clickid menuBtnSettings
        16800 check @end-window windowHolder=1 menuHolder=1 endUI=0
        17000 esc
        17800 check @end-window-closed windowHolder=0 menuHolder=1 :preview=1 endUI=0
        18000 esc
        19000 check @end-window-esc {RESULTS}
        19500 esc
        20000 clickid instructionHolder
        21000 check @end-play-click {RESULTS}
        21500 con endless
        60000 respawn
        64000 check @next-round {HUD} endUI=0 matchVoteHolder=0 endMidHolder=0 miniMapOverlay=1
        64500 quit
    """, timeout=180)


def sc_endscreen_team():
    """End screen of a team mode: the team totals survive the ESC round trip too."""
    run_client("endscreen-tdm", ["--offline", "--map", "kanji"], """
        0 play
        500 con mode tdm
        3000 respawn
        5000 check @tdm-alive inGameUI=1 menuHolder=0
        5500 con end
        16000 check @end endUI=1 endInfo=1 teamTotal0=1 teamTotal1=1 endMidHolder=1 matchVoteHolder=1
        16500 esc
        17500 check @end-esc menuHolder=1 endUI=0
        18000 esc
        19000 check @end-esc-esc endUI=1 endInfo=1 teamTotal0=1 teamTotal1=1 endMidHolder=1 matchVoteHolder=1
        19500 quit
    """)


def sc_death():
    """Death screen: the card and the Tab board (v7 polls the board key before tmpPlayer.active),
    ESC while dead, back to the card, then the respawn."""
    run_client("death", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 check @alive {HUD} miniMapOverlay=1
        1100 down TAB
        1400 check @alive-tab centerLeaderDisplay=1
        1500 con kill
        1700 up TAB
        2000 check @dead-tab-released centerLeaderDisplay=0 deathUIHolder=1
        2100 down TAB
        2400 check @dead-tab centerLeaderDisplay=1 deathUIHolder=1
        2500 up TAB
        2700 check @dead deathUIHolder=1 inGameUI=1 topLeftMain=0 weapHolder=0 bottomRight=0 chatUI=1 centerLeaderDisplay=0 menuHolder=0 miniMapOverlay=0
        2800 esc
        3300 check @dead-esc menuHolder=1 deathUIHolder=0 inGameUI=0 chatUI=1 :preview=1
        3400 esc
        3600 check @dead-esc-esc inGameUI=1 chatUI=1 menuHolder=0 deathUIHolder=? topLeftMain=?
        9000 respawn
        12000 check @respawned {HUD} deathUIHolder=0 miniMapOverlay=1
        12500 esc
        13000 check @alive-esc {MENU} miniMapOverlay=0
        13500 esc
        14000 check @alive-esc-esc {HUD} miniMapOverlay=1
        14500 quit
    """)


def sc_spectate():
    """Spectating: the Spectate switch, ESC there and back (after clicking the switch, ESC still leaves
    the menu), a mode change while spectating, a disconnect / reconnect, then playing again."""
    run_client("spectate", ["--offline", "--map", "kanji"], """
        0 menu
        300 con bot_add 2
        1000 check @menu menuHolder=1 spectButton=1 :preview=1
        1100 clickid specBtn
        1500 play
        3000 check @spect spectateUI=1 inGameUI=1 chatUI=1 menuHolder=0 weapHolder=0 deathUIHolder=0
        3500 esc
        4500 check @spect-esc spectateUI=0 menuHolder=1 chatUI=1 inGameUI=0
        5000 esc
        6000 check @spect-esc-esc spectateUI=1 inGameUI=1 chatUI=1 menuHolder=0
        6500 con mode tdm
        9500 check @spect-mode-change spectateUI=1 chatUI=1 menuHolder=0
        10000 con disconnect
        11000 check @spect-disconnect instructionsUpdate=1 spectateUI=0 inGameUI=0 overlay=0 aimRecticle=0
        11500 con reconnect
        14000 check @spect-reconnect instructionsUpdate=0 menuHolder=1 spectateUI=0 inGameUI=0 chatUI=1
        15100 clickid specBtn
        15500 play
        17500 check @unspect-play inGameUI=1 weapHolder=1 bottomRight=1 chatUI=1 spectateUI=0 menuHolder=0
        18000 quit
    """)


def sc_map_change():
    """Map and mode changes with the menu closed (the offline game reloads and respawns in one frame)."""
    run_client("map-change", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 check @alive {HUD} speedRunHolder=0
        1500 con mode bhop
        4500 respawn
        6000 check @parkour speedRunHolder=1 weapHolder=0 inGameUI=1 chatUI=1
        6500 esc
        7000 check @parkour-esc menuHolder=1 inGameUI=0
        7500 esc
        8500 check @parkour-esc-esc speedRunHolder=1 weapHolder=0 inGameUI=1
        9000 con mode ffa
        12000 respawn
        14000 check @ffa-after-parkour {HUD} speedRunHolder=0
        14500 con map Burg
        17500 respawn
        19500 check @map-change {HUD}
        20000 con mode ctf
        23000 respawn
        25000 check @ctf teamScores=1 inGameUI=1
        25500 esc
        26000 esc
        27000 check @ctf-esc-esc teamScores=1 {HUD}
        27500 con kill
        28000 check @ctf-dead teamScores=0 deathUIHolder=1
        28500 quit
    """, timeout=120)


def sc_interact():
    """The interact prompt (one of Freight's "pickup Junk" interact triggers) through ESC and the
    showUI setting (a Toggle bind: cont_27 = U, cont_27_set = showUI). The trigger spawns about 20 s
    into the game (rsc 20) and not in every game: no prompt retries (a few runs), then skips."""
    for attempt in range(6):
        if run_client(f"interact{attempt or ''}", ["--offline", "--map", "freight"], INTERACT_SCRIPT,
                      storage={"cont_27": "85", "cont_27_set": "showUI"}, require=("near", "interactMsg", "1")) is not None:
            return
    record("interact", "near", "interactMsg", "1", "skipped: no Junk trigger in 6 runs", True)


INTERACT_SCRIPT = """
        0 play
        20200 pose 349 -8.6 140.4 0 0
        21500 check @near interactMsg=1
        22000 esc
        22500 check @near-esc interactMsg=0 menuHolder=1
        23000 esc
        23500 pose 349 -8.6 140.4 0 0
        24500 check @near-esc-esc interactMsg=1
        25000 down U
        25050 up U
        25500 check @showui-off interactMsg=0 inGameUI=0 chatUI=0
        26000 down U
        26050 up U
        26500 pose 349 -8.6 140.4 0 0
        27500 check @showui-on interactMsg=1 inGameUI=1 chatUI=1
        28000 quit
"""


def sc_custom_gui():
    """A map's custom GUI (#customGUIHolder, laboratory's gui) through ESC, death and the end screen."""
    run_client("custom-gui", ["--offline", "--map", "laboratory"], """
        0 play
        1500 check @alive customGUIHolder=1
        2000 esc
        2500 check @alive-esc customGUIHolder=0 menuHolder=1
        3000 esc
        3500 check @alive-esc-esc customGUIHolder=1
        4000 con kill
        4500 check @dead customGUIHolder=1 deathUIHolder=1
        5000 esc
        5500 check @dead-esc customGUIHolder=0
        6000 esc
        6500 check @dead-esc-esc customGUIHolder=1
        12000 respawn
        14000 check @respawned customGUIHolder=1
        14500 con end
        25000 check @end customGUIHolder=0 endUI=1
        25500 esc
        26000 esc
        27000 check @end-esc-esc customGUIHolder=0 endUI=1
        27500 quit
    """)


def sc_krunkscript():
    """KrunkScript GAME.UI divs (kanji's client script) through ESC and death."""
    run_client("krunkscript", ["--offline", "--map", "kanji"], """
        0 play
        2000 check @alive customHolder=? inGameUI=1
        2500 esc
        3000 check @alive-esc customHolder=0 inGameUI=0
        3500 esc
        4000 check @alive-esc-esc customHolder=? inGameUI=1
        4500 quit
    """)


def sc_spray():
    """The spray wheel (held F): closed by death (v7 io_killPlayer sprayWheelClose), the end screen
    and the menu."""
    run_client("spray", ["--offline", "--map", "kanji"], """
        0 play
        500 down F
        1200 check @held :spray=1
        1300 con kill
        1600 check @dead :spray=0
        9000 respawn
        11000 check @respawned-key-still-held :spray=0
        11100 up F
        11500 down F
        12200 check @held-again :spray=1
        12300 esc
        12600 check @held-esc :spray=0
        12700 up F
        13000 esc
        13500 down F
        14200 check @held-3 :spray=1
        14300 con end
        18500 check @end-title :spray=0 endUI=1
        18600 up F
        19000 quit
    """)


def sc_windows():
    """Menu windows over the menu: the class preview and the menu come back when one closes."""
    run_client("windows", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 check @alive {HUD}
        1500 esc
        2000 check @menu {MENU}
        2100 clickid menuBtnSettings
        2600 check @window windowHolder=1 menuHolder=1
        2700 esc
        3200 check @window-closed {MENU} windowHolder=0
        3300 esc
        4000 check @back {HUD}
        4500 quit
    """)


def sc_online():
    """Online: ESC / death on a server, offline and back online, disconnect / reconnect, then the
    server goes away (Lost Connection): ESC keeps the error box (v7 f2040 is terminal), and
    `offline` leaves it."""
    server, port = start_server("online")
    killed = []

    def watch(line):
        if "@kill-server" in line and not killed:
            killed.append(1)
            crash(server)

    try:
        run_client("online", ["--connect", f"127.0.0.1:{port}"], f"""
            0 play
            1500 check @online-alive {HUD}
            2000 esc
            2500 check @online-esc {MENU}
            3000 esc
            3500 check @online-esc-esc {HUD}
            4000 con offline
            7000 respawn
            9000 check @offline {HUD}
            9500 con connect 127.0.0.1:{port}
            13000 respawn
            15000 check @online-again {HUD}
            15500 con disconnect
            16500 check @disconnected instructionsUpdate=1 menuHolder=0 inGameUI=0 chatHolder=0
            16600 esc
            17000 check @disconnected-esc instructionsUpdate=1 inGameUI=0
            17500 con reconnect
            20000 respawn
            22000 check @reconnected {HUD}
            22500 check @kill-server inGameUI=?
            34000 check @lost-connection instructionsUpdate=1 menuHolder=0 inGameUI=0 chatHolder=0
            34500 esc
            35500 check @lost-connection-esc instructionsUpdate=1 inGameUI=0
            36000 esc
            37000 check @lost-connection-esc-esc instructionsUpdate=1 inGameUI=0
            37500 con offline
            40000 respawn
            42000 check @offline-after-lost {HUD} instructionsUpdate=0 chatHolder=1
            42500 quit
        """, on_line=watch, timeout=150)
    finally:
        stop(server)


def sc_lost_during_end():
    """The server goes away while the results show: the Lost Connection box, not the old results."""
    server, port = start_server("lost-end")
    killed = []

    def watch(line):
        if "@kill-server" in line and not killed:
            killed.append(1)
            crash(server)

    try:
        run_client("lost-end", ["--connect", f"127.0.0.1:{port}"], f"""
            0 play
            1000 con end
            12000 check @end endUI=1 matchVoteHolder=?
            12100 check @kill-server endUI=?
            24000 check @lost instructionsUpdate=1 endUI=0
            24500 esc
            25500 check @lost-esc instructionsUpdate=1 endUI=0
            26000 quit
        """, on_line=watch, timeout=90)
    finally:
        stop(server)


class Manifest(http.server.BaseHTTPRequestHandler):
    body = b""

    def do_GET(self):  # noqa: N802
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(self.body)))
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, *args):
        pass


def sc_outdated():
    """The Outdated box (update_ui.c): a manifest with a newer protocol; it follows the menu."""
    (port,) = free_ports(1)
    Manifest.body = json.dumps({"format": 1, "channel": "latest", "outdated_grace_h": 24,
                                "build": {"describe": "v9.9.9-1-gfeedbee", "commit": "feedbeefeedbeefeedbeefeedbeefeedbeefeed",
                                          "commit_time": 4000000000, "build_time": "2096-10-12T00:00:00Z", "protocol": "9.9"},
                                "platforms": {}}).encode()
    httpd = http.server.HTTPServer(("127.0.0.1", port), Manifest)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        run_client("outdated", ["--offline", "--map", "kanji"], f"""
            0 menu
            5000 check @menu knUpdateAd=1 menuHolder=1
            5100 play
            6500 check @playing knUpdateAd=0 {HUD}
            7000 esc
            7500 check @esc knUpdateAd=1 menuHolder=1
            7600 clickid menuBtnSettings
            8000 check @window windowHolder=1
            8100 esc
            8600 check @window-closed knUpdateAd=1 windowHolder=0
            9000 quit
        """, env_extra={"KRUNKNATIVE_UPDATES_URL": f"http://127.0.0.1:{port}/latest.json"})
    finally:
        httpd.shutdown()


def sc_comp():
    """A comp lobby (server_bin gameserver.host_type comp): its menu (#mMenuHolComp, v7 f1904 at
    every INIT) instead of the default one, then offline (no comp: the default menu again)."""
    server, port = start_server("comp", ["--gameserver.host_type=comp"])
    try:
        run_client("comp", ["--connect", f"127.0.0.1:{port}"], """
            0 menu
            3000 check @comp-menu mMenuHolComp=1 mMenuHolDef=0 menuHolder=1
            3500 con offline
            7000 check @offline mMenuHolComp=0 menuHolder=0 inGameUI=1
            7200 esc
            8000 check @offline-esc mMenuHolComp=0 mMenuHolDef=1 menuHolder=1 :preview=1
            8500 quit
        """, timeout=90)
    finally:
        stop(server)


SCENARIOS = {
    "endscreen": sc_endscreen, "endscreen-tdm": sc_endscreen_team, "death": sc_death, "spectate": sc_spectate,
    "map-change": sc_map_change, "interact": sc_interact, "custom-gui": sc_custom_gui, "krunkscript": sc_krunkscript,
    "spray": sc_spray, "windows": sc_windows, "online": sc_online, "lost-end": sc_lost_during_end, "outdated": sc_outdated,
    "comp": sc_comp,
}


def main():
    names = [n for n in SCENARIOS if not ONLY or n in ONLY]
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=int(os.environ.get("KN_UI_TRANSITIONS_JOBS", "4"))) as pool:
            for future in [pool.submit(SCENARIOS[n]) for n in names]:
                future.result()
    finally:
        with LOCK:
            for p in procs:
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGTERM)
    order = {n: i for i, n in enumerate(names)}
    rows = sorted(results, key=lambda r: order.get(r[0], 99))
    for scenario, label, elem, want, got, ok in rows:
        if want == "?" and ok:
            continue
        print(f"{'PASS' if ok else 'FAIL'}  {scenario:13s} {label:28s} {elem}={got}" + ("" if ok else f" (want {want})"), flush=True)
    # matrix: one line per probe, the checked elements with their values (! = wrong)
    print("\nmatrix (scenario / transition: element=value, ! marks a failure, ? informative)")
    seen = []
    for scenario, label, *_ in rows:
        if (scenario, label) not in seen:
            seen.append((scenario, label))
    for scenario, label in seen:
        cells = [f"{'!' if not ok else ''}{e}={g}" for s, l, e, w, g, ok in rows if s == scenario and l == label]
        print(f"  {scenario + ' / ' + label:45s} " + " ".join(cells))
    passed = sum(1 for r in rows if r[5])
    print(f"{passed}/{len(rows)} passed")
    if rows and passed == len(rows):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    sys.exit(0 if rows and passed == len(rows) else 1)


if __name__ == "__main__":
    main()
