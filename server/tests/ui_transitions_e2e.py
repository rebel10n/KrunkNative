#!/usr/bin/env python3
"""UI state transitions end-to-end: what the player sees of every state-managed piece of the
page before and after ESC / the menu, death, the end screen, spectating, menu windows, map and
mode changes, disconnect / reconnect, Lost Connection and offline <-> online, and the Object
Manager grid's scroll position through its clicks (`scroll` probes: scrollTop).

  python3 -I server/tests/ui_transitions_e2e.py BUILD_DIR [scenario ...]   (from the repository root)

Each scenario is one headless client_bin run (KRUNKNATIVE_HEADLESS=1, EGL; KRUNKNATIVE_EGL_DEVICE
picks the GPU) driven by an input script (client/src/input_script.c): keys, clicks on page
elements (`clickid`), console lines (`con`) and `vis @label <ids>` probes, which log whether
each element is shown (html_dom_is_shown: rendered, visible, not transparent, non-empty;
`:preview` / `:spray` are the GL-drawn class preview and spray wheel) and `chat @label <place>`
checks, which read #chatHolder's box (`rect` probes) and compare its place in #uiBase with the
main.css rule of the state (CHAT_PLACES). The expectations follow Krunker v10.0.2, the page in
ui/ (~/.cache/kn-v10/v10.js, `v10:N` = its line: UI.toggleMenu / toggleMenuHider / toggleControlUI /
toggleGameUI / hideUI / showEndScreen / toggleEndScreen, v10:160950-161060), which keeps v7.1.4's
transitions apart from what the scenarios note. The scenarios run as guests (no account): v10
hides a guest's chat on the menu (hideChatIfGuestUser, v10:208576, from toggleMenuHider
v10:161017) and the menu's Spectate switch (hideSpectateIfGuestUser, v10:208579; signed-in players
get both: server/tests/v10_menu_e2e.py). Online scenarios run server_bin on free ports
(kn_proto.free_ports).

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

# #chatHolder's place in #uiBase (CSS px: left, bottom) per state, v7 main.css: the #uiBase class
# rules (same specificity, so the later one wins: onMenu < onCompMenu < onGame < onSpect <
# onEndScrn < onDeathScrn) that UI.toggleMenuHider / toggleControlUI / showEndScreen /
# toggleEndScreen / f1975 toggle. "death" is where the chat-moveup animation (1.5 s) ends. "none":
# the chat is hidden (a guest on the menu: v10 hideChatIfGuestUser, v10:208576).
CHAT_PLACES = {"menu": (150, 20), "comp": (20, 150), "game": (15, 130), "spect": (20, 20), "end": (20, 20),
               "death": (64, 75)}
CHAT_TOLERANCE = 2.0  # CSS px


def chat_place(chat, base):
    """`rect` values of #chatHolder and #uiBase (x,y,w,h,offsetWidth,offsetHeight) -> "left,bottom"
    of the chat in #uiBase CSS px (#uiBase is scaled: its rect over its layout size)."""
    if chat == "-" or base == "-":
        return "-"
    cx, cy, cw, ch, _, _ = map(float, chat.split(","))
    bx, by, bw, bh, ow, oh = map(float, base.split(","))
    sx, sy = bw / ow, bh / oh
    return f"{(cx - bx) / sx:.0f},{(by + bh - cy - ch) / sy:.0f}"


def chat_place_ok(want, got):
    if want == "none":
        return got == "-"
    if want == "?" or got == "-" or want not in CHAT_PLACES:
        return want == "?"
    left, bottom = map(float, got.split(","))
    return all(abs(a - b) <= CHAT_TOLERANCE for a, b in zip((left, bottom), CHAT_PLACES[want]))


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def expand(script):
    """`T esc` -> a press and release of ESC; `T check @label id=1 id=0 id=?` -> `T vis @label ids`
    plus the expectations ({label: {id: '1'|'0'|'?'}}, in order); `T chat @label place` -> `T rect
    @label chatHolder uiBase` plus the expectation {label: {':chat': place}} (CHAT_PLACES)."""
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
        elif cmd == "chat":
            label = rest[0].lstrip("@")
            expects.append((label, {":chat": rest[1]}))
            lines.append(f"{t} rect @{label} chatHolder uiBase")
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
            got.setdefault(m.group(1), {}).update(kv.split("=", 1) for kv in m.group(2).split())
            order.append(m.group(1))
        m = re.search(r"\[input_script\] rect t=\d+ @(\S+)(.*)", line)
        if m:
            rects = dict(kv.split("=", 1) for kv in m.group(2).split())
            got.setdefault(m.group(1), {})[":chat"] = chat_place(rects.get("chatHolder", "-"), rects.get("uiBase", "-"))
    if require and got.get(require[0], {}).get(require[1]) != require[2]:
        print(f"SKIP  {name}: {require[0]} {require[1]}={got.get(require[0], {}).get(require[1])} (needs {require[2]})", flush=True)
        return None
    for label, want in expects:
        have = got.get(label)
        for elem, value in want.items():
            if have is None:
                record(name, label, elem, value, "no probe", value == "?")
            elif elem == ":chat":
                record(name, label, elem, value, have.get(elem, "-"), chat_place_ok(value, have.get(elem, "-")))
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


# The page parts a playing (alive, menu closed) FFA player sees, and the menu (a guest's: no chat,
# v10:208576 hideChatIfGuestUser(true) from toggleMenuHider, v10:161017).
HUD = "inGameUI=1 topLeftMain=1 weapHolder=1 bottomRight=1 chatUI=1 chatList=1 menuHolder=0"
MENU = "menuHolder=1 instructionHolder=1 inGameUI=0 chatUI=0 :preview=1 endUI=0 deathUIHolder=0"
# The minimap is on in these runs: v10's showMinimap defaults to off (v10:204651-204656 'val': !1),
# so the scenarios that follow the overlay (#miniMapOverlay) through the transitions turn it on.
MINIMAP_ON = {"knSettings": '{"showMinimap":true}'}
RESULTS = "endUI=1 endInfo=1 matchVoteHolder=1 endMidHolder=1 endTabbedView=1 victorySub=1 menuHolder=0 inGameUI=0 chatUI=1"


# ---------------------------------------------------------------------------------- scenarios

def sc_endscreen():
    """End screen (FFA): ESC brings the menu over the results, ESC brings them back (UI.toggleEndScreen),
    also after a menu window and through the Play click; the next round gives the HUD back."""
    run_client("endscreen", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 check @alive {HUD}
        1000 chat @alive game
        1500 con end
        12000 check @end {RESULTS} spectateUI=0 deathUIHolder=0 :cursor=1
        12000 chat @end end
        12500 esc
        13500 check @end-esc {MENU} :cursor=1
        13500 chat @end-esc none
        14000 esc
        15000 check @end-esc-esc {RESULTS} :cursor=1
        15000 chat @end-esc-esc end
        15500 esc
        16000 clickid knV10Settings
        16800 check @end-window windowHolder=1 menuHolder=1 endUI=0
        17000 esc
        17800 check @end-window-closed windowHolder=0 menuHolder=1 :preview=1 endUI=0
        18000 esc
        19000 check @end-window-esc {RESULTS}
        19500 esc
        20000 clickid instructionHolder
        21000 check @end-play-click {RESULTS} :cursor=1
        21500 con endless
        60000 respawn
        64000 check @next-round {HUD} endUI=0 matchVoteHolder=0 endMidHolder=0 miniMapOverlay=1 :cursor=0
        64000 chat @next-round game
        64500 quit
    """, timeout=180, storage=MINIMAP_ON)


def sc_endscreen_cursor():
    """The mouse on the end screen (v7 io_endGame: CONTROLS.disable() -> exitPointerLock once the
    results show, config.endAnim after the END): hidden during the title (shown when the round ends
    with the menu open: hideUI closes the menu, the lock stays lost; pointer-lock covers the look),
    shown with the results, also when the round ends with the menu open or while spectating, and when Click to Play over
    the results only brings them back (CONTROLS.toggle(true) while locked: UI.toggleEndScreen())."""
    run_client("endscreen-cursor", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 check @alive {HUD} :cursor=0
        1400 esc
        1800 check @menu menuHolder=1 :cursor=1
        2000 con end
        6500 check @title endUI=1 menuHolder=0 :cursor=1
        12000 check @results {RESULTS} :cursor=1
        12500 esc
        13500 check @results-menu {MENU} :cursor=1
        14000 clickid instructionHolder
        15000 check @results-play-click {RESULTS} :cursor=1
        15500 clickid instructionHolder
        16500 check @results-click-again {RESULTS} :cursor=1
        32000 check @next-round {HUD} endUI=0 :cursor=0
        32500 quit
    """)
    run_client("endscreen-cursor-spect", ["--offline", "--map", "kanji"], """
        0 menu
        300 con bot_add 2
        1100 call setSpect 1
        1500 play
        3000 check @spect spectateUI=1 menuHolder=0 :cursor=0
        3500 con end
        14000 check @spect-results endUI=1 endInfo=1 menuHolder=0 :cursor=1
        14500 esc
        15500 check @spect-results-menu menuHolder=1 endUI=0 :cursor=1
        16000 clickid instructionHolder
        17000 check @spect-results-play-click endUI=1 endInfo=1 menuHolder=0 :cursor=1
        17500 quit
    """)


