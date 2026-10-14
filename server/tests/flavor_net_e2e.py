#!/usr/bin/env python3
"""Krunker Strike is a product FLAVOR of Krunker Box Office (doc/v10/krunker-strike.md,
shared/include/flavor.h). flavor-net carries the flavor across the wire and refuses cross-flavor
peers, mirroring how the ruleset mask is handled (ruleset_e2e.py).

  python3 -I server/tests/flavor_net_e2e.py BUILD_DIR      (run from the repository root)

The flavor rides in the high nibble (bits 4-7) of the HELLO `rulesets` byte and the INIT `ruleset`
byte, beside the ruleset wire id in the low nibble (shared/include/shared.h NET_WIRE_FLAVOR). Flavor
0 (base) is a zero nibble, so a base build is byte-identical and a flavor-less peer (net-bridge, the
server-list probe, a pre-flavor build) is accepted. The server refuses a peer whose HELLO announces
a non-base flavor different from its own; the client refuses a server whose INIT advertises a
different flavor. So every cross-flavor pair is turned away by whichever side is non-base.

No database or coordinator: standalone server_bin processes from generated server.cfg files.

Server side (kn_proto, no GL):
  base server
    - a strike HELLO is refused (ERROR names the server flavor, never spawns);
    - a base HELLO and a flavor-less HELLO both spawn; its INIT advertises flavor 0 (byte 3).
  strike server
    - a base HELLO and a flavor-less HELLO both spawn (backward compatible, the client refuses us
      from our INIT instead); a strike HELLO spawns; its INIT advertises flavor 1 (strike);
    - a HELLO announcing some other non-base flavor is refused (ERROR names Krunker Strike).

Client side (client_bin headless, KN_FLAVOR=... so the launch never depends on the --flavor CLI):
  - a strike client refuses a base server and a base client refuses a strike server, each with a
    clear flavor message and no spawn;
  - a strike client joins and plays on a strike server (same-flavor succeeds, no refusal).
  Skipped (not failed) when client_bin cannot init here (no fonts / GL); set KRUNKNATIVE_ASSETS_DIR
  to a checkout that has assets/css/fonts to run them.

Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORTS = kn_proto.free_ports(2)
WORK = tempfile.mkdtemp(prefix="kn-flavor-net-e2e-", dir=os.path.expanduser("~/.cache"))
INIT, SPAWN, ERROR = 2, 4, 30
# HELLO `rulesets` byte: NET_HELLO_RULESETS (bit 2) in the low nibble + the flavor in the high nibble
# (shared.h NET_WIRE_RULESET_BYTE). None = a flavor-less HELLO (3 bytes, no rulesets byte at all).
RULESETS = 1 << 2
HELLO_FLAVORLESS = None
HELLO_BASE = RULESETS | (0 << 4)    # 0x04, byte-identical to a pre-flavor build
HELLO_STRIKE = RULESETS | (1 << 4)  # 0x14
HELLO_OTHER = RULESETS | (2 << 4)   # 0x24, a non-base flavor this build does not have
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def note(text):
    print(f"NOTE  {text}", flush=True)


def start_server(i, extra, args=()):
    cfg = os.path.join(WORK, f"s{i}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Flavor #{i}"\nport {PORTS[i]}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\n{extra}')
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


def attempt(port, rulesets_byte):
    """Direct connect with a HELLO carrying `rulesets_byte` (None = flavor-less); drive toward a
    spawn. Returns (spawned, refused, err_message, init_flavor_nibble).

    A cross-flavor refusal is an ERROR right after the INIT (the server kicks when it reads the
    HELLO). It is read BEFORE sending any ENT: writing to the half-closed socket would RST away the
    buffered ERROR, so refusal detection must not race a write."""
    c = kn_proto.Conn("127.0.0.1", port)
    hello = bytes([1, 4, 2]) if rulesets_byte is None else bytes([1, 4, 2, rulesets_byte])
    c.send(kn_proto.HELLO, hello)
    c.join(name="fn")
    init = c.read(until=INIT, timeout=5)
    init_flavor = (init[17] >> 4) if (init is not None and len(init) >= 18) else None
    refused = c.read(until=ERROR, timeout=1.5) is not None  # the kick's ERROR, if any, before we write
    spawned = False
    if init is not None and not refused:
        c.ent(class_index=0, lag_comp=1.0)
        c.read(until=SPAWN, timeout=5)
        spawned = any(t == SPAWN and (kn_proto.spawn_info(p) or {}).get("is_you") for t, p in c.packets)
    err = next((kn_proto.error_info(p)[0] for t, p in c.packets if t == ERROR), None)
    c.close()
    return spawned, refused, err, init_flavor


def read_log(path):
    time.sleep(0.3)
    with open(path, errors="replace") as f:
        return f.read()


def assets_dir():
    """A checkout with the UI fonts, so client_bin can init headless. Honours KRUNKNATIVE_ASSETS_DIR,
    else the working tree's assets/. None means the client cases are skipped (bare worktree)."""
    for cand in [os.environ.get("KRUNKNATIVE_ASSETS_DIR"), "assets"]:
        if cand and os.path.exists(os.path.join(cand, "css", "fonts", "font2.ttf")):
            return os.path.abspath(cand)
    return None


