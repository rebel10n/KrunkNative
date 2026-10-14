#!/usr/bin/env python3
"""The version toggle on server_bin (doc/v10/version-toggle.md, shared/include/ruleset.h).

  python3 -I server/tests/ruleset_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: standalone server_bin processes from generated server.cfg files.
  1. a default server (no ruleset key) plays v7: INIT is 18 bytes with ruleset 2 (RulesetId
     V7_1_4 + 1); a HELLO without the ruleset mask (every build before the toggle) plays there.
  2. `ruleset v10` in server.cfg (and --ruleset): INIT carries 3; the log names Krunker v10.0.2;
     the server-info query answers "ruleset":"v10"; a peer whose HELLO has no mask (v7 only) is
     refused with ERROR 004010; a peer announcing v10 (mask bit 2) spawns.
  3. an unknown ruleset (`--ruleset v3`) stops server_bin with exit code 2.
  4. client_bin (headless, Game version v7 saved) joins the v10 server: it adopts v10 for the
     session ("[ruleset] the server plays Krunker v10.0.2"), plays, and its console `ruleset`
     reports v10 playing / v7 saved; offline with v10 saved the sandbox plays v10.
  5. the v3.1.1 menu page (ui/v3, client/include/ruleset_ui.h): with v7 saved the page is ui/'s
     (v7 DOM markers); the console `ruleset v3` saves v3 (knRuleset); the next start loads
     ui/v3/index.html (v3.1.1 markers present, v7 ones absent), says that v3.1.1 gameplay is not
     ported (log + notice), plays offline on the v7.1.4 ruleset (console: "Playing Krunker v7.1.4
     (v7) offline", setting and page v3.1.1), and writes a menu and an in-game frame.
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORTS = kn_proto.free_ports(3)
CLIENT_TIMEOUT = 90
WORK = tempfile.mkdtemp(prefix="kn-ruleset-e2e-", dir=os.path.expanduser("~/.cache"))
INIT, ERROR, SPAWN = 2, 30, 4
MASK_V7, MASK_V10 = 1 << 1, 1 << 2
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def start_server(i, extra, args=()):
    cfg = os.path.join(WORK, f"s{i}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Ruleset #{i}"\nport {PORTS[i]}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\n{extra}')
    log = os.path.join(WORK, f"s{i}.log")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"), *args],
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p, log


def wait_port(port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    start = data.find(b"{")
    return json.loads(data[start:].split(b"\0", 1)[0].decode()) if start >= 0 else {}


def connect(port, hello):
    c = kn_proto.Conn("127.0.0.1", port)
    c.send(kn_proto.HELLO, bytes(hello))
    c.join(name="rs")
    init = c.read(until=INIT, timeout=5)
    return c, init


def spawned(c):
    c.ent(class_index=0, lag_comp=1.0)
    end = time.time() + 5
    while time.time() < end:
        p = c.read(until=SPAWN, timeout=1)
        if p is not None and kn_proto.spawn_info(p)["is_you"]:
            return True
    return False


def refused(c, code):
    c.read(timeout=3)
    return any(t == ERROR and kn_proto.error_info(p)[1] == code for t, p in c.packets)


def run_client(name, args, script, saved=None, cfg_name=None):
    """client_bin headless with an input script and a private config (cfg_name: another run's,
    to see what it saved); returns its log text."""
    cfg = os.path.join(WORK, f"{cfg_name or name}-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    if saved:
        with open(os.path.join(cfg, "krunknative", "knRuleset.json"), "w") as f:
            f.write(saved)
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    log = os.path.join(WORK, f"{name}.log")
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env)
        procs.append(p)
        try:
            p.wait(timeout=CLIENT_TIMEOUT)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            check(f"{name}: client_bin finished its script", False, log)
    with open(log, errors="replace") as f:
        return f.read()


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    # 1. default: v7
    p, log = start_server(0, "")
    check("v7 server up", wait_port(PORTS[0]))
    c, init = connect(PORTS[0], [1, 3, 1])  # a HELLO from before the toggle (no mask)
    check("v7 INIT: 18 bytes, ruleset 2 (v7.1.4)", init is not None and len(init) == 18 and init[17] == 2, repr(init and init[:18]))
    check("v7: an old HELLO plays", spawned(c))
    c.close()
    info = query(PORTS[0])
    check("v7 server-info ruleset", info.get("ruleset") == "v7", str(info.get("ruleset")))

    # 2. ruleset v10 (server.cfg)
    p, log = start_server(1, "ruleset v10\n")
    check("v10 server up", wait_port(PORTS[1]))
    old, init = connect(PORTS[1], [1, 3, 1])
    check("v10 INIT ruleset 3 (v10.0.2)", init is not None and len(init) >= 18 and init[17] == 3, repr(init and init[:18]))
    check("v10: a v7-only peer is refused (004010)", refused(old, "004010"))
    old.close()
    new, init = connect(PORTS[1], [1, 4, 1, MASK_V7 | MASK_V10])
    check("v10: a peer announcing v10 spawns", spawned(new))
    new.close()
    info = query(PORTS[1])
    check("v10 server-info ruleset", info.get("ruleset") == "v10", str(info.get("ruleset")))
    time.sleep(0.3)
    with open(log) as f:
        text = f.read()
    check("v10 log names the ruleset", "Ruleset Krunker v10.0.2 (v10)" in text)

    # 3. --ruleset v3: not built
    p, log = start_server(2, "", ("--ruleset", "v3"))
    try:
        rc = p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        rc = None
    with open(log) as f:
        text = f.read()
    check("--ruleset v3: exit 2", rc == 2 and "Unknown ruleset" in text, f"rc {rc}")

    # 4. the client adopts the server's ruleset; offline it plays the saved one
    text = run_client("join-v10", ["--connect", f"127.0.0.1:{PORTS[1]}"], "0 play\n2500 state\n2600 con ruleset\n3500 quit\n", saved="v7")
    check("client: adopts the v10 server's ruleset", "[ruleset] the server plays Krunker v10.0.2: adopting it" in text)
    check("client: plays there", "connected to server" in text and "weapon=Assault Rifle menu=0" in text)
    check("client: console ruleset (v10 playing, v7 saved)",
          "Playing Krunker v10.0.2 (v10)" in text and "Game version setting: v7.1.4" in text)
    text = run_client("offline-v10", ["--offline", "assets/maps/kanji.json"], "0 play\n1500 con ruleset\n2500 quit\n", saved="v10")
    check("client: offline plays the saved v10", "Playing Krunker v10.0.2 (v10) offline" in text)

    # 5. the v3.1.1 page: switch to it, restart, play
    text = run_client("switch-v3", ["--offline", "assets/maps/kanji.json"],
                      "0 play\n1000 vis @page merchHolder killCardHolder genericPop bottomLeftHealthBar\n1200 con ruleset v3\n"
                      "1800 con ruleset\n2400 quit\n", saved="v7")
    vis = vis_line(text, "@page")
    check("v7 saved: ui/'s page (v7 markers, no v3.1.1 ones)",
          vis.get("merchHolder") == "-" and vis.get("killCardHolder") == "-" and vis.get("genericPop") in ("0", "1")
          and vis.get("bottomLeftHealthBar") in ("0", "1"), str(vis))
    check("console `ruleset v3` saves v3.1.1", "[ruleset] saved Krunker v3.1.1 (v3)" in text
          and "Game version setting: v3.1.1, menu page: v7.1.4" in text, "")
    saved = os.path.join(WORK, "switch-v3-cfg", "krunknative", "knRuleset.json")
    check("knRuleset storage holds v3", os.path.exists(saved) and "v3" in open(saved).read())
    menu_png, game_png = os.path.join(WORK, "v3-menu.png"), os.path.join(WORK, "v3-game.png")
    text = run_client("offline-v3", ["--offline", "assets/maps/kanji.json"],
                      f"0 menu\n600 vis @page merchHolder killCardHolder confPop genericPop bottomLeftHealthBar menuWindow\n"
                      f"700 shot {menu_png}\n900 play\n2000 state\n2200 shot {game_png}\n2400 con ruleset\n3000 quit\n",
                      cfg_name="switch-v3")
    check("v3 saved: startup loads ui/v3", "[ruleset] menu page: ui/v3 (Krunker v3.1.1)" in text)
    vis = vis_line(text, "@page")
    check("v3 page DOM (v3.1.1 markers present, v7 ones absent)",
          vis.get("merchHolder") in ("0", "1") and vis.get("killCardHolder") in ("0", "1") and vis.get("confPop") in ("0", "1")
          and vis.get("genericPop") == "-" and vis.get("bottomLeftHealthBar") == "-" and vis.get("menuWindow") in ("0", "1"), str(vis))
    check("v3: says its gameplay is not ported (log + notice)",
          "plays the Krunker v7.1.4 ruleset (v3.1.1 gameplay is not ported yet)" in text
          and "[ruleset] offline: Krunker v3.1.1 gameplay is not ported yet: games play Krunker v7.1.4 rules" in text)
    check("v3: plays offline on the v7.1.4 ruleset",
          "Playing Krunker v7.1.4 (v7) offline" in text and "Game version setting: v3.1.1, menu page: v3.1.1" in text
          and "weapon=Assault Rifle menu=0" in text)
    check("v3: menu and in-game frames written", all(os.path.exists(p) and os.path.getsize(p) > 10000 for p in (menu_png, game_png)),
          f"{menu_png} {game_png}")


def vis_line(text, label):
    """The ids of the first `vis <label> ...` probe line: {id: "1" | "0" | "-"}."""
    for line in text.splitlines():
        if line.startswith("[input_script] vis ") and f" {label} " in line + " ":
            return dict(tok.split("=", 1) for tok in line.split()[3:] if "=" in tok)
    return {}


try:
    main()
finally:
    for p in procs:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
if results and all(results):
    shutil.rmtree(WORK, ignore_errors=True)
    print(f"{sum(results)}/{len(results)} passed")
else:
    print(f"{sum(results)}/{len(results)} passed  (logs: {WORK})")
sys.exit(0 if results and all(results) else 1)