STATE_RE = re.compile(r"\[input_script\] state t=(\d+) .*? menu=(\d) locked=(\d) pos=(\S+) .*? ended=(\d) .*? "
                      r"cursor=(\w+) yaw=(\S+) pitch=(\S+)")


def check_states(name, lines, script, expects):
    """`state @label` probes of the script, in order, against expects {label: {key: value}}: menu, locked
    (CONTROLS.enabled), ended, cursor (GLFW mode: normal | disabled), look=same|moved (yaw/pitch
    against the probe named by `since`: the mouse moved in between) and move=same|moved (the
    player's position against `since`: a movement key was held in between). A key stays held across
    two probes and is released after the last one: a stalled frame (a loaded host) runs every due
    event in one frame, and a press + release there would never move the player."""
    labels = [m.group(1) for m in re.finditer(r"^\s*\d+ state @(\S+)", script, re.M)]
    states = [m for m in (STATE_RE.search(line) for line in lines or []) if m]
    got = {}
    for label, m in zip(labels, states):
        got[label] = {"menu": m.group(2), "locked": m.group(3), "move": tuple(map(float, m.group(4).split(","))),
                      "ended": m.group(5), "cursor": m.group(6), "look": (float(m.group(7)), float(m.group(8)))}
    for label, want in expects.items():
        have = got.get(label)
        for key, value in want.items():
            if key == "since":
                continue
            if have is None:
                record(name, label, key, value, "no probe", False)
            elif key in ("look", "move"):
                base = got.get(want["since"])
                if base is None:
                    record(name, label, key, value, "no probe", False)
                    continue
                moved = any(abs(a - b) > (1e-3 if key == "look" else 1.0) for a, b in zip(have[key], base[key]))
                shown = "moved" if moved else "same"
                fmt = lambda v: ",".join(f"{c:.4f}" if key == "look" else f"{c:.1f}" for c in v)  # noqa: E731
                record(name, label, key, value, f"{shown}({fmt(base[key])}->{fmt(have[key])})", shown == value)
            else:
                record(name, label, key, value, have[key], have[key] == value)


def sc_pointer_lock():
    """The pointer lock and mouse look (v7 controls.js: CONTROLS.enabled follows the pointer lock and the
    mouse only turns the camera under it; pointer_lock.h): Esc open -> close locks again at once
    (natively), the menu turns nothing, a round that ends with the Esc menu up runs its title with the
    cursor free and inert (hideUI closes the menu, the lock stays lost) until a click on the game view
    locks it again, the results free it (CONTROLS.disable()), the next round locks it again."""
    locked = {"menu": "0", "locked": "1", "cursor": "disabled"}
    free = {"locked": "0", "cursor": "normal"}
    script = """
        0 play
        1000 state @alive
        1100 cursor 700 400
        1300 cursor 900 300
        1500 state @alive-look
        1600 esc
        2000 state @menu
        2100 cursor 300 300
        2300 cursor 500 600
        2500 state @menu-look
        2600 esc
        3000 state @menu-closed
        3100 cursor 700 400
        3300 cursor 900 300
        3500 state @menu-closed-look
        3600 esc
        3800 con end
        8500 check @title-free endUI=1 menuHolder=0 :cursor=1
        8500 state @title-free
        8600 cursor 300 300
        8800 cursor 500 600
        9000 state @title-free-look
        9200 click 640 360
        9500 state @title-click
        9600 cursor 700 400
        9800 cursor 900 300
        10000 state @title-click-look
        13500 check @results endUI=1 endInfo=1 :cursor=1
        13500 state @results
        13600 cursor 300 300
        13800 cursor 500 600
        14000 state @results-look
        # the next round's map load stalls a frame (~1 s): probe once it settled, or every due
        # event runs in that one frame
        34000 state @next-round
        34100 cursor 700 400
        34300 cursor 900 300
        34500 state @next-round-look
        34600 down W
        35100 state @next-round-walking
        35700 state @next-round-walk
        35800 up W
        36000 quit
    """
    lines = run_client("pointer-lock", ["--offline", "--map", "kanji"], script)
    check_states("pointer-lock", lines, script, {
        "alive": dict(locked, ended="0"),
        "alive-look": dict(locked, look="moved", since="alive"),
        "menu": dict(free, menu="1"),
        "menu-look": dict(free, menu="1", look="same", since="menu"),
        "menu-closed": dict(locked),
        "menu-closed-look": dict(locked, look="moved", since="menu-closed"),
        "title-free": dict(free, menu="0", ended="1"),
        "title-free-look": dict(free, menu="0", ended="1", look="same", since="title-free"),
        "title-click": dict(locked, ended="1"),
        "title-click-look": dict(locked, ended="1", look="moved", since="title-click"),
        "results": dict(free, menu="0", ended="1"),
        "results-look": dict(free, ended="1", look="same", since="results"),
        "next-round": dict(locked, ended="0"),
        "next-round-look": dict(locked, ended="0", look="moved", since="next-round"),
        "next-round-walk": dict(locked, ended="0", move="moved", since="next-round-look"),
    })

    # Death -> CLICK TO PLAY -> respawn, then an END while playing: the title keeps the lock (the
    # controls stay on until config.endAnim ran out), the results free it and turn nothing.
    script = """
        0 play
        1000 state @alive
        1500 con kill
        2500 state @dead
        7000 check @click-to-play menuHolder=1 instructionHolder=1 :cursor=1
        7000 state @click-to-play
        7100 cursor 300 300
        7300 cursor 500 600
        7500 state @click-to-play-look
        7600 clickid instructionHolder
        9500 state @respawned
        9600 cursor 700 400
        9800 cursor 900 300
        10000 state @respawned-look
        10100 down W
        10600 state @respawned-walking
        11200 state @respawned-walk
        11300 up W
        11500 con end
        16500 state @title-locked
        16600 cursor 300 300
        16800 cursor 500 600
        17000 state @title-locked-look
        21500 state @results
        21600 cursor 700 400
        21800 cursor 900 300
        22000 state @results-look
        22500 quit
    """
    lines = run_client("pointer-lock-respawn", ["--offline", "--map", "kanji"], script)
    check_states("pointer-lock-respawn", lines, script, {
        "alive": dict(locked),
        "dead": dict(locked),
        "click-to-play": dict(free, menu="1"),
        "click-to-play-look": dict(free, menu="1", look="same", since="click-to-play"),
        "respawned": dict(locked, ended="0"),
        "respawned-look": dict(locked, look="moved", since="respawned"),
        "respawned-walk": dict(locked, move="moved", since="respawned-look"),
        "title-locked": dict(locked, ended="1"),
        "title-locked-look": dict(locked, ended="1", look="moved", since="title-locked"),
        "results": dict(free, menu="0", ended="1"),
        "results-look": dict(free, ended="1", look="same", since="results"),
    })


def sc_pointer_lock_chat():
    """A chat line left open (Enter, typing, not sent) when the game takes the input back without a
    click: the next round after the end-screen results, a spawn that closes CLICK TO PLAY (Mag Dump's
    forceSpawn). The browser's click blurs #chatInput (v7 mousedown f2139); here the controls coming
    back do (ui_controls_release_focus). Before, the mouse turned the camera but no key moved."""
    locked = {"menu": "0", "locked": "1", "cursor": "disabled"}
    script = """
        0 play
        1000 con end
        13000 check @results endUI=1 endInfo=1 chatUI=1 :cursor=1
        13200 down ENTER
        13250 up ENTER
        13500 type gg
        13800 state @results-typing
        34000 state @next-round
        34100 down W
        34600 state @next-round-walking
        35200 state @next-round-walk
        35300 up W
        35500 quit
    """
    lines = run_client("pointer-lock-chat-end", ["--offline", "--map", "kanji"], script)
    check_states("pointer-lock-chat-end", lines, script, {
        "results-typing": {"locked": "0", "cursor": "normal", "ended": "1"},
        "next-round": dict(locked, ended="0"),
        "next-round-walk": dict(locked, ended="0", move="moved", since="next-round"),
    })
    script = """
        0 play
        300 con mode md
        3000 respawn
        5000 state @alive
        5500 con kill
        6500 down ENTER
        6550 up ENTER
        6800 type lol
        14000 check @forced-spawn inGameUI=1 menuHolder=0 deathUIHolder=0
        14000 state @forced-spawn
        14100 down W
        14600 state @forced-spawn-walking
        15200 state @forced-spawn-walk
        15300 up W
        15500 quit
    """
    lines = run_client("pointer-lock-chat-spawn", ["--offline", "--map", "kanji"], script)
    check_states("pointer-lock-chat-spawn", lines, script, {
        "alive": dict(locked),
        "forced-spawn": dict(locked),
        "forced-spawn-walk": dict(locked, move="moved", since="forced-spawn"),
    })


