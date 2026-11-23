#!/usr/bin/env python3
"""The Host Game window, v10.0.2's layout (client/src/ui/host_flow_ui.c, ui/windows/8.html).

  python3 -I client/tests/host_game_window_e2e.py BUILD_DIR      (from the repository root)

Host Game (window 42) -> Custom Game Setup opens v10's own Host window (showWindow(8) / openHostWindow):
  - the header line and the four tabs v10 has: Select, Game Settings, Advanced, Server;
  - Start Game is v10's #startServBtn in the tab row (a button, not a tab): it creates the room
    (createPrivateRoom). Without a brain it says so in #hostGameMsg and changes nothing else;
  - the Select tab is v10's: the community banner, "Default Krunker Maps" with a checkbox per map;
  - the Server tab has v10's Privacy section and the one addition, the dedicated server command (the
    server_bin line, with its Copy command). No server name, port, public address, no-console field, no
    Play-offline or Get-the-dedicated-server button;
  - createPrivateRoom's checks in v10.0.7's order (deob-10.0.7.js:204669-204698): a map first (a checked one, the chosen
    game or Raw Game Data), then a mode; Raw Game Data (windows.host.raw) is on Custom's Select tab and counts as the map;
    Start Game gives its button back after the answer (customRoomResponse);
  - a community map (selectHostMap): v10.0.7's card (Change Game, the trash, #commMap), "Loading preset..." with Start
    Game greyed and no onclick while getMapPreset is out (a stub brain answers late), then Force Default Settings on
    (Game Settings / Advanced greyed, the rows hidden) and KR Rewards with the map's funds; the rows are the preset's.
    Clicking Force Default Settings off shows the rows and turns KR Rewards off; the trash is Custom with no game again.
Exit code 0 when every check passed; the input-script log is kept in ~/.cache/kn-hostgame-e2e-* on failure.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-hostgame-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []

TABS = [("hstTab0", "Select"), ("hstTab1", "Game Settings"), ("hstTab2", "Advanced"), ("hstTab3", "Server")]


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def fold(s):
    return re.sub(r"\s+", " ", s or "").strip()


# ---------------------------------------------------------------- native
def run_native(script, timeout=240, name="native", brain=None):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        args = [f"{BUILD}/client_bin", "--offline", "--map", "kanji"] + (["--brain", brain] if brain else [])
        p = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT,
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


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ " + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def nodes(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ " + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return m.group(2) if m else None


def grep(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=\S+ n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


def checked(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=\d+ checked=(\d)", log)
    return int(m.group(1)) if m else -1


def value(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=\d+ checked=\d value=(.*?) text=", log)
    return m.group(1) if m else None


def attr(log, label):
    m = re.search(r"\[input_script\] attr t=\d+ " + re.escape(label) + r" n=(\d+) \S+?=(.*)", log)
    return m.group(2).strip() if m else None


def style(log, label):
    m = re.search(r"\[input_script\] style t=\d+ " + re.escape(label) + r" \S+?=(\S*)", log)
    return m.group(1) if m else None


# getMapPreset's stub (the brain's /webhooks/general/maps/config): late, so "Loading preset..." is seen first
PRESET = {"modes": [1], "settings": {"maxPlayers": 6}}


class Stub(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/webhooks/general/maps/config"):
            time.sleep(2.5)
            body = json.dumps({"da": json.dumps(PRESET), "fu": 1500}).encode()
            self.send_response(200)
        else:
            body = b'{"error":"not found"}'
            self.send_response(404)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


script = [
    "0 menu",
    "300 call showWindow 42",
    "800 call openHostWindow 0 0",
    "1300 vis @open hostHeader hstTab0 hstTab1 hstTab2 hstTab3 startServBtn hostCMapPickr hostFlowCommand rawMapData",
]
for i, (tab, _) in enumerate(TABS):
    script.append(f"1400 text @tab{i} [id={tab}]")
script += [
    "1410 text @start [id=startServBtn]",
    "1450 text @header [id=hostHeader]",
    "1460 nodes @maps input[id^='gameMap']",
    "1490 grep @krunker Default [id=menuWindow]",
    "1500 call windows.7.switchTab 3",
    "1600 vis @server hostFlowCommand hostFlowCommandBlock",
    "1650 text @cmd [id=hostFlowCommand]",
    "1660 grep @copy Copy [id=hostFlowCommandBlock]",
    "1670 grep @offline offline [id=menuWindow]",
    "1680 grep @getserver Get [id=hostFlowCommandBlock]",
    "1690 nodes @fields input[id^='hostFlow']",
    # v10's order: no map (and no mode) says the map first
    "1510 call windows.7.switchTab 1",
    "1520 clickid gameMode0",
    "1530 call windows.7.switchTab 0",
    "1540 clickid startServBtn",
    "1550 text @nomap [id=hostGameMsg]",
    # Raw Game Data is a map: then the mode
    "1560 clickid rawMapData",
    '1570 type {"name":"Raw"}',
    "1580 clickid startServBtn",
    "1590 text @nomode [id=hostGameMsg]",
    "1592 call windows.7.switchTab 1",
    "1593 clickid gameMode0",
    "1594 call windows.7.switchTab 0",
    '1695 call knHostFlow "map" "kanji"',
    "1700 clickid startServBtn",
    "1800 text @msg [id=hostGameMsg]",
    "1810 style @after startServBtn:pointer-events",
    "1900 quit",
]
log = run_native("\n".join(script))

opened = vis(log, "@open")
check(opened.get("hostHeader") == "1" and opened.get("hstTab0") == "1",
      f"the Host window opens with v10's header line and tabs ({opened})")
for i, (tab, name) in enumerate(TABS):
    got = fold(text(log, f"@tab{i}"))
    check(got == name, f"tab {i + 1} is v10's {name!r} ({got!r})")
check(fold(text(log, "@start")) == "Start Game", f"Start Game is v10's button in the tab row ({text(log, '@start')!r})")
check(fold(text(log, "@header")).startswith("Custom Game Setup"), f"the header line is the host type's title ({text(log, '@header')!r})")
check(opened.get("hostCMapPickr") == "1", "Select: v10's community banner is shown")
check(nodes(log, "@maps") >= 10, f"Select: one checkbox per Krunker map ({nodes(log, '@maps')})")
check(grep(log, "@krunker") >= 1, "Select: the 'Default Krunker Maps' list")

server = vis(log, "@server")
check(server.get("hostFlowCommand") == "1", f"Server: the dedicated server command is shown ({server})")
check(fold(text(log, "@cmd")).startswith("server_bin"), f"the command is one server_bin line ({text(log, '@cmd')!r})")
check(grep(log, "@copy") >= 1, "the command has its Copy command button")
check(grep(log, "@offline") == 0, "no Play this map offline button")
check(grep(log, "@getserver") == 0, "no Get the dedicated server button")
check(nodes(log, "@fields") == 0, "no server name, port, public address or public port fields")
msg = fold(text(log, "@msg"))
check("Box Office" in msg, f"Start Game without a brain says so in #hostGameMsg ({msg!r})")
check(opened.get("rawMapData") == "1", f"Select: Custom's Raw Game Data field (windows.host.raw) ({opened})")
check(fold(text(log, "@nomap")) == "Select at least one Game or Map",
      f"no map and no mode: the map is checked first ({text(log, '@nomap')!r})")
check(fold(text(log, "@nomode")) == "Select at least one Mode",
      f"Raw Game Data counts as the map, then the mode is checked ({text(log, '@nomode')!r})")
check(style(log, "@after") in ("all", "-"), f"the refusal gives Start Game back (pointer-events {style(log, '@after')!r})")

# ---------------------------------------------------------------- a community map (selectHostMap)
stub = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
threading.Thread(target=stub.serve_forever, daemon=True).start()
community = [
    "0 menu",
    '300 call selectHostMap "Bhop_VIP" "" 0 "eskrr"',
    "700 vis @loading hostGameNameHolder commMap defaultSetts forcedSettings rawMapData hostCMapPickr",
    "710 text @loadmsg [id=hostGameMsg]",
    "720 attr @loadclick onclick [id=startServBtn]",
    "730 attr @loadtab3 class [id=hstTab3]",
    "5000 vis @card hostGameNameHolder commMap defaultSetts krRewards rawMapData",
    "5010 text @name [id=commMap]",
    "5020 attr @tab1 class [id=hstTab1]",
    "5030 attr @fs style [id=forcedSettings]",
    "5040 text @default [id=defaultSetts]",
    "5050 text @kr [id=krRewards]",
    "5060 grep @funds 1,500 [id=menuWindow]",
    "5070 text @mode1 [id=gameMode1]",
    "5080 text @mp [id=customSmaxPlayers]",
    "5090 nodes @maps input[id^='gameMap']",
    "5100 clickid defaultSetts",
    "5200 style @fs2 forcedSettings:display",
    "5210 attr @tab1b class [id=hstTab1]",
    "5220 text @kr2 [id=krRewards]",
    "5300 call openHostWindow 1",
    "5600 vis @trash hostGameNameHolder hostCMapPickr rawMapData",
    "5700 quit",
]
clog = run_native("\n".join(community), name="community", brain="http://127.0.0.1:%d" % stub.server_address[1])
stub.shutdown()
loading = vis(clog, "@loading")
check(loading.get("hostGameNameHolder") == "1" and loading.get("commMap") == "1" and loading.get("hostCMapPickr") in ("0", "-", None),
      f"the chosen game is v10.0.7's card, not the banner ({loading})")
check(fold(text(clog, "@loadmsg")) == "Loading preset...", f"getMapPreset out: 'Loading preset...' ({text(clog, '@loadmsg')!r})")
check(attr(clog, "@loadclick") in ("", "-"), f"... Start Game has no onclick while loading ({attr(clog, '@loadclick')!r})")
check("bigMenTabDis" in (attr(clog, "@loadtab3") or ""), f"... the Server tab is greyed while loading ({attr(clog, '@loadtab3')!r})")
check(loading.get("defaultSetts") in ("0", "-", None) and loading.get("rawMapData") in ("0", "-", None),
      f"... no switches, no Raw Game Data while loading ({loading})")
card = vis(clog, "@card")
check(fold(text(clog, "@name")) == "Bhop_VIP", f"#commMap names the game ({text(clog, '@name')!r})")
check(checked(clog, "@default") == 1, "the preset came: Force Default Settings is on")
check("bigMenTabDis" in (attr(clog, "@tab1") or ""), f"... Game Settings greyed ({attr(clog, '@tab1')!r})")
check("display:none" in (attr(clog, "@fs") or "").replace(" ", ""), f"... the rows hidden ({attr(clog, '@fs')!r})")
check(checked(clog, "@kr") == 1 and grep(clog, "@funds") >= 1,
      f"the map has funds: KR Rewards on, 'Available 1,500 KR' ({card}, {checked(clog, '@kr')}, {grep(clog, '@funds')})")
check(checked(clog, "@mode1") == 1 and value(clog, "@mp") == "6", f"the rows are the preset's (mode 1, maxPlayers {value(clog, '@mp')!r})")
check(nodes(clog, "@maps") == 0 and card.get("rawMapData") in ("0", "-", None), "a chosen game: no map list, no Raw Game Data")
check(style(clog, "@fs2") == "block" and "bigMenTabDis" not in (attr(clog, "@tab1b") or "x") and checked(clog, "@kr2") == 0,
      f"Force Default Settings off: the rows shown, the tabs back, KR Rewards off ({style(clog, '@fs2')!r}, {attr(clog, '@tab1b')!r})")
trash = vis(clog, "@trash")
check(trash.get("hostGameNameHolder") in ("0", "-", None) and trash.get("hostCMapPickr") == "1" and trash.get("rawMapData") == "1",
      f"the trash (openHostWindow(true)): Custom with no game again ({trash})")

if failures:
    print(f"host_game_window_e2e: {len(failures)} failed (logs in {WORK})")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("host_game_window_e2e: all checks passed")
