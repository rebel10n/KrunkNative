#!/usr/bin/env python3
"""F5 (Reload Game: the full reload, which ends with `reconnect`), console `reconnect` /
`sandbox` / `offline <map>` end-to-end test (client/src/ui/ui_controls.c reload_key,
client/src/game_reload.c, client/src/ui/ui_console.c ui_console_reconnect / cmd_sandbox /
cmd_offline, client/src/main.c --sandbox / --map).

  python3 -I server/tests/reconnect_key_e2e.py BUILD_DIR      (run from the repository root)

  1. online: server_bin on a free port (kn_proto.free_ports); a headless client_bin joins it
     (--connect) and plays; F5 reloads and rejoins the same server (the server sees a second join)
     and the player is back in the game (input script `state`: menu closed, a weapon, a new spawn)
  2. offline: client_bin --offline --map kanji plays the catalogue's Kanji; F5 restarts the local
     game on the same map; `sandbox` switches to the sandbox map (catalogue "Test"), F5 restarts
     it there; after `disconnect` the console's `reconnect` starts the offline game again on that
     map; `offline Burg` switches maps; `help` lists `sandbox`
  3. client_bin --sandbox starts on the sandbox map
Keys and console lines go through KRUNKNATIVE_INPUT_SCRIPT (times from `play`). Needs a headless
GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-f5-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
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


def run_client(name, args, script, timeout=90):
    """client_bin headless with an input script; returns its log lines."""
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    return read(log).splitlines()


def between(lines, start, end=None):
    """The lines after the first match of `start` up to the first later match of `end`."""
    out, on = [], False
    for line in lines:
        if not on and re.search(start, line):
            on = True
            continue
        if on and end and re.search(end, line):
            break
        if on:
            out.append(line)
    return out


def states(lines):
    """input_script `state` lines -> dicts"""
    out = []
    for line in lines:
        m = re.search(r"\[input_script\] state t=(\d+) slot=(-?\d+)/(\d+) weapon=(.+?) menu=(\d) locked=(\d) pos=(\S+)", line)
        if m:
            out.append({"t": int(m.group(1)), "loadout": int(m.group(3)), "weapon": m.group(4), "menu": int(m.group(5)),
                        "pos": m.group(7)})
    return out


def in_game(state):
    return state and state["menu"] == 0 and state["loadout"] > 0 and state["weapon"] != "-"


def main():
    try:
        online()
        offline()
        sandbox_flag()
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
        if all(results) and results:
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


def online():
    (port,) = free_ports(1)
    server_log = os.path.join(WORK, "server.log")
    with open(server_log, "w") as out:
        server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port)], stdout=out,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(server)
    lines = run_client("online", ["--connect", f"127.0.0.1:{port}"],
                       "0 play\n3000 state\n4000 down F5\n4050 up F5\n12000 state\n13000 quit\n")
    os.killpg(server.pid, signal.SIGTERM)
    server.wait(timeout=10)
    here = f"connected to server 127.0.0.1:{port}"
    st = states(lines)
    check("online: the client joins the server and plays", any(here in l for l in lines) and st and in_game(st[0]),
          f"states {st}")
    after = between(lines, r"\[controls\] reload key")
    check("online: F5 reloads, then reconnects", any("[reload] rejoining the server" in l for l in after)
          and any(f"[mm] reconnect: 127.0.0.1:{port}" in l for l in after))
    check("online: F5 reconnects to the same server", any(here in l for l in between(lines, r"\[controls\] reload key", r"state t=1")),
          "; ".join(l for l in after if "connected" in l))
    check("online: the server sees the client join again", read(server_log).count("connected from 127.0.0.1") >= 2)
    check("online: still in the game after the reconnect (spawned again)",
          len(st) >= 2 and in_game(st[1]) and st[1]["pos"] != st[0]["pos"], f"states {st}")


def offline():
    lines = run_client("offline", ["--offline", "--map", "kanji"],
                       "0 play\n2000 state\n3000 down F5\n3050 up F5\n6000 state\n"
                       "7000 con sandbox\n10000 state\n11000 down F5\n11050 up F5\n14000 state\n"
                       "15000 con disconnect\n17000 con reconnect\n20000 state\n"
                       "21000 con help\n22000 con offline Burg\n25000 state\n26000 quit\n")
    st = states(lines)
    check("offline: --map kanji starts on the catalogue map", any(re.search(r"startup map kanji: maps/\S*kanji\.json", l) for l in lines)
          and any("[sandbox] Loaded map Kanji (0)" in l for l in lines))
    check("offline: the player plays", st and in_game(st[0]), f"states {st}")
    first = between(lines, r"\[controls\] reload key", r"\] sandbox$")
    check("offline: F5 restarts the local game on the same map",
          any(re.search(r"\[sandbox\] Loaded map 0 mode", l) for l in first) and any("Restarted the offline game" in l for l in first),
          "; ".join(first))
    check("offline: in the game again after F5", len(st) >= 2 and in_game(st[1]), f"states {st}")

    sandbox = between(lines, r"\] sandbox$", r"\[controls\] reload key")
    m = [re.search(r"\[sandbox\] Loaded map (\d+) mode", l) for l in sandbox]
    slot = next((x.group(1) for x in m if x), None)
    check("offline: `sandbox` switches to the sandbox map", slot is not None and len(st) >= 3 and st[2]["pos"].startswith("0.000,0.000"),
          f"slot {slot}, states {st}")
    second = between(lines, r"\] sandbox$")
    second = between(second, r"\[controls\] reload key", r"\] disconnect$")
    check("sandbox: F5 restarts it on the sandbox map", slot is not None and any(f"[sandbox] Loaded map {slot} mode" in l for l in second),
          "; ".join(second))

    third = between(lines, r"\] disconnect$", r"\] help$")
    check("offline: `reconnect` after `disconnect` starts the offline game again on the sandbox map",
          any("[sandbox] Loaded map Test (" in l for l in third) and any("Restarted the offline game" in l for l in third),
          "; ".join(third))
    check("console help lists `sandbox`", any(re.search(r"^\[console\]\s+sandbox\s+play offline on the sandbox", l) for l in lines))
    switch = between(lines, r"\] offline Burg$")
    check("`offline <map>` switches the offline game's map", any(re.search(r"\[sandbox\] Loaded map \d+ mode", l) for l in switch)
          and not any(re.search(r"failed|not found", l) for l in switch), "; ".join(switch[:5]))


def sandbox_flag():
    lines = run_client("sandbox", ["--sandbox"], "0 play\n1000 state\n2000 quit\n")
    st = states(lines)
    check("--sandbox starts on the sandbox map", any("[sandbox] Loaded map Test (" in l for l in lines) and st and in_game(st[0]),
          "; ".join(l for l in lines if "sandbox]" in l or "startup map" in l))


if __name__ == "__main__":
    main()