def sc_pointer_lock_next_map():
    """Online, the server's next round on the next rotation map (12 s rounds; prod chitown: a player
    was idle-kicked 60 s after the map change, the server getting only empty input): once the new
    map is up the client must lock, look and move again with no click (natively), also when the Esc
    menu was open at the END, when a chat line was left open on the results, and when the menu was
    over the results as the map changed (the spawn closes it, as for a forceSpawn)."""
    locked = {"menu": "0", "locked": "1", "cursor": "disabled"}
    probes = """
        41000 state @next-map
        41100 cursor 700 400
        41300 cursor 900 300
        41600 state @next-map-look
        41700 down W
        42200 state @next-map-walking
        42800 state @next-map-walk
        42900 up W
        43500 quit
    """
    expects = {
        "next-map": dict(locked, ended="0"),
        "next-map-look": dict(locked, ended="0", look="moved", since="next-map"),
        "next-map-walk": dict(locked, ended="0", move="moved", since="next-map-look"),
    }
    variants = {
        "plain": "",
        "menu-at-end": "9000 esc",                                  # the Esc menu up at the END
        "chat-on-results": "20000 down ENTER\n20050 up ENTER\n20300 type gg",  # chat left open
        # the menu over the results (Esc): the new map's spawn closes it (client_player_spawned)
        "menu-on-results": "20000 esc\n20800 check @results-menu menuHolder=1 endUI=0",
    }
    for name, steps in variants.items():
        server, port = start_server(f"next-map-{name}", ("--round-seconds", "12"))
        try:
            script = "0 play\n" + steps + "\n" + probes
            lines = run_client(f"next-map-{name}", ["--connect", f"127.0.0.1:{port}"], script, timeout=120)
            check_states(f"next-map-{name}", lines, script, expects)
        finally:
            stop(server)
        # informative: the rotation's maps (it may repeat one; the INIT path is the same)
        maps = re.findall(r"Loaded map (\S+)", read(os.path.join(WORK, f"next-map-{name}-server.log")))
        record(f"next-map-{name}", "server", "maps", "?", "->".join(maps) or "-", True)


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
    ESC while dead, back to the card, then the respawn. The minimap is on (MINIMAP_ON)."""
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
        3300 check @dead-esc menuHolder=1 deathUIHolder=0 inGameUI=0 chatUI=0 :preview=1
        3300 chat @dead-esc none
        3400 esc
        3600 check @dead-esc-esc inGameUI=1 chatUI=1 menuHolder=0 deathUIHolder=? topLeftMain=?
        3600 chat @dead-esc-esc game
        9000 respawn
        12000 check @respawned {HUD} deathUIHolder=0 miniMapOverlay=1
        12000 chat @respawned game
        12500 esc
        13000 check @alive-esc {MENU} miniMapOverlay=0
        13000 chat @alive-esc none
        13500 esc
        14000 check @alive-esc-esc {HUD} miniMapOverlay=1
        14000 chat @alive-esc-esc game
        14500 quit
    """, storage=MINIMAP_ON)


