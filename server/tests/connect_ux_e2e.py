#!/usr/bin/env python3
"""Server switches and reconnects look like a game load, not a lost connection (connect-ux).

  python3 -I server/tests/connect_ux_e2e.py BUILD_DIR      (run from the repository root)

client/src/ui/menu_ui.c menu_poll_connection / menu_show_connecting_map, client/src/net/events.c
client_net_give_up, client/src/net/net.c net_retry_wait (NET_RETRY_BUDGET), server/tests/kn_proto.py
free_ports. Two headless client_bin runs against server_bin:

  1. switch: a headless client plays on server A; `connect` to server B, which is not up yet. The
     transport retries (refused) while the page shows the loading spinner (.lds-ring) and no error box
     (.instructionsHeader absent). Server B starts; a retry connects, the client is in the game again,
     and the switch never showed the error screen ("[ui log] error screen" absent).
  2. give up: the client plays on server A, which is then killed (no ERROR: a dropped session). The
     spinner stays up through the transport's backoff; after NET_RETRY_BUDGET failures the transport
     gives up: the Lost Connection box shows with a Retry button (the first tab) and the give-up text
     in the log. Server A is started again and Retry (call knRetry) brings the client back in the game.

Samples are read with the input script's `nodes` / `text` lines (class selectors: the id selectors do
not see the page's boot markup). Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL;
KRUNKNATIVE_EGL_DEVICE picks it). KRUNKNATIVE_ASSETS_DIR is passed through (worktrees lack the
untracked font dirs). Prints PASS/FAIL lines; exit 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-connect-ux-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return None


def start_server(port, name):
    log = os.path.join(WORK, f"{name}.log")
    out = open(log, "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port)], stdout=out,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return p


def start_client(name, args, script):
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    out = open(log, "w")
    p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def states(text):
    out = []
    for m in re.finditer(r"\[input_script\] state t=(\d+) slot=(-?\d+)/(\d+) weapon=(.+?) menu=(\d)", text):
        out.append({"t": int(m.group(1)), "loadout": int(m.group(3)), "weapon": m.group(4), "menu": int(m.group(5))})
    return out


def in_game(st):
    return bool(st) and st["menu"] == 0 and st["loadout"] > 0 and st["weapon"] != "-"


def nodes_at(text, label, lo, hi):
    """match counts of the `nodes @label sel` lines whose script time is in [lo, hi)"""
    return [int(m.group(2)) for m in re.finditer(r"\[input_script\] nodes t=(\d+) " + re.escape(label) + r" elements=\d+ match=(\d+)", text)
            if lo <= int(m.group(1)) < hi]


def text_at(text, label, lo, hi):
    """the `text @label sel` lines whose script time is in [lo, hi): (t, n, text)"""
    out = []
    for m in re.finditer(r"\[input_script\] text t=(\d+) " + re.escape(label) + r" n=(\d+) checked=\d+ value=.*? text=(.*)", text):
        if lo <= int(m.group(1)) < hi:
            out.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return out


def finish(p, timeout=30):
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


def switch_scenario():
    port_a, port_b = free_ports(2)
    a = start_server(port_a, "a1")
    p, log = start_client("switch", ["--connect", f"127.0.0.1:{port_a}"],
                          "0 play\n2500 state\n3000 con connect 127.0.0.1:%d\n"
                          "3300 nodes @spin .lds-ring\n3300 nodes @box .instructionsHeader\n"
                          "3350 text @tab .instructionsTab\n"
                          "9000 state\n9100 nodes @box .instructionsHeader\n"
                          "9400 respawn\n9800 state\n9900 nodes @spin .lds-ring\n"
                          "10500 quit\n" % port_b)
    # the samples during the refused retries are logged before server B starts
    sampled = wait_for(lambda: nodes_at(read(log), "@box", 3300, 3400) or None, 60)
    text = read(log)
    st0 = states(text)
    check("switch: the client plays on server A first", bool(st0) and in_game(st0[0]), str(st0[:1]))
    check("switch: the client switches to the target (direct connect)", "[mm] direct connect 127.0.0.1:%d" % port_b in text,
          text[-300:])
    spin = nodes_at(text, "@spin", 3300, 3400)
    check("switch: the loading spinner shows while the target is down", spin and spin[0] >= 1, str(spin))
    box = nodes_at(text, "@box", 3300, 3400)
    check("switch: no error box while retrying (no .instructionsHeader)", box and box[0] == 0, str(box))
    check("switch: no error screen while retrying", "[ui log] error screen" not in text,
          [l for l in text.splitlines() if "error screen" in l][:2])

    b = start_server(port_b, "b1")  # the next retry (a few seconds' backoff) finds it
    wait_for(lambda: "connected to server 127.0.0.1:%d" % port_b in read(log), 40)
    finish(p, 60)
    text = read(log)
    st = states(text)
    check("switch: connected to the target", "connected to server 127.0.0.1:%d" % port_b in text, text[-300:])
    check("switch: back in the game on the target", len(st) >= 2 and in_game(st[-1]), str(st[-2:]))
    box = nodes_at(text, "@box", 9100, 9200)
    check("switch: no error box after a successful switch", box and box[0] == 0, str(box))
    check("switch: the error screen never showed", "[ui log] error screen" not in text,
          [l for l in text.splitlines() if "error screen" in l][:3])
    check("switch: the old server saw the client leave", "connected from 127.0.0.1" in read(os.path.join(WORK, "a1.log")))
    for s in (a, b):
        os.killpg(s.pid, signal.SIGTERM)


def giveup_scenario():
    port_a = free_ports(1)[0]
    a = start_server(port_a, "a2")
    p, log = start_client("giveup", ["--connect", f"127.0.0.1:{port_a}"],
                          "0 play\n3000 state\n"
                          "20000 nodes @spin .lds-ring\n20000 nodes @box .instructionsHeader\n"
                          "50000 nodes @box .instructionsHeader\n50100 text @tab .instructionsTab\n"
                          "53000 call knRetry\n53500 respawn\n58000 state\n60000 quit\n")
    seen = wait_for(lambda: "connected to server 127.0.0.1:%d" % port_a in read(log) and any(s["t"] >= 3000 for s in states(read(log))), 60)
    check("giveup: the client plays on the server", seen, read(log)[-300:])
    os.killpg(a.pid, signal.SIGKILL)  # no ERROR: the session drops
    a.wait()
    gave_up = wait_for(lambda: "gave up after" in read(log), 90)
    check("giveup: the transport gives up after the retry budget", gave_up, read(log)[-500:])
    # the server comes back; the client stays on the disconnected screen until Retry
    connects_before = read(log).count("connected to server 127.0.0.1:%d" % port_a)
    a2 = start_server(port_a, "a2b")
    time.sleep(4)
    check("giveup: no reconnect without Retry (the server is up again, the client holds)",
          read(log).count("connected to server 127.0.0.1:%d" % port_a) == connects_before, str(connects_before))
    finish(p, 90)
    text = read(log)
    st = states(text)
    during = nodes_at(text, "@spin", 20000, 20100)
    check("giveup: the spinner shows while the transport retries", during and during[0] >= 1, str(during))
    early_box = nodes_at(text, "@box", 20000, 20100)
    check("giveup: no error box while the transport still retries", early_box and early_box[0] == 0, str(early_box))
    final_box = nodes_at(text, "@box", 50000, 50100)
    check("giveup: the disconnected screen (Lost Connection box) shows when it gives up", final_box and final_box[0] >= 1, str(final_box))
    tab = text_at(text, "@tab", 50100, 50200)
    check("giveup: the disconnected screen has a Retry button (its first tab)", tab and tab[0][2].strip() == "Retry", str(tab))
    gave_log = [l for l in text.splitlines() if "[ui log] error screen" in l]
    check("giveup: the error screen says the client gave up", any("gave up after" in l for l in gave_log), str(gave_log[:2]))
    check("giveup: Retry reconnects to the server", text.count("connected to server 127.0.0.1:%d" % port_a) > connects_before,
          str(text.count("connected to server")))
    check("giveup: back in the game after Retry", len(st) >= 2 and in_game(st[-1]), str(st[-2:]))
    os.killpg(a2.pid, signal.SIGTERM)


def main():
    try:
        switch_scenario()
        giveup_scenario()
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
        if results and all(results):
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()