def run_client(name, kn_flavor, port, timeout, assets):
    """client_bin headless, driven by an input script, flavor via KN_FLAVOR (the --flavor CLI is
    flavor-core's, orthogonal here). A refused client sits on the error screen and does not quit, so
    the timeout kill is expected for those; the assertions read the log either way."""
    cfg = os.path.join(WORK, f"{name}-cfg")
    os.makedirs(cfg, exist_ok=True)
    script = os.path.join(WORK, f"{name}.script")
    with open(script, "w") as f:
        f.write("0 play\n2500 state\n3500 quit\n")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR", "KN_FLAVOR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_ASSETS_DIR": assets})
    if kn_flavor:
        env["KN_FLAVOR"] = kn_flavor
    log = os.path.join(WORK, f"{name}.log")
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--connect", f"127.0.0.1:{port}"],
                             stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env)
        procs.append(p)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")

    # the two servers, one of each flavor
    _, base_log = start_server(0, "")
    _, strike_log = start_server(1, "flavor strike\n")
    check("base server up", wait_port(PORTS[0]))
    check("strike server up", wait_port(PORTS[1]))
    check("strike server: Flavor: Krunker Strike in the log", "Flavor: Krunker Strike (strike)" in read_log(strike_log))

    # --- server side: the base server (kn_proto) ---
    spawned, refused, err, flav = attempt(PORTS[0], HELLO_BASE)
    check("base server + base HELLO spawns", spawned and not refused, str(err))
    check("base server advertises flavor 0 in INIT", flav == 0, str(flav))
    spawned, refused, err, _ = attempt(PORTS[0], HELLO_FLAVORLESS)
    check("base server + a flavor-less HELLO spawns (net-bridge / probe)", spawned and not refused, str(err))
    spawned, refused, err, _ = attempt(PORTS[0], HELLO_STRIKE)
    check("base server refuses a strike HELLO (no spawn)", refused and not spawned, str(err))
    check("base server refusal names the server flavor", bool(err) and "Krunker Box Office" in err, str(err))

    # --- server side: the strike server (kn_proto) ---
    spawned, refused, err, flav = attempt(PORTS[1], HELLO_STRIKE)
    check("strike server + strike HELLO spawns", spawned and not refused, str(err))
    check("strike server advertises flavor 1 in INIT", flav == 1, str(flav))
    spawned, refused, err, _ = attempt(PORTS[1], HELLO_FLAVORLESS)
    check("strike server + a flavor-less HELLO spawns (backward compatible)", spawned and not refused, str(err))
    spawned, refused, err, flav = attempt(PORTS[1], HELLO_BASE)
    check("strike server accepts a base HELLO but still advertises strike (client refuses us)",
          spawned and not refused and flav == 1, f"spawn={spawned} refused={refused} flavor={flav}")
    spawned, refused, err, _ = attempt(PORTS[1], HELLO_OTHER)
    check("strike server refuses an other-flavor HELLO (no spawn)", refused and not spawned, str(err))
    check("strike server refusal names Krunker Strike", bool(err) and "Krunker Strike" in err, str(err))

    check("base server: no spurious flavor refusal of matching peers",
          "Refusing client" not in read_log(base_log) or "peer flavor 0" not in read_log(base_log))

    # --- client side: real client_bin refuses / accepts by flavor ---
    assets = assets_dir()
    if not assets:
        note("client_bin flavor cases SKIPPED: no assets/css/fonts here (set KRUNKNATIVE_ASSETS_DIR)")
        return

    text = run_client("strike-vs-base", "strike", PORTS[0], 25, assets)
    check("strike client refuses a base server (clear message, no spawn)",
          "refusing server: flavor mismatch" in text and "weapon=Assault" not in text)
    text = run_client("base-vs-strike", "base", PORTS[1], 25, assets)
    check("base client refuses a strike server (vice versa, no spawn)",
          "refusing server: flavor mismatch" in text and "weapon=Assault" not in text)
    text = run_client("strike-vs-strike", "strike", PORTS[1], 45, assets)
    check("strike client joins and plays a strike server (same-flavor succeeds)",
          "connected to server" in text and "weapon=Assault Rifle" in text and "refusing server: flavor mismatch" not in text)


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