def sc_death_menu():
    """Death -> "Click to Play": the death card, then config.deathDelay later v7's f1771
    (UI.toggleMenu(true)) brings the menu and the chat goes from the death screen's place to the
    menu's; Click to Play gives the in-game place back. Also when the menu was already up at the
    death (ESC first): f1771 still runs toggleMenu(true), whose toggleMenuHider drops .onDeathScrn."""
    run_client("death-menu", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 check @alive {HUD}
        1000 chat @alive game
        1500 con kill
        3300 check @dead deathUIHolder=1 menuHolder=0 chatUI=1
        3300 chat @dead death
        7000 check @click-to-play {MENU} instructions=1
        7000 chat @click-to-play none
        7500 clickid instructionHolder
        9500 check @played {HUD} deathUIHolder=0
        9500 chat @played game
        10000 esc
        10500 check @menu-before-death {MENU}
        10500 chat @menu-before-death none
        11000 con kill
        12800 check @menu-dead menuHolder=1 instructionHolder=1 chatUI=0
        12800 chat @menu-dead none
        16000 check @menu-dead-click-to-play {MENU} instructions=1
        16000 chat @menu-dead-click-to-play none
        16500 clickid instructionHolder
        18500 check @played-again {HUD} deathUIHolder=0
        18500 chat @played-again game
        19000 quit
    """)


def sc_forced_spawn():
    """A forceSpawn mode (Mag Dump): after config.deathDelay the menu (CLICK TO PLAY) is up when the
    server spawns the player on its own; the chat must leave the menu's place for the in-game one.
    (The death card's toggleGameUI(false) / the spawn's toggleGameUI(true) never touch .onGame: a
    pass that took them for it played the whole life with the chat at the menu's place.) Two
    lives. KrunkNative leaves the menu on that spawn (client_player_spawned); v7 keeps it up until
    a click (io_addPlayers leaves the UI alone)."""
    run_client("forced-spawn", ["--offline", "--map", "kanji"], f"""
        0 play
        300 con mode md
        3000 respawn
        5000 check @alive {HUD}
        5000 chat @alive game
        5500 con kill
        7300 check @dead deathUIHolder=1 menuHolder=0
        7300 chat @dead death
        9300 check @click-to-play menuHolder=1 instructionHolder=1 inGameUI=0
        9300 chat @click-to-play none
        14000 check @forced-spawn inGameUI=1 menuHolder=0 deathUIHolder=0 chatUI=1
        14000 chat @forced-spawn game
        14500 con kill
        18300 check @click-to-play-2 menuHolder=1 instructionHolder=1
        18300 chat @click-to-play-2 none
        23000 check @forced-spawn-2 inGameUI=1 menuHolder=0 deathUIHolder=0
        23000 chat @forced-spawn-2 game
        23500 quit
    """)


def sc_spectate():
    """Spectating: setSpect (the menu's Spectate switch, which v10 hides from a guest:
    hideSpectateIfGuestUser, v10:208579-208581; v10_menu_e2e.py checks it for an account), ESC there
    and back, a mode change while spectating, a disconnect / reconnect, then playing again."""
    run_client("spectate", ["--offline", "--map", "kanji"], """
        0 menu
        300 con bot_add 2
        1000 check @menu menuHolder=1 spectButton=0 spectButtonHolder=0 :preview=1
        1000 chat @menu none
        1100 call setSpect 1
        1500 play
        3000 check @spect spectateUI=1 inGameUI=1 chatUI=1 menuHolder=0 weapHolder=0 deathUIHolder=0
        3000 chat @spect spect
        3500 esc
        4500 check @spect-esc spectateUI=0 menuHolder=1 chatUI=0 inGameUI=0
        4500 chat @spect-esc none
        5000 esc
        6000 check @spect-esc-esc spectateUI=1 inGameUI=1 chatUI=1 menuHolder=0
        6000 chat @spect-esc-esc spect
        6500 con mode tdm
        9500 check @spect-mode-change spectateUI=1 chatUI=1 menuHolder=0
        9500 chat @spect-mode-change spect
        10000 con disconnect
        11000 check @spect-disconnect instructionsUpdate=1 spectateUI=0 inGameUI=0 overlay=0 aimRecticle=0
        11500 con reconnect
        14000 check @spect-reconnect instructionsUpdate=0 menuHolder=1 spectateUI=0 inGameUI=0 chatUI=0
        14000 chat @spect-reconnect none
        15100 call setSpect 0
        15500 play
        17500 check @unspect-play inGameUI=1 weapHolder=1 bottomRight=1 chatUI=1 spectateUI=0 menuHolder=0
        17500 chat @unspect-play game
        18000 quit
    """)


def sc_map_change():
    """Map and mode changes with the menu closed (the offline game reloads and respawns in one frame).
    A fresh CTF round has no #teamScores: v7 f1996 shows it only once a team has scored."""
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
        25000 check @ctf teamScores=0 inGameUI=1
        25500 esc
        26000 esc
        27000 check @ctf-esc-esc teamScores=0 {HUD}
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
    """Menu windows over the menu: the class preview and the menu come back when one closes. The
    minimap is off by default (v10 showMinimap 'val': !1, v10:204651-204656)."""
    run_client("windows", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 check @alive {HUD} miniMapOverlay=0
        1500 esc
        2000 check @menu {MENU}
        2100 clickid knV10Settings
        2600 check @window windowHolder=1 menuHolder=1
        2700 esc
        3200 check @window-closed {MENU} windowHolder=0
        3300 esc
        4000 check @back {HUD}
        4500 quit
    """)


def sc_om_scroll():
    """The Object Manager (F1, client/src/ui/object_manager_ui.c): its grids (#knOmGrid<tab>, a scroll
    container) keeps its scrollTop through whatever a click on it does (a weapon added / taken out,
    a right-click drop, Undo / Remove / Clear Extras, object / entity / NPC spawns and their
    results, LOADOUT_EXTRA); a tab switch and a search show new results from the top. `scroll`
    probes (input_script.c) log scrollTop; `[object-manager] action N` lines show each click landed."""
    tile = "617 432"  # inside a tile at 1280x720 after two wheel steps (the grid wraps to 6 columns since 87cb7ba;
                      # y 400 fell into the 4 px gap between two rows)
    probes = [  # (label, rule): "0", ">0" or "=label" (the same scrollTop as that probe)
        ("open", "0"), ("w-wheel", ">0"), ("w-add", "=w-wheel"), ("w-take-out", "=w-wheel"), ("w-drop", "=w-wheel"),
        ("w-undo", "=w-wheel"), ("w-remove", "=w-wheel"), ("w-add2", "=w-wheel"), ("w-clear", "=w-wheel"),
        ("o-tab", "0"), ("o-wheel", ">0"), ("o-spawn", "=o-wheel"), ("e-tab", "0"), ("e-wheel", ">0"), ("e-spawn", "=e-wheel"),
        ("n-tab", "0"), ("n-wheel", ">=0"), ("n-spawn", "=n-wheel"), ("w-tab", "0"), ("w-wheel2", ">0"), ("w-search", "0"),
    ]
    lines = run_client("om-scroll", ["--offline", "--map", "kanji"], f"""
        0 play
        1000 down F1
        1050 up F1
        2000 check @om windowHolder=1 knOmGrid3=1
        2000 scroll @open knOmGrid3
        2100 cursor {tile}
        2200 wheel -1
        2300 wheel -1
        2800 scroll @w-wheel knOmGrid3
        2900 click {tile}
        3900 scroll @w-add knOmGrid3
        4000 click {tile}
        5000 scroll @w-take-out knOmGrid3
        5100 mdown right
        5150 mup right
        6100 scroll @w-drop knOmGrid3
        6200 clickid knOmUndoBtn
        7200 scroll @w-undo knOmGrid3
        7300 clickid knOmRemoveBtn
        8300 scroll @w-remove knOmGrid3
        8400 click {tile}
        9400 scroll @w-add2 knOmGrid3
        9500 clickid knOmClearBtn
        10500 scroll @w-clear knOmGrid3
        10600 clickid knOmTab0
        11300 scroll @o-tab knOmGrid0
        11400 cursor {tile}
        11500 wheel -1
        11600 wheel -1
        12100 scroll @o-wheel knOmGrid0
        12200 click {tile}
        13200 scroll @o-spawn knOmGrid0
        13300 clickid knOmTab1
        14000 scroll @e-tab knOmGrid1
        14100 cursor {tile}
        14200 wheel -1
        14700 scroll @e-wheel knOmGrid1
        14800 click {tile}
        15800 scroll @e-spawn knOmGrid1
        15900 clickid knOmTab2
        16600 scroll @n-tab knOmGrid2
        16700 cursor {tile}
        16800 wheel -1
        17300 scroll @n-wheel knOmGrid2
        17400 click {tile}
        18400 scroll @n-spawn knOmGrid2
        18500 clickid knOmTab3
        19200 scroll @w-tab knOmGrid3
        19300 cursor {tile}
        19400 wheel -1
        19500 wheel -1
        20000 scroll @w-wheel2 knOmGrid3
        20100 clickid knOmSearchBox
        20600 type a
        21300 scroll @w-search knOmGrid3
        21500 quit
    """)
    if lines is None:
        return
    got = {}
    for line in lines:
        m = re.search(r"\[input_script\] scroll t=\d+ @(\S+) knOmGrid\d=(\S+)", line)
        if m:
            got[m.group(1)] = m.group(2)
    for label, rule in probes:
        have = got.get(label, "no probe")
        try:
            top = float(have)
        except ValueError:
            top = None
        if rule.startswith("="):
            ok = top is not None and have == got.get(rule[1:])
        elif rule == ">0":
            ok = top is not None and top > 0.0
        elif rule == ">=0":  # the NPCs tab may fit without scrolling at 6 columns; the spawn must keep it either way
            ok = top is not None and top >= 0.0
        else:
            ok = top == 0.0
        record("om-scroll", label, "knOmGrid.scrollTop", rule, have, ok)
    # every click reached the sandbox server: ADD, REMOVE, DROP, UNDO, REMOVE_AIM, ADD, CLEAR, OBJECT, ENTITY, NPC
    actions = [int(m.group(1)) for m in (re.search(r"\[object-manager\] action (\d+) item", line) for line in lines) if m]
    want = [0, 1, 2, 4, 9, 8, 1, 3, 7, 5, 6]
    record("om-scroll", "clicks", "actions", " ".join(map(str, want)), " ".join(map(str, actions)), actions == want)


def sc_om_tools():
    """The Object Manager's Tools pane at a short window (1280x600): its list (#knOmToolsList, a scroll
    container under the fixed TOOLS title) does not cut the lines under "Pick a tool to equip the Tool
    Gun" off any more, it scrolls: the wheel over it, and a drag of its scrollbar's thumb (mdown on the
    thumb, the cursor down the bar, mup) to the bottom, where the last line (#knOmToolGun) shows whole;
    then the last tool (Properties) and the last control (Release: Drop) are clicked there, and the
    list keeps its scroll position through the picks and a tab switch (object_manager_ui.c om_rerender).
    `scroll` / `rect` probes (input_script.c) give scrollTop and the boxes; the `[ui] handler` lines
    show the clicks landed."""
    lines = run_client("om-tools", ["--offline", "--map", "kanji"], """
        0 play
        1000 down F1
        1050 up F1
        2000 check @t-open windowHolder=1 knOmToolsList=1 knOmToolGun=1
        2000 scroll @t-open knOmToolsList
        2000 rect @t-open knOmToolsList knOmToolGun
        2100 cursorid knOmToolsList 0.5 0.5
        2200 wheel -1
        2700 scroll @t-wheel knOmToolsList
        2800 wheel 1
        2800 wheel 1
        3300 scroll @t-wheel-up knOmToolsList
        3400 cursorid knOmToolsList 0.985 0.04
        3450 mdown left
        3500 cursorid knOmToolsList 0.985 0.4
        3550 cursorid knOmToolsList 0.985 0.98
        3600 mup left
        4100 scroll @t-drag knOmToolsList
        4100 rect @t-drag knOmToolsList knOmToolGun
        4200 clickid knOmTool6
        5200 scroll @t-pick knOmToolsList
        5300 clickid knOmDrop1
        6300 scroll @t-drop knOmToolsList
        6400 clickid knOmTab1
        7100 scroll @t-tab knOmToolsList
        7200 quit
    """, env_extra={"KRUNKNATIVE_WINDOW_SIZE": "1280x600"})
    if lines is None:
        return
    tops, rects = {}, {}
    for line in lines:
        m = re.search(r"\[input_script\] scroll t=\d+ @(\S+) knOmToolsList=(\S+)", line)
        if m:
            tops[m.group(1)] = m.group(2)
        m = re.search(r"\[input_script\] rect t=\d+ @(\S+)(.*)", line)
        if m:
            rects[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split())

    def top(label):
        try:
            return float(tops.get(label, "-"))
        except ValueError:
            return None

    def bottoms(label):
        """(the list's visible bottom, #knOmToolGun's bottom) in viewport px, None when missing"""
        try:
            lst = [float(v) for v in rects[label]["knOmToolsList"].split(",")]
            gun = [float(v) for v in rects[label]["knOmToolGun"].split(",")]
            return lst[1] + lst[3], gun[1] + gun[3]
        except (KeyError, ValueError, IndexError):
            return None

    opened, dragged = bottoms("t-open"), bottoms("t-drag")
    record("om-tools", "t-open", "knOmToolGun", "below the list's bottom (needs a scroll)",
           opened, opened is not None and opened[1] > opened[0] + 1.0)
    record("om-tools", "t-open", "knOmToolsList.scrollTop", "0", tops.get("t-open", "no probe"), top("t-open") == 0.0)
    record("om-tools", "t-wheel", "knOmToolsList.scrollTop", ">0", tops.get("t-wheel", "no probe"), (top("t-wheel") or 0.0) > 0.0)
    record("om-tools", "t-wheel-up", "knOmToolsList.scrollTop", "0", tops.get("t-wheel-up", "no probe"), top("t-wheel-up") == 0.0)
    record("om-tools", "t-drag", "knOmToolsList.scrollTop", ">0 (thumb dragged to the end)", tops.get("t-drag", "no probe"),
           (top("t-drag") or 0.0) > 0.0)
    record("om-tools", "t-drag", "knOmToolGun", "inside the list (the last line shows whole)",
           dragged, dragged is not None and dragged[1] <= dragged[0] + 1.0)
    # the wheel reached the end too when one step was enough: never further than the drag
    record("om-tools", "t-wheel", "knOmToolsList.scrollTop", "<= the dragged end", tops.get("t-wheel", "no probe"),
           top("t-wheel") is not None and top("t-drag") is not None and top("t-wheel") <= top("t-drag") + 0.01)
    for label in ("t-pick", "t-drop", "t-tab"):
        record("om-tools", label, "knOmToolsList.scrollTop", "=t-drag", tops.get(label, "no probe"),
               top(label) is not None and top(label) == top("t-drag"))
    handlers = [m.group(1) for m in (re.search(r"\[ui\] handler (knOm(?:Tool|PhysDrop|Tab)\(\d+\)) click", line) for line in lines) if m]
    want = ["knOmTool(6)", "knOmPhysDrop(1)", "knOmTab(1)"]
    record("om-tools", "clicks", "handlers", " ".join(want), " ".join(handlers), handlers == want)


def sc_online():
    """Online: ESC / death on a server, offline and back online, disconnect / reconnect, then the
    server goes away: the loading spinner while the transport retries (no error box), the Lost
    Connection box once it gives up (NET_RETRY_BUDGET, ~50 s: connect_ux_e2e), ESC keeps the
    error box (v7 f2040 is terminal), and `offline` leaves it."""
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
            34000 check @lost-retrying instructionsUpdate=0 inGameUI=0 chatHolder=0
            75000 check @lost-connection instructionsUpdate=1 menuHolder=0 inGameUI=0 chatHolder=0
            75500 esc
            76500 check @lost-connection-esc instructionsUpdate=1 inGameUI=0
            77000 esc
            78000 check @lost-connection-esc-esc instructionsUpdate=1 inGameUI=0
            78500 con offline
            81000 respawn
            83000 check @offline-after-lost {HUD} instructionsUpdate=0 chatHolder=1
            83500 quit
        """, on_line=watch, timeout=180)
    finally:
        stop(server)


def sc_lost_during_end():
    """The server goes away while the results show: not the old results, the spinner while the
    transport retries, then the Lost Connection box once it gives up."""
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
            12000 check @end endUI=1 matchVoteHolder=? :cursor=1
            12100 check @kill-server endUI=?
            24000 check @lost-retrying instructionsUpdate=0 endUI=0
            64000 check @lost instructionsUpdate=1 endUI=0
            64500 esc
            65500 check @lost-esc instructionsUpdate=1 endUI=0
            66000 quit
        """, on_line=watch, timeout=130)
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
            7600 clickid knV10Settings
            8000 check @window windowHolder=1
            8100 esc
            8600 check @window-closed knUpdateAd=1 windowHolder=0
            9000 quit
        """, env_extra={"KRUNKNATIVE_UPDATES_URL": f"http://127.0.0.1:{port}/latest.json"})
    finally:
        httpd.shutdown()


