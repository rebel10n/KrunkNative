#!/usr/bin/env python3
"""A web gateway restarting under a player, end to end (doc/backend.md 9.4 "rejoin", 10
"gateways"): server_bin + net-bridge/rtc (kn-rtc, WebRTC) + net-bridge --web (the page and the
WebSocket bridge) + the web client in headless Chromium (tools/kref/gateway-restart.js). Every
port from kn_proto.free_ports().

  python3 -I server/tests/gateway_restart_e2e.py BUILD_DIR WEB_DIST_DIR    (run from the repo root)

  A. WebRTC, a deploy restarts kn-rtc ("update" in KRUNK_NATIVE_STOP_REASON_FILE, SIGTERM): the
     gateway sends the browser its knet goodbye with an ERROR frame (GAME UPDATING, 004004) and the
     game server its goodbye; the page shows Game Updating + "Restarting..." and reloads itself
     (Settings > Client > Auto-restart for updates, default on: a deploy may have brought a newer
     page too; client/include/game_reload.h), reconnects through the restarted gateway and is back
     in game; the server freed the player at once (no knet timeout)
  B. WebSocket, net-bridge restarts (no reason file): an ERROR frame at a frame boundary ("Server
     restarting", 004006) and a 1012 close; the page shows it and reconnects
  C. WebRTC, kn-rtc is killed (-9: no word from anyone) and started again: the page shows the
     transport's own reason ("Disconnected: ...") + "Reconnecting...", and reconnects

Needs Go (the gateway is built into the scratch dir), node + net-bridge/node_modules and Chromium.
Exit 77 when one is missing.
"""
import json
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

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
DIST = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "dist/web")
ROOT = os.getcwd()
GAME, ICE, SIG, HTTP, CDPP = kn_proto.free_ports(5)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-gateway-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
REASON = os.path.join(WORK, "gateway-stop-reason")
CHROME = shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 200)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def start(cmd, log, env=None, cwd=ROOT):
    p = subprocess.Popen(cmd, cwd=cwd, env=dict(os.environ, **(env or {})), stdout=open(log, "a"), stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return p


def stop(p, sig=signal.SIGTERM, secs=10):
    if p.poll() is None:
        os.killpg(p.pid, sig)
    try:
        p.wait(secs)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


RTC_LOG = os.path.join(WORK, "rtc.log")
BRIDGE_LOG = os.path.join(WORK, "bridge.log")
GAME_LOG = os.path.join(WORK, "game.log")
GATEWAY_ENV = {"KRUNK_NATIVE_PORT": str(GAME), "KRUNK_NATIVE_STOP_REASON_FILE": REASON}


def start_rtc():
    n = read_log(RTC_LOG).count("[rtc] signaling")
    p = start([os.path.join(WORK, "kn-rtc"), "-http", "127.0.0.1:%d" % SIG, "-ice-addr", ":%d" % ICE], RTC_LOG, GATEWAY_ENV)
    wait_for(lambda: read_log(RTC_LOG).count("[rtc] signaling") > n, 10)
    return p


def start_bridge():
    n = read_log(BRIDGE_LOG).count("web listening on")
    p = start(["node", "-r", "ts-node/register/transpile-only", "index.ts", "--web"], BRIDGE_LOG,
              dict(GATEWAY_ENV, KRUNK_NATIVE_BRIDGE_PORT=str(HTTP), KRUNK_NATIVE_WEB_DIR=DIST), cwd=os.path.join(ROOT, "net-bridge"))
    wait_for(lambda: read_log(BRIDGE_LOG).count("web listening on") > n, 30)
    return p


def page(transport):
    url = "http://127.0.0.1:%d/?mode=offline&ws=ws://127.0.0.1:%d/native&transport=%s" % (HTTP, HTTP, transport)
    if transport == "rtc":
        url += "&rtc=http://127.0.0.1:%d/rtc" % SIG
    return url


def scenario(name, transport, restart):
    """Get in game over transport, run restart(), report what the page showed."""
    session = "webrtc session established" if transport == "rtc" else "connected to server"
    drv = subprocess.Popen(["node", "tools/kref/gateway-restart.js", str(CDPP), page(transport), "45", session], cwd=ROOT,
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    first = drv.stdout.readline().strip()
    check(f"{name}: in game over {transport}", first == "IN_GAME", first)
    if first != "IN_GAME":
        print(drv.stdout.read()[-2000:])
        drv.kill()
        return None
    restart()
    drv.stdin.write("go\n")
    drv.stdin.flush()
    out = drv.stdout.read()
    drv.wait()
    try:
        report = json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        check(f"{name}: driver report", False, out[-600:])
        return None
    return report


def main():
    missing = [w for w in ("go", "node") if not shutil.which(w)]
    if missing or not CHROME or not os.path.isdir(os.path.join(ROOT, "net-bridge", "node_modules")) \
            or not os.path.isfile(os.path.join(DIST, "client.wasm")):
        print("gateway_restart_e2e: needs go, node, net-bridge/node_modules, Chromium and a web build: skipped")
        return 77
    subprocess.run(["go", "build", "-trimpath", "-buildvcs=false", "-o", os.path.join(WORK, "kn-rtc"), "."],
                   cwd=os.path.join(ROOT, "net-bridge", "rtc"), check=True, env=dict(os.environ, GOTOOLCHAIN="local", CGO_ENABLED="0"))
    try:
        start([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(GAME), "server/tests/arena.json"], GAME_LOG)
        rtc = start_rtc()
        bridge = start_bridge()
        start([CHROME, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader",
               "--user-data-dir=" + os.path.join(WORK, "chrome"), "--remote-debugging-port=%d" % CDPP, "about:blank"],
              os.path.join(WORK, "chrome.log"))
        check("server, kn-rtc, net-bridge and Chromium up",
              wait_for(lambda: "Listening on UDP" in read_log(GAME_LOG), 10) and "[rtc] signaling" in read_log(RTC_LOG)
              and "web listening on" in read_log(BRIDGE_LOG) and wait_for(lambda: "DevTools listening" in read_log(
                  os.path.join(WORK, "chrome.log")), 15),
              read_log(BRIDGE_LOG)[-300:])

        # A. a deploy restarts kn-rtc
        state = {}

        def restart_rtc_update():
            nonlocal rtc
            state["timeouts"] = read_log(GAME_LOG).count("timed out")
            state["left"] = read_log(GAME_LOG).count("disconnected")
            with open(REASON, "w") as f:
                f.write("update\n")
            state["t"] = time.time()
            stop(rtc)
            os.unlink(REASON)
            state["freed"] = wait_for(lambda: read_log(GAME_LOG).count("disconnected") > state["left"] and time.time(), 5)
            rtc = start_rtc()

        rep = scenario("A", "rtc", restart_rtc_update)
        if rep:
            screens = " || ".join(rep["screens"])
            log = "\n".join(rep["console"])
            check("A: kn-rtc told the player GAME UPDATING before closing",
                  re.search(r'stopping \(1 sessions\): telling players "GAME UPDATING" \(004004\)', read_log(RTC_LOG)) is not None,
                  read_log(RTC_LOG)[-400:])
            check("A: the page got the gateway's ERROR (knet goodbye with a frame)",
                  "disconnected from server: GAME UPDATING (004004)" in log, log[-600:])
            check("A: the error screen says Game Updating and Restarting... (the auto-restart reloads the page)",
                  "Game Updating" in screens and "Restarting..." in screens, screens[-400:])
            check("A: the page reloaded itself for the update", "reloading the page" in log, log[-600:])
            check("A: the game server freed the player at once (the gateway's goodbye, not a knet timeout)",
                  state.get("freed") and state["freed"] - state["t"] < 3 and read_log(GAME_LOG).count("timed out") == state["timeouts"],
                  read_log(GAME_LOG)[-400:])
            check("A: a new WebRTC session through the restarted gateway", rep["sessionAgain"], log[-600:])
            check("A: back in game", rep["inGameAgain"], screens[-300:])

        # B. net-bridge restarts (no reason file: "Server restarting")
        def restart_bridge():
            nonlocal bridge
            stop(bridge)
            bridge = start_bridge()

        rep = scenario("B", "ws", restart_bridge)
        if rep:
            screens = " || ".join(rep["screens"])
            log = "\n".join(rep["console"])
            check("B: net-bridge told the session before closing",
                  re.search(r'stopping: telling 1 session\(s\) "Server restarting" \(004006\)', read_log(BRIDGE_LOG)) is not None
                  and 'told "Server restarting"' in read_log(BRIDGE_LOG), read_log(BRIDGE_LOG)[-400:])
            check("B: the page got the bridge's ERROR frame", "disconnected from server: Server restarting (004006)" in log, log[-600:])
            check("B: the error screen says Server restarting and Reconnecting...",
                  "Server restarting" in screens and "Reconnecting..." in screens, screens[-400:])
            check("B: reconnected through the restarted bridge", rep["sessionAgain"], log[-600:])
            check("B: back in game", rep["inGameAgain"], screens[-300:])

        # C. kn-rtc crashes (-9) and systemd starts it again 2 s later
        def kill_rtc():
            nonlocal rtc
            stop(rtc, signal.SIGKILL)
            time.sleep(2)
            rtc = start_rtc()

        rep = scenario("C", "rtc", kill_rtc)
        if rep:
            screens = " || ".join(rep["screens"])
            log = "\n".join(rep["console"])
            m = re.search(r"disconnected from server: (.+)", log)
            check("C: the page names the transport's reason (no ERROR from anyone)",
                  m is not None and re.search(r"no data from the server|network path to the connection gateway|gateway closed", m.group(1)),
                  log[-600:])
            check("C: the error screen shows Disconnected: <reason> and Reconnecting...",
                  "Disconnected: " in screens and "Reconnecting..." in screens and "Reason: " in screens, screens[-400:])
            check("C: reconnected over WebRTC", rep["sessionAgain"], log[-600:])
            check("C: back in game", rep["inGameAgain"], screens[-300:])
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
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
