#!/usr/bin/env python3
"""Developer console autofill for a non-dev session (ui_console.c player_target_candidates, PT_OWN; shared/src/chat_commands.c cmd_ping).

  python3 -I client/tests/console_autofill_nondev_e2e.py BUILD_DIR      (run from the repository root)

A non-dev session needs a connection the server does not take as local and an account without the DEV flag.
The server marks a connection local when its address is 127.x (server/src/net.c: client->local), and a local
connection is dev (net_client_is_dev). So the server runs on this machine's LAN address with a bot, and the client
(a guest: no account service in a headless run) connects to that address, not to loopback. Its console then reads
its own roster: con_dev() is false (no offline, no DEV account), so `ping` offers only the caller's own name and no
selector, and the server's cmd_ping refuses another player's ping ("You can only view your own ping.").

Checks (the client log and the hint rows):
  1. the console's `ping ` rows are the caller's own name only (the bot is in the roster, and is not listed);
  2. `ping @` offers nothing (no selector, as cmd_ping refuses them);
  3. `ping blue` (the bot's name) offers nothing;
  4. Enter `ping Blue_Bot` is refused by the server: "You can only view your own ping.", so the parser agrees;
  5. Enter `ping` gives the caller's own ping ("Your ping: ").
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it), and a LAN address
(override with KN_AUDIT_LAN_IP). Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-nondev-", dir=CACHE if os.path.isdir(CACHE) else None)
PORT = int(os.environ.get("KN_AUDIT_NONDEV_PORT", "21988"))
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def lan_ip():
    """This machine's non-loopback IPv4 address (a UDP socket's route, no packet sent)."""
    override = os.environ.get("KN_AUDIT_LAN_IP")
    if override:
        return override
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.1.1", 9))
        return s.getsockname()[0]
    finally:
        s.close()


def typing(text):
    lines = []
    parts = text.split(" ")
    for j, part in enumerate(parts):
        if j:
            lines.append("down SPACE\nup SPACE")
        if part:
            lines.append(f"type {part}")
    return lines


def typed_line(out, t, text):
    out.append(f"{t} down CTRL+A"); out.append(f"{t + 10} up CTRL+A"); t += 40
    for line in typing(text):
        for part in line.split("\n"):
            out.append(f"{t} {part}"); t += 12
    return t


def build_script(address):
    out = ["0 menu", "200 down GRAVE", "220 up GRAVE"]  # `menu` first: a later `play` enters the game (input_script)
    t = 400
    t = typed_line(out, t, f"connect {address}")
    out.append(f"{t} down ENTER"); out.append(f"{t + 10} up ENTER"); t += 300
    # the join (the session comes up a while after the connect line, the lobby's map loads): `play` is sent until
    # the client has spawned (client->me is the player spawned in game; a play before the join does nothing)
    for _ in range(12):
        t += 4000
        out.append(f"{t} play")
    t += 15000
    for key, text in (("p1", "ping "), ("p2", "ping @"), ("p3", "ping blue")):
        t = typed_line(out, t, text)
        t += 250
        for n in range(1, 13):
            out.append(f"{t} text @row_{key}_{n} .hints>.hintItem:nth-child({n})"); t += 6
        t += 60
    for key, text in (("e1", "ping Blue_Bot"), ("e2", "ping")):
        t = typed_line(out, t, text)
        out.append(f"{t} down ENTER"); out.append(f"{t + 10} up ENTER"); t += 2000
    out.append(f"{t} quit")
    return "\n".join(out) + "\n"


ROW = re.compile(r"\[input_script\] text t=\d+ @row_(\S+?)_(\d+) n=(\d+) checked=\d value=.*? text=(.*)$")


def read_rows(text):
    rows = {}
    for line in text.splitlines():
        m = ROW.search(line)
        if m and int(m.group(3)):
            rows.setdefault(m.group(1), {})[int(m.group(2))] = m.group(4).strip()
    out = {}
    for key, by_n in rows.items():
        names = []
        for n in range(2, 13):
            if n not in by_n:
                break
            names.append(by_n[n])
        out[key] = names
    return out


def main():
    ip = lan_ip()
    address = f"{ip}:{PORT}"
    server_log = os.path.join(WORK, "server.log")
    server = subprocess.Popen([f"{BUILD}/server_bin", "--cfg", "none", "--headless", "--port", str(PORT),
                               "assets/maps/official/10_kanji.json"],
                              stdin=subprocess.PIPE, stdout=open(server_log, "w"), stderr=subprocess.STDOUT,
                              start_new_session=True)
    try:
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                socket.create_connection((ip, PORT), timeout=1).close()
                break
            except OSError:
                time.sleep(0.5)
        check("the server listens on the LAN address", time.time() < deadline, address)
        server.stdin.write(b"bot 1 name=Blue_Bot\n")  # the server console: the bot is in the roster
        server.stdin.flush()
        time.sleep(3)

        script = os.path.join(WORK, "nondev.script")
        with open(script, "w") as f:
            f.write(build_script(address))
        cfg = os.path.join(WORK, "cfg"); state = os.path.join(WORK, "state")
        os.makedirs(cfg, exist_ok=True); os.makedirs(state, exist_ok=True)
        log = os.path.join(WORK, "client.log")
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                    "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": state, "KRUNKNATIVE_NO_BROWSER": "1",
                    "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
        with open(log, "w") as out:
            p = subprocess.Popen([f"{BUILD}/client_bin"], stdout=out, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, env=env, start_new_session=True)
            try:
                p.wait(timeout=900)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        with open(log, errors="replace") as f:
            text = f.read()
        check("the client ran its script", "events from" in text, "")
        rows = read_rows(text)
        names_seen = [line for line in text.splitlines() if "[console] Server:" in line or "[cmd]" in line]

        p1 = rows.get("p1", [])
        p2 = rows.get("p2", [])
        p3 = rows.get("p3", [])
        if os.environ.get("KN_AUDIT_EXPECT_DEV") == "1":  # the control: a loopback connection is dev (NET_RIGHTS_DEV)
            check("1. dev control: `ping ` offers the roster (the caller and the bot)",
                  "Blue_Bot" in p1 and "Guest_1" in p1, f"got {p1}")
            check("2. dev control: `ping @` offers @me only (ping names one player: @bots would be refused)", p2 == ["@me"], f"got {p2}")
            check("3. dev control: `ping blue` offers the bot", p3 == ["Blue_Bot"], f"got {p3}")
            check("4. dev control: Enter `ping Blue_Bot`: the server answers the bot's ping",
                  "[console] Server: Blue_Bot's ping" in text, f"replies: {names_seen[-6:]}")
        else:
            check("1. `ping ` offers only the caller's own name (the bot is not offered)",
                  len(p1) == 1 and "Blue_Bot" not in p1 and not any(x.startswith("@") or x == "*" for x in p1),
                  f"got {p1}")
            check("2. `ping @` offers no selector", not p2, f"got {p2}")
            check("3. `ping blue` offers nothing (the bot is another player)", not p3, f"got {p3}")
            check("4. Enter `ping Blue_Bot`: the server refuses another player's ping",
                  "[console] Server: You can only view your own ping." in text, f"replies: {names_seen[-6:]}")
        check("5. Enter `ping`: the caller's own ping", "[console] Server: Your ping" in text, "")
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
finally:
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}", flush=True)
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)