def sc_server_list():
    """The server list has no account-only rows (Krunker's has none: v10 row badges OFFICIAL, 24/7,
    2XP, KR, star, lock): a signed-out offline-mode client lists a matchmaker lobby whose meta still
    says onlineMode (a server_bin that refused guests) and a finder server announcing online_mode
    (legacy "guests refused") like any other row: no verified_user icon, no "Account required",
    the finder row not hidden, and the finder is asked without the ?online_mode=0 filter."""
    (port,) = free_ports(1)
    asked = []

    class Finder(Manifest):
        def do_GET(self):  # noqa: N802
            asked.append(self.path)
            super().do_GET()

    Finder.body = json.dumps({"ok": True, "servers": [
        {"host": "127.0.0.1", "port": 1, "query_port": 1, "name": "Finder Online", "map": "Kanji", "mode": "Free for All",
         "modeId": 0, "players": 2, "max": 8, "tags": [], "community": "", "online_mode": True, "password": False,
         "region": "local"}]}).encode()
    listing = os.path.join(WORK, "server-list.json")
    with open(listing, "w") as f:
        json.dump({"games": [["LEGACY1:abcde", "local", 3, 8, {"i": "Burg", "v": "1.0.0", "cs": False, "pr": False, "earnKR": True,
                                                               "mode": 0, "map": "Burg", "name": "Legacy Online", "host": "127.0.0.1",
                                                               "port": 2, "onlineMode": True}, 120]]}, f)
    httpd = http.server.HTTPServer(("127.0.0.1", port), Finder)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        lines = run_client("server-list", ["--offline", "--map", "kanji"], """
            0 menu
            800 call showWindow 2
            2500 call setExpandedRegion 0
            3500 nodes @rows .settNameIn
            3500 text @first .setBodH .settNameIn
            3500 text @last .setBodH .settNameIn:last-child
            3500 nodes @marker .settNameIn span[title]
            3800 quit
        """, timeout=90, env_extra={"KRUNKNATIVE_MODE": "offline", "KRUNKNATIVE_SERVER_LIST": listing,
                                    "KRUNKNATIVE_FINDER_URL": f"http://127.0.0.1:{port}/servers"})
    finally:
        httpd.shutdown()
    if lines is None:
        return
    nodes, texts = {}, {}
    for line in lines:
        m = re.search(r"\[input_script\] nodes t=\d+ @(\S+) elements=\d+ match=(\d+)", line)
        if m:
            nodes[m.group(1)] = int(m.group(2))
        m = re.search(r"\[input_script\] text t=\d+ @(\S+) n=\d+ checked=\d+ value=.*? text=(.*)$", line)
        if m:
            texts[m.group(1)] = m.group(2)
    rows = " | ".join(texts.get(k, "") for k in ("first", "last"))
    record("server-list", "rows", "count", "2", str(nodes.get("rows")), nodes.get("rows") == 2)
    record("server-list", "rows", "both", "Burg+Finder", rows, "Burg" in rows and "Finder Online" in rows)
    marked = "verified_user" in rows or "Account required" in rows or "LOG IN" in rows
    record("server-list", "rows", "account-only", "none", rows if marked else "none", not marked)
    record("server-list", "rows", "marker-spans", "0", str(nodes.get("marker")), nodes.get("marker") == 0)
    record("server-list", "finder", "query", "no online_mode filter", " ".join(asked) or "not asked",
           bool(asked) and not any("online_mode" in a for a in asked))


def sc_comp():
    """A comp lobby (server_bin gameserver.host_type comp): its menu (#mMenuHolComp, v7 f1904 at
    every INIT) instead of the default one, then offline (no comp: the default menu again)."""
    server, port = start_server("comp", ["--gameserver.host_type=comp"])
    try:
        run_client("comp", ["--connect", f"127.0.0.1:{port}"], """
            0 menu
            3000 check @comp-menu mMenuHolComp=1 mMenuHolDef=0 menuHolder=1
            3000 chat @comp-menu none
            3500 con offline
            7000 check @offline mMenuHolComp=0 menuHolder=0 inGameUI=1
            7200 esc
            8000 check @offline-esc mMenuHolComp=0 mMenuHolDef=1 menuHolder=1 :preview=1
            8500 quit
        """, timeout=90)
    finally:
        stop(server)


def skins_expected(weapon):
    """ui_loadout.c owned_skins() in offline mode (every store skin) for Select Skin of weapon
    index `weapon`: rarity, season, index, all descending; None / Random come first (no data-index)."""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "skins.json")) as f:
        skins = json.load(f)
    rows = [(s.get("rarity", 0), s.get("seas") or 1, i) for i, s in enumerate(skins)
            if s.get("name") and not s.get("type") and s.get("weapon") == weapon + 1]
    rows.sort(reverse=True)
    return ["-", "-"] + [str(i) for _, _, i in rows]


def parse_rects(line):
    m = re.search(r"\[input_script\] rects t=\d+ @(\S+) n=\d+(.*)", line)
    if not m:
        return None, []
    cards = []
    for cell in m.group(2).split():
        index, box = cell.split("@", 1)
        x, y, w, h = map(float, box.split(","))
        cards.append((index, x, y, w, h))
    return m.group(1), cards


