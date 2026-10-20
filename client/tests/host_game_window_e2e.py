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
    Play-offline or Get-the-dedicated-server button.
Exit code 0 when every check passed; the input-script log is kept in ~/.cache/kn-hostgame-e2e-* on failure.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

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
def run_native(script, timeout=240):
    path = os.path.join(WORK, "native.script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, "native.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
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


script = [
    "0 menu",
    "300 call showWindow 42",
    "800 call openHostWindow 0 0",
    "1300 vis @open hostHeader hstTab0 hstTab1 hstTab2 hstTab3 startServBtn hostCMapPickr hostFlowCommand",
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
    '1695 call knHostFlow "map" "kanji"',
    "1700 clickid startServBtn",
    "1800 text @msg [id=hostGameMsg]",
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

if failures:
    print(f"host_game_window_e2e: {len(failures)} failed (logs in {WORK})")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("host_game_window_e2e: all checks passed")
