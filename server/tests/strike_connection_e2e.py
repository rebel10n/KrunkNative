#!/usr/bin/env python3
"""Krunker Strike's connection popups end to end (client/src/ui/strike_connect.c): FRVR 1.37.2's
Lost Connection (dSe) and Game Updating (oN) popups, POPUP_WARNING over the Strike page.

  python3 -I server/tests/strike_connection_e2e.py BUILD_DIR      (run from the repository root)

Every port from kn_proto.free_ports(); every process this test starts is stopped by it.

  A. the server dies (SIGKILL, a crash): the Strike client (--flavor strike --connect) on the main menu
     shows Lost Connection once its reconnect fails: the popup's node is in the page, its Reconnect button
     retries (log "[strike] reconnect"), and the page keeps working (the popup is answered, the menu is up).
  B. the server stops for an update (console "quit update": ERROR 004004 GAME UPDATING): the Strike page
     shows the Game Updating popup before the client restarts itself (game_reload.c).

Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-conn-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.25)
    return False


def kill_group(p, sig=signal.SIGKILL):
    if p.poll() is None:
        try:
            os.killpg(p.pid, sig)
        except OSError:
            pass
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()


class DirectServer:
    """server_bin without a brain on a fixed port, its console on stdin."""
    count = 0

    def __init__(self, port):
        DirectServer.count += 1
        self.log = os.path.join(WORK, "server%d.log" % DirectServer.count)
        # --flavor strike: a Strike client is refused by a base server (004007, "This is a Krunker Box Office server")
        self.p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--flavor", "strike", "--port", str(port)], cwd=ROOT,
                                  stdout=open(self.log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE,
                                  start_new_session=True)
        procs.append(self.p)
        self.up = bool(wait_for(lambda: "Listening on" in read_log(self.log), 20))

    def quit_update(self):
        try:
            self.p.stdin.write(b"quit update\n")
            self.p.stdin.flush()
        except OSError:
            pass
        try:
            self.p.wait(20)
        except subprocess.TimeoutExpired:
            kill_group(self.p)


def start_client(name, port, script_lines):
    script = os.path.join(WORK, name + ".script")
    with open(script, "w") as f:
        f.write("\n".join(script_lines) + "\n")
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL",
                                                            "KRUNKNATIVE_LAUNCHER", "KRUNKNATIVE_UPDATES_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_NET_TRANSPORT": "tcp"})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--flavor", "strike", "--connect", "127.0.0.1:%d" % port],
                         cwd=ROOT, stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                         start_new_session=True)
    procs.append(p)
    return p, log


def nodes_of(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else None


def case_lost(port):
    server = DirectServer(port)
    check("A: direct server up", server.up, read_log(server.log)[-300:])
    p, log = start_client("lost", port, [
        "0 menu",
        "20000 nodes @kill-here .frvr-button",      # the server is killed once the script passes this
        "80000 nodes @lost .mobPopAlert .frvr-button",  # the popup is up 60 s later (the reconnect has failed)
        "81000 clicksel .mobPopAlert .frvr-button",     # Reconnect (id selectors miss in nodes: class ones match)
        "82000 quit"])
    booted = wait_for(lambda: "[strike] screen MAIN_MENU" in read_log(log), 90)
    check("A: the Strike main menu is up on the server", booted, read_log(log)[-400:])
    if not booted:
        kill_group(p)
        kill_group(server.p)
        return
    passed = wait_for(lambda: re.search(r"\[input_script\] nodes t=\d+ @kill-here ", read_log(log)), 60)
    kill_group(server.p)  # a crash: the socket closes under the client
    check("A: the script reached the kill point", passed, read_log(log)[-300:])
    shown = wait_for(lambda: "[strike] screen POPUP_WARNING" in read_log(log), 60)
    check("A: Lost Connection pops up over the page", shown, read_log(log)[-600:])
    try:
        p.wait(120)
    except subprocess.TimeoutExpired:
        kill_group(p)
        check("A: the client finished its script", False, read_log(log)[-400:])
        return
    text = read_log(log)
    check("A: the popup's Reconnect button is in the page 60 s after the drop", nodes_of(text, "lost") == 1, str(nodes_of(text, "lost")))
    check("A: Reconnect answers the popup and retries", "[strike] action reconnect" in text and "[strike] reconnect" in text, text[-400:])
    # Reconnect retries the server: the attempt starts (the spinner's connect), and a refused retry shows the
    # popup again (a fresh Lost Connection), so the popup itself is not checked after the click
    after = text.split("[strike] reconnect", 1)[1] if "[strike] reconnect" in text else ""
    check("A: Reconnect starts a connection attempt", "connecting (" in after, after[:200])


def case_updating(port):
    server = DirectServer(port)
    check("B: direct server up (update)", server.up, read_log(server.log)[-300:])
    p, log = start_client("updating", port, ["0 menu", "150000 quit"])
    booted = wait_for(lambda: "[strike] screen MAIN_MENU" in read_log(log), 90)
    check("B: the Strike main menu is up before the update", booted, read_log(log)[-400:])
    if not booted:
        kill_group(p)
        kill_group(server.p)
        return
    server.quit_update()
    shown = wait_for(lambda: "[strike] screen POPUP_WARNING" in read_log(log), 60)
    check("B: Game Updating pops up on the server's update", shown, read_log(log)[-600:])
    # the restarted server takes the client back: the popup goes (strike_connect_tick), or the client
    # restarts itself first (game_reload.c's auto-restart: the new process has no popup)
    time.sleep(2.0)
    server2 = DirectServer(port)
    check("B: the restarted server is up", server2.up, read_log(server2.log)[-300:])
    done = wait_for(lambda: "[strike] popup POPUP_WARNING closed" in read_log(log)
                    or "[reload] the server restarted for an update: relaunching" in read_log(log), 120)
    text = read_log(log)
    check("B: the popup goes once the client is back on the server (or the client restarts itself)", done,
          text[-500:])
    kill_group(p)
    kill_group(server2.p)
    kill_group(server.p)


def main():
    try:
        (port_a, port_b) = kn_proto.free_ports(2)
        case_lost(port_a)
        case_updating(port_b)
    finally:
        for p in procs:
            kill_group(p)
        shutil.rmtree(WORK, ignore_errors=True)
    ok = bool(results) and all(results)
    print("ALL PASS" if ok else "SOME FAILED", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