def sc_skins_grid():
    """Select Skin with every store skin (offline mode, client/src/ui/ui_vgrid.c): only the rows
    near the scroll position are in the DOM, every rendered card is the item of its row / column
    in the full sorted list (top, a jump to the middle, the bottom), a click on the bottom card
    equips it (selectSkin -> localStorage skins), the v7 search filters the grid. Frame times
    while scrolling are logged (perf: informative, never failed)."""
    weapon = 1  # skinSelector(1): the second weapon's skins (the largest weapon list, ~500)
    want = skins_expected(weapon)
    sel = "div[id=skinList] .skinCard"  # no "#": it starts an input-script comment
    script = [f"0 menu", f"500 call skinSelector {weapon}", "1500 nodes @open .skinCard", f"1500 rects @top {sel}",
              "1600 setscroll skinList 100", f"1700 rects @s100 {sel}", "1700 scroll @s100 skinList",
              "1800 setscroll skinList 0", "1900 ft @scroll start"]
    t = 1900
    for i in range(1, 241):  # top to bottom in ~4 s of 16 ms frames
        script.append(f"{t + i * 16} setscroll skinList {i * 100}")
    t += 241 * 16
    script += [f"{t} ft @scroll stop", f"{t + 100} nodes @swept .skinCard",
               f"{t + 200} setscroll skinList 9000", f"{t + 400} rects @mid {sel}", f"{t + 400} scroll @mid skinList",
               f"{t + 500} setscroll skinList 1000000", f"{t + 700} rects @bottom {sel}", f"{t + 700} scroll @bottom skinList",
               f"{t + 700} nodes @bottom .skinCard", f"{t + 800} clicksel {sel}[data-index]",
               f"{t + 1500} vis @equipped windowHolder", f"{t + 1600} call skinSelector {weapon}",
               f"{t + 2400} clickid itemSearch", f"{t + 2500} type gold", f"{t + 2600} call searchItems",
               f"{t + 3000} rects @search {sel}", f"{t + 3000} nodes @search .skinCard", f"{t + 3200} quit"]
    lines = run_client("skins-grid", ["--offline", "--map", "kanji"], "\n".join(script), timeout=240,
                       env_extra={"KRUNKNATIVE_MODE": "offline", "KRUNKNATIVE_FIXED_DELTA": "16"})
    if lines is None:
        return
    rects, scrolls, nodes, clicked = {}, {}, {}, None
    for line in lines:
        label, cards = parse_rects(line)
        if label:
            rects[label] = cards
        m = re.search(r"\[input_script\] scroll t=\d+ @(\S+) skinList=(\S+)", line)
        if m and m.group(2) != "-":
            scrolls[m.group(1)] = float(m.group(2))
        m = re.search(r"\[input_script\] nodes t=\d+ @(\S+) elements=(\d+) match=(\d+)", line)
        if m:
            nodes[m.group(1)] = int(m.group(3))
        m = re.search(r"\[ui\] handler selectSkin\((-?\d+),", line)
        if m:
            clicked = m.group(1)
        if "[input_script] ft " in line:
            print("perf  skins-grid " + line.split("] ", 1)[1], flush=True)
    top = rects.get("top", [])
    if not top:
        record("skins-grid", "open", "#skinList cards", ">0", "none", False)
        return
    y0, x0 = top[0][2], top[0][1]
    cols = sum(1 for c in top if abs(c[2] - y0) < 1)
    pitch_y = next((c[2] - y0 for c in top if c[2] - y0 > 1), 0)
    pitch_x = top[1][1] - x0 if cols > 1 else 1
    moved = {c[0]: c[2] for c in rects.get("s100", []) if c[0] != "-"}
    shift = [moved[c[0]] - c[2] for c in top if c[0] in moved]
    k = -shift[0] / (scrolls.get("s100", 100) or 100) if shift else 0  # window px per scroll px
    record("skins-grid", "open", "geometry", "cols>1 pitch>0", f"cols={cols} pitch={pitch_y:.1f} k={k:.3f}", cols > 1 and pitch_y > 0 and k > 0)

    def check(label):
        cards, s = rects.get(label, []), scrolls.get(label, 0.0)
        bad, first = [], None
        for index, x, y, w, h in cards:
            row = round((y - y0 + k * s) / pitch_y)
            pos = row * cols + round((x - x0) / pitch_x)
            first = pos if first is None else min(first, pos)
            if pos < 0 or pos >= len(want) or want[pos] != index:
                bad.append(f"{index}@{pos}")
        ok = bool(cards) and not bad
        record("skins-grid", label, "cards in place", "all", f"{len(cards)} from #{first}" + (f" bad {' '.join(bad[:4])}" if bad else ""), ok)
        return cards

    check("top")
    check("mid")
    bottom = check("bottom")
    last = bottom[-1][0] if bottom else "-"
    record("skins-grid", "bottom", "last card", want[-1], last, last == want[-1])
    for label in ("open", "swept", "bottom"):
        n = nodes.get(label, -1)
        record("skins-grid", label, "skinCards in DOM", f"<=120 of {len(want)}", str(n), 0 < n <= 120)
    record("skins-grid", "equip", "selectSkin", want[-1], str(clicked), clicked == want[-1])
    saved = read(os.path.join(WORK, "skins-grid-cfg", "krunknative", "skins.json"))
    try:
        equipped = str(json.loads(saved).get(str(weapon)))
    except ValueError:
        equipped = "-"
    record("skins-grid", "equip", "storage skins", want[-1], equipped, equipped == want[-1])
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "skins.json")) as f:
        skins = json.load(f)
    found = [c[0] for c in rects.get("search", []) if c[0] != "-"]
    misses = [i for i in found if "gold" not in json.dumps(skins[int(i)]).lower()]
    record("skins-grid", "search", "matches", "gold items only", f"{len(found)} cards" + (f" misses {misses[:3]}" if misses else ""), bool(found) and not misses)


def sc_hub_items():
    """The social hub's item list (--hub p=items, every store item, client/src/ui/social_ui.c): a
    virtual grid, so a few rows of cards exist whatever the scroll position, down to the bottom
    (v7.1.4 appends 500-card chunks as it is scrolled; native has no scroll events, so before the
    grid the list stopped at 500)."""
    script = ["0 menu", "1000 nodes @open .marketCard", "1000 scroll @open itemsList", "1100 ft @scroll start"]
    t = 1100
    for i in range(1, 201):
        script.append(f"{t + i * 16} setscroll itemsList {i * 1000}")
    t += 201 * 16
    script += [f"{t} ft @scroll stop", f"{t + 100} setscroll itemsList 100000000", f"{t + 300} nodes @bottom .marketCard",
               f"{t + 300} scroll @bottom itemsList", f"{t + 400} quit"]
    lines = run_client("hub-items", ["--offline", "--hub", "p=items"], "\n".join(script), timeout=180,
                       env_extra={"KRUNKNATIVE_MODE": "offline", "KRUNKNATIVE_FIXED_DELTA": "16"})
    if lines is None:
        return
    nodes, scrolls = {}, {}
    for line in lines:
        m = re.search(r"\[input_script\] nodes t=\d+ @(\S+) elements=\d+ match=(\d+)", line)
        if m:
            nodes[m.group(1)] = int(m.group(2))
        m = re.search(r"\[input_script\] scroll t=\d+ @(\S+) itemsList=(\S+)", line)
        if m:
            scrolls[m.group(1)] = m.group(2)
        if "[input_script] ft " in line:
            print("perf  hub-items " + line.split("] ", 1)[1], flush=True)
    for label in ("open", "bottom"):
        n = nodes.get(label, -1)
        record("hub-items", label, "marketCards in DOM", "1..150", str(n), 0 < n <= 150)
    bottom = scrolls.get("bottom", "-")
    record("hub-items", "bottom", "itemsList.scrollTop", ">20000 (thousands of rows)", bottom, bottom != "-" and float(bottom) > 20000)


# A loadout with something in every slot the Change Loadout window pictures (store indices of
# assets/skins.json): Triggerman (Assault Rifle: charm + sight slots) with the Grinchmas skin and the
# Acog, the Grappler secondary (weapon 20: its preview is only in the content tree, ui/textures stops at
# weapon_19), a melee skin, both charms, and one item per Customize slot (offline every skin is owned).
FULL_LOADOUT = {
    "classindex": "0", "secondaryInd": "20", "skins": '{"1":2592,"0":1405,"5":5501,"3":-2}', "attachIndex": "1", "meleeIndex": "6436",
    "charms": "[5577,6269]", "hatIndex": "6435", "headIndex": "6401", "faceIndex": "6447", "bodyIndex": "6413",
    "backIndex": "6400", "dyeIndex": "6437", "waistIndex": "6416", "shoeIndex": "6432", "petIndex": "6409",
    "wristIndex": "4249", "sprayIndex": "6418", "playerCardIndex": "6431", "streakIndex1": "-1", "streakIndex2": "-1",
}
# Loadout tab icons: primary, its charm and sight, secondary, its charm, melee (custIcon), the
# reticle (custIconScope), the streak (custStreakIcon); Customize: one per slot; Class: every card.
# (Selectors without '#': the input script reads it as a comment.) want: the boxes a full loadout
# shows / a guest's (charms, Customize slots: "X" without an item; the default spray is a picture).
ICON_SELECTORS = {"loadout": ("img.custIcon, img.custIconScope, img.custStreakIcon", 8, 6),
                  "custom": ("img.custIcon, img.custIconCard, div.custIconSpray", 12, 1), "class": ("img.classImgC", 16, 16),
                  "box": ("img[id=menuClassIcn]", 1, 1)}
BLANK_STDDEV = 6.0  # luminance stddev (0-255) inside an image box below which it shows nothing


def blank_boxes(png, rects):
    """The `rects` boxes ("idx@x,y,w,h") of `png` that show no picture: the pixels inside (2 px
    inset) are flat, i.e. only the container's background shows through. -> [(box, stddev)]."""
    from PIL import Image, ImageStat
    img = Image.open(png).convert("L")
    out = []
    for item in rects:
        x, y, w, h = (float(v) for v in item.split("@", 1)[1].split(","))
        if w < 6 or h < 6:
            out.append((item, 0.0))
            continue
        box = (int(x) + 2, int(y) + 2, int(x + w) - 2, int(y + h) - 2)
        sd = ImageStat.Stat(img.crop(box)).stddev[0]
        if sd < BLANK_STDDEV:
            out.append((item, sd))
    return out


def frame_png(prefix):
    """The PNG an input script `frames <prefix> 1` wrote (its global frame number appended)."""
    import glob
    found = sorted(glob.glob(prefix + "_[0-9][0-9][0-9][0-9].png"))
    return found[-1] if found else None


def loadout_images_run(name, storage, size, guest=False):
    """One client on the menu with `storage` (offline mode: every skin owned; a guest: online mode
    without an account, so no store items), through the Change Loadout tabs and the Prize Wheels:
    every image resolves to a file (KRUNKNATIVE_HTML_IMAGE_LOG) and every picture box of the Loadout /
    Customize / Class tabs and the menu's class box (#menuClassIcn) shows a picture in the frame."""
    miss_log = os.path.join(WORK, f"{name}.misses")
    shot = lambda label: os.path.join(WORK, f"{name}-{label}")  # noqa: E731
    sel = {k: v[0] for k, v in ICON_SELECTORS.items()}
    script = ["0 menu", f"250 rects @box {sel['box']}", f"260 frames {shot('box')} 1",
              "300 call showWindow 3", "400 call windows.2.switchTab 1", "1200 vis @loadout windowHolder",
              f"1210 rects @loadout {sel['loadout']}", f"1220 frames {shot('loadout')} 1",
              "1300 call windows.2.switchTab 2", f"2000 rects @custom {sel['custom']}", f"2010 frames {shot('custom')} 1",
              "2100 call windows.2.switchTab 0", "2800 vis @class windowHolder", f"2810 rects @class {sel['class']}",
              f"2820 frames {shot('class')} 1", "2900 call showWindow 14", "3000 call windows.13.switchTab 1",
              "3800 vis @wheels windowHolder", "3900 quit"]
    lines = run_client(name, ["--offline", "--map", "kanji"], "\n".join(script), timeout=120,
                       env_extra={"KRUNKNATIVE_MODE": "online" if guest else "offline", "KRUNKNATIVE_FIXED_DELTA": "16",
                                  "KRUNKNATIVE_HTML_IMAGE_LOG": miss_log, "KRUNKNATIVE_WINDOW_SIZE": size},
                       storage=storage)
    if lines is None:
        return
    shown = {m.group(1): m.group(2) for m in (re.search(r"\[input_script\] vis t=\d+ @(\S+) windowHolder=(\S+)", l) for l in lines) if m}
    for label in ("loadout", "class", "wheels"):
        record(name, label, "windowHolder", "1", shown.get(label, "-"), shown.get(label) == "1")
    rects = {}
    for line in lines:
        m = re.search(r"\[input_script\] rects t=\d+ @(\S+) n=(\d+)(.*)", line)
        if m:
            rects[m.group(1)] = m.group(3).split()
    for label, (_, want_full, want_guest) in ICON_SELECTORS.items():
        want = want_guest if guest else want_full
        boxes, png = rects.get(label, []), frame_png(shot(label))
        record(name, label, "picture boxes", f">={want}", str(len(boxes)), len(boxes) >= want)
        blank = blank_boxes(png, boxes) if png else [("no frame", 0.0)]
        record(name, label, "blank pictures", "none", " ".join(f"{b}(sd {sd:.1f})" for b, sd in blank[:3]) or "none", not blank)
    # "undefined" is v7's own knife preview URL (textures/previews/weapons/undefined.png 404s on the real site too)
    misses = sorted({l.split("\t")[1] for l in read(miss_log).splitlines() if l.startswith("image-miss\t")} - {"undefined"})
    record(name, "all", "missing images", "none", " ".join(misses[:4]) or "none", not misses)


def sc_loadout_images():
    """Blank-image sweep of the loadout / class / skins surface (the user's "the primary skin in the
    loadout is invisible"): a fully equipped loadout at 1280x720 and 1920x1080, and a fresh guest
    (online mode, no account, no saved picks: every default preview). Before f75b57b the pixelated icons (class / tab icons,
    class cards) whose nearest-filter twin could not be made and the previews only the content tree
    has (the Grappler, every weapon past 19) drew nothing; the store's spin cards lost their vignette.
    Before ui_loadout_look the class box showed the default weapon icon under an equipped skin."""
    loadout_images_run("loadout-images", FULL_LOADOUT, "1280x720")
    loadout_images_run("loadout-images-1080", FULL_LOADOUT, "1920x1080")
    loadout_images_run("loadout-images-guest", {}, "1280x720", guest=True)


def loadout_images_web(name, storage, size, guest=False):
    """loadout_images_run in the web build (KN_WEB_DIST = its dist dir), where the browser loads every
    <img> itself from the asset host URL html_dom_asset_url built: headless Chromium (DevTools
    protocol), the same Loadout / Customize / Class tabs and class box. Every image request that
    fails (HTTP status >= 400 or a network error) is reported with its URL, every <img> that loaded
    no picture (complete, naturalWidth 0) with its src, and every picture box must show a picture in
    the page screenshot (blank_boxes)."""
    import asyncio
    import base64
    import urllib.request
    try:
        import websockets
    except ImportError:
        print(f"SKIP  {name}: needs python websockets", flush=True)
        return
    import shutil as _shutil
    chrome = _shutil.which("chromium-browser") or _shutil.which("chromium") or _shutil.which("google-chrome")
    dist = os.environ.get("KN_WEB_DIST", "")
    if not chrome or not os.path.isfile(os.path.join(dist, "client.wasm")):
        print(f"SKIP  {name}: needs chromium and KN_WEB_DIST (a web build's dist dir)", flush=True)
        return
    width, height = size.split("x")
    http_port, cdp_port = free_ports(2)
    own = [subprocess.Popen([sys.executable, "-m", "http.server", "--bind", "127.0.0.1", str(http_port)], cwd=dist,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True),
           subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                             "--enable-unsafe-swiftshader", "--hide-scrollbars", f"--remote-debugging-port={cdp_port}",
                             f"--user-data-dir={os.path.join(WORK, name + '-chrome')}", f"--window-size={width},{height}",
                             "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)]
    with LOCK:
        procs.extend(own)
    origin = f"http://127.0.0.1:{http_port}"
    sel = {k: v[0] for k, v in ICON_SELECTORS.items()}
    boxes_js = ("(sel => Array.from(document.querySelectorAll(sel)).map(e => { const b = e.getBoundingClientRect();"
                " return '-@' + [b.left, b.top, b.width, b.height].map(v => v.toFixed(1)).join(','); }))(%s)")
    broken_js = ("Array.from(document.images).filter(i => i.complete && i.naturalWidth === 0 && i.getAttribute('src')"
                 " && i.getClientRects().length).map(i => i.getAttribute('src'))")
    failed, requests = [], {}
    result = {}

    async def session():
        target = None
        for _ in range(200):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{cdp_port}/json/list", timeout=2) as r:
                    target = next((t for t in json.load(r) if t.get("type") == "page"), None)
                if target:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.1)
        if not target:
            return
        async with websockets.connect(target["webSocketDebuggerUrl"], max_size=1 << 28) as ws:
            ids, replies = iter(range(1, 1 << 30)), {}

            async def reader():
                async for raw in ws:
                    msg = json.loads(raw)
                    method, params = msg.get("method"), msg.get("params", {})
                    if method == "Network.requestWillBeSent":
                        requests[params["requestId"]] = params["request"]["url"]
                    elif method == "Network.responseReceived" and params["response"]["status"] >= 400:
                        failed.append((params["response"]["url"], params["response"]["status"]))
                    elif method == "Network.loadingFailed" and not params.get("canceled"):
                        failed.append((requests.get(params["requestId"], "?"), params.get("errorText", "")))
                    elif "id" in msg:
                        replies[msg["id"]] = msg

            task = asyncio.create_task(reader())

            async def call(method, **params):
                i = next(ids)
                await ws.send(json.dumps({"id": i, "method": method, "params": params}))
                for _ in range(1200):
                    if i in replies:
                        return replies.pop(i).get("result", {})
                    await asyncio.sleep(0.05)
                return {}

            async def evaluate(expr):
                r = await call("Runtime.evaluate", returnByValue=True, expression=expr)
                return r.get("result", {}).get("value")

            async def until(expr, seconds):
                for _ in range(int(seconds * 4)):
                    if await evaluate(expr):
                        return True
                    await asyncio.sleep(0.25)
                return False

            await call("Network.enable")
            await call("Page.enable")
            await call("Page.navigate", url=origin + "/favicon.png")  # the origin, for localStorage
            await asyncio.sleep(0.5)
            await evaluate("localStorage.clear(), %s.forEach(([k, v]) => localStorage.setItem(k, v)), 1" % json.dumps(list(storage.items())))
            # ?offline=1: the embedded sandbox server; mode=offline: offline mode (every skin owned), else a
            # guest in online mode (no account: no store items)
            await call("Page.navigate", url=origin + "/?offline=1&map=kanji" + ("" if guest else "&mode=offline"))
            ready = await until("!!(window.windows && document.getElementById('menuClassIcn')"
                                " && document.getElementById('menuClassIcn').getAttribute('src'))", 90)
            result["ready"] = ready
            if ready:
                await asyncio.sleep(3.0)
                steps = [("box", None), ("loadout", "showWindow(3), windows[2].switchTab(1)"),
                         ("custom", "windows[2].switchTab(2)"), ("class", "windows[2].switchTab(0)")]
                for label, action in steps:
                    if action:
                        await evaluate(action + ", 1")
                        await asyncio.sleep(2.5)
                    boxes = await evaluate(boxes_js % json.dumps(sel[label])) or []
                    shot = await call("Page.captureScreenshot", format="png")
                    png = os.path.join(WORK, f"{name}-{label}.png")
                    with open(png, "wb") as f:
                        f.write(base64.b64decode(shot.get("data", "")))
                    result[label] = (boxes, png, await evaluate(broken_js) or [])
            task.cancel()

    try:
        asyncio.run(asyncio.wait_for(session(), 240))
    except Exception as e:  # noqa: BLE001 - a dead browser is a failed probe, not a crash of the run
        result.setdefault("error", repr(e))
    finally:
        for p in own:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    record(name, "page", "menu ready", "1", "1" if result.get("ready") else result.get("error", "0"), result.get("ready"))
    broken = set()
    for label, (_, want_full, want_guest) in ICON_SELECTORS.items():
        if label not in result:
            continue
        boxes, png, broken_imgs = result[label]
        broken.update(broken_imgs)
        want = want_guest if guest else want_full
        record(name, label, "picture boxes", f">={want}", str(len(boxes)), len(boxes) >= want)
        blank = blank_boxes(png, boxes)
        record(name, label, "blank pictures", "none", " ".join(f"{b}(sd {sd:.1f})" for b, sd in blank[:3]) or "none", not blank)
    # the knife's v7 "undefined" preview 404s on the real site too
    images = sorted({f"{u} ({st})" for u, st in failed if re.search(r"\.(png|jpe?g|gif|webp|svg)(\?|$)", u) and "/undefined.png" not in u})
    record(name, "all", "failed image requests", "none", " ".join(images[:4]) or "none", not images)
    broken = sorted(b for b in broken if "undefined.png" not in b)
    record(name, "all", "broken <img>", "none", " ".join(broken[:4]) or "none", not broken)


def sc_loadout_images_web():
    """loadout-images on the web build (KN_WEB_DIST): the browser fetches the images itself."""
    loadout_images_web("loadout-images-web", FULL_LOADOUT, "1280x720")
    loadout_images_web("loadout-images-web-1080", FULL_LOADOUT, "1920x1080")
    loadout_images_web("loadout-images-web-guest", {}, "1280x720", guest=True)


def wsel_lines(lines, label):
    """{who: selection} of the `wsel @label` probe (PlayerArms.selection; who = preview / me)."""
    out = {}
    for line in lines:
        m = re.search(r"\[input_script\] wsel t=\d+ @" + re.escape(label) + r" who=(\S+) slot=(-?\d+) tp=(-?\d+) (.*)", line)
        if m:
            out[m.group(1)] = (int(m.group(2)), int(m.group(3)), m.group(4).strip())
    return out


def sc_loadout_preview():
    """The menu's class preview (class_preview.c, third-person model set) and our first-person
    viewmodel in game pick the same weapon, skin (model / texture keys), tint, sight, charm and wrist
    item for the same loadout (ui_loadout_look feeds both; `wsel` reads PlayerArms.selection):
      - Grinchmas + Acog + charm + wrist: before, the preview always mounted the Reflex Sight
        (attach_0: its Player never got the attachIndex) while the viewmodel had the Acog;
      - AJ47 (a forceAttach skin) with the Acog picked: v7 mounts the Reflex Sight on both;
      - the Piss (Object Manager): no model, the procedural hose on the first-person and the
        third-person (preview) model sets alike, in the skin colour.
    Menu and in-game frames are kept next to the logs (WORK) for review."""
    cases = {
        "acog": ({"classindex": "0", "skins": '{"1":2592}', "attachIndex": "1", "charms": "[5577]", "wristIndex": "4249",
                  "secondaryInd": "20"}, "attach=attach_1"),
        "force-attach": ({"classindex": "0", "skins": '{"1":4596}', "attachIndex": "1"}, "attach=attach_0"),
    }
    for case, (storage, want_attach) in cases.items():
        name = f"loadout-preview-{case}"
        shot = lambda label: os.path.join(WORK, f"{name}-{label}")  # noqa: E731
        script = ["0 menu", "1500 wsel @menu", f"1600 frames {shot('menu')} 1", "2000 play", "4500 wsel @game",
                  f"4600 frames {shot('game')} 1"]
        if case == "acog":
            script += ["5000 om add 65", "6000 wsel @piss", f"6100 frames {shot('piss')} 1", "6500 con thirdperson",
                       "7500 wsel @piss3", f"7600 frames {shot('piss3')} 1"]
        script.append("8000 quit")
        lines = run_client(name, ["--offline", "--map", "kanji"], "\n".join(script), timeout=120,
                           env_extra={"KRUNKNATIVE_MODE": "offline", "KRUNKNATIVE_FIXED_DELTA": "16"}, storage=storage)
        if lines is None:
            continue
        menu, game = wsel_lines(lines, "menu"), wsel_lines(lines, "game")
        preview = menu.get("preview", (-1, -1, "-"))
        me = game.get("me", (-1, -1, "-"))
        record(name, "menu", "preview model set", "third person", "third person" if preview[1] == 1 else str(preview[1]), preview[1] == 1)
        record(name, "menu", "preview sight", want_attach, preview[2], want_attach in preview[2].split())
        record(name, "game", "viewmodel model set", "first person", "first person" if me[1] == 0 else str(me[1]), me[1] == 0)
        record(name, "game", "viewmodel == preview", preview[2], me[2], me[2] != "-" and me[2] == preview[2])
        if case == "acog":
            first, third = wsel_lines(lines, "piss").get("me", (-1, -1, "-")), wsel_lines(lines, "piss3").get("me", (-1, -1, "-"))
            record(name, "piss", "first-person hose", "w=65 model=hose", first[2], "w=65" in first[2].split() and "model=hose" in first[2].split())
            record(name, "piss3", "third-person == first-person", first[2], third[2], third[1] == 1 and third[2] == first[2])


SCENARIOS = {
    "endscreen": sc_endscreen, "endscreen-tdm": sc_endscreen_team, "death": sc_death, "death-menu": sc_death_menu, "forced-spawn": sc_forced_spawn, "spectate": sc_spectate,
    "map-change": sc_map_change, "interact": sc_interact, "custom-gui": sc_custom_gui, "krunkscript": sc_krunkscript,
    "spray": sc_spray, "windows": sc_windows, "online": sc_online, "lost-end": sc_lost_during_end, "outdated": sc_outdated,
    "comp": sc_comp, "om-scroll": sc_om_scroll, "om-tools": sc_om_tools, "skins-grid": sc_skins_grid, "hub-items": sc_hub_items,
    "loadout-images": sc_loadout_images, "loadout-preview": sc_loadout_preview,
    "loadout-images-web": sc_loadout_images_web,
}
SCENARIOS["endscreen-cursor"] = sc_endscreen_cursor
SCENARIOS["server-list"] = sc_server_list
SCENARIOS["pointer-lock"] = sc_pointer_lock
SCENARIOS["pointer-lock-chat"] = sc_pointer_lock_chat
SCENARIOS["pointer-lock-next-map"] = sc_pointer_lock_next_map


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
