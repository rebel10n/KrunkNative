#!/usr/bin/env python3
"""KrunkNative third-person camera end-to-end (client/src/third_person.c, console `thirdperson` /
`firstperson`, doc/v7/decisions.md "Third person").

  python3 -I client/tests/third_person_e2e.py BUILD_DIR [FRAMES_DIR]      (run from the repository root)

  1. offline Kanji and Sandstorm (the client's sandbox server grants it): a headless client_bin
     with an input script (fixed 60 fps step) plays in first person (viewmodel, no own model),
     `thirdperson` (own model in the world pass, no viewmodel, the camera behind the shoulder),
     aiming down sights (first person while aimed, third person again after), backed against a
     wall (the camera pulled in, never inside or behind solid geometry), `firstperson` (viewmodel
     back), `/thirdperson on` typed as chat (the grant alone turns it on). Every frame of the third-person stretches is checked through `tpstate`
     (client/src/input_script.c); a frame of each step is written (FRAMES_DIR, kept).
     Then a death in third person (the death cam, the respawn third person again).
  2. a public server_bin (port from kn_proto.free_ports): `thirdperson` is refused with the
     server's notify, the camera stays first person and the server logs the denial.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-tp-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAMES = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(WORK, "frames")
os.makedirs(FRAMES, exist_ok=True)
results = []
procs = []

# Backed against a wall, facing away from it (feet x y z, yaw, pitch): third_person_test prints them
MAPS = {
    "kanji": ("assets/maps/kanji.json", "-331.33 -143.00 -16.33 0.7854 0"),
    "sandstorm": ("assets/maps/sandstorm_v3.json", "13.08 13.00 10.08 0.7854 0"),
}


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


def run_client(name, args, script, timeout=180):
    """client_bin headless with an input script (its events sorted by time, stable); its log lines."""
    events = [l for l in script.splitlines() if l.strip()]
    events.sort(key=lambda l: int(l.split()[0]))
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write("\n".join(events) + "\n")
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_FIXED_DELTA": "16.667", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
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


TP_RE = re.compile(r"\[input_script\] tpstate t=(\d+) view=(\d) model=(\d) own=(\d) viewmodel=(\d) keep=(\S+) "
                   r"cam=(\S+) eye=(\S+) wall=(\d)")


def tpstates(lines):
    out = []
    for line in lines:
        m = TP_RE.search(line)
        if m:
            out.append({"t": int(m.group(1)), "view": int(m.group(2)), "model": int(m.group(3)), "own": int(m.group(4)),
                        "viewmodel": int(m.group(5)), "keep": float(m.group(6)),
                        "cam": tuple(map(float, m.group(7).split(","))), "eye": tuple(map(float, m.group(8).split(","))),
                        "wall": int(m.group(9))})
    return out


def span(states, a, b):
    return [s for s in states if a <= s["t"] < b]


def dist(p, q):
    return sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5


def every(start, end, step=100):
    return "".join(f"{t} tpstate\n" for t in range(start, end, step))


def offline(key):
    path, backed = MAPS[key]
    shot = lambda n: os.path.join(FRAMES, f"{key}-{n}.png")  # noqa: E731
    # times from the first active frame after `play`
    script = ("0 play\n"
              f"1400 tpstate\n1400 shot {shot('1-first')}\n"
              "1500 con thirdperson\n" + every(2500, 4000) + f"3000 shot {shot('2-third')}\n"
              "3200 look 1.2 0\n" + f"3800 shot {shot('3-third-turned')}\n"
              "4000 mdown right\n" + every(4800, 5300) + f"5000 shot {shot('4-ads')}\n"
              "5400 mup right\n" + every(6500, 7000) +
              f"7000 pose {backed}\n" + every(7600, 9000) + f"8000 shot {shot('5-backed')}\n"
              "8500 look 0 0.6\n" + f"8900 shot {shot('6-backed-up')}\n"
              "9000 con firstperson\n" + every(9600, 10000) + f"9800 shot {shot('7-first-again')}\n"
              "10300 con say /thirdperson on\n" + every(11000, 11400) + "11500 quit\n")
    lines = run_client(key, ["--offline", path], script)
    st = tpstates(lines)
    check(f"{key}: the script ran (tpstate lines)", len(st) > 40, f"{len(st)} lines; log {os.path.join(WORK, key + '.log')}")
    if not st:
        return
    first = span(st, 0, 1500)
    check(f"{key}: first person: the viewmodel, no own model", first and all(s["viewmodel"] and not s["own"] and not s["view"] for s in first),
          str(first))
    third = span(st, 2500, 4000)
    check(f"{key}: thirdperson: third-person view, own model in the world, no viewmodel",
          third and all(s["view"] and s["model"] and s["own"] and not s["viewmodel"] for s in third),
          str([s for s in third if not (s["view"] and s["own"] and not s["viewmodel"])][:3]))
    check(f"{key}: thirdperson: the camera is behind the shoulder (3..15.1 from the eye), never in a wall",
          third and all(3.0 <= dist(s["cam"], s["eye"]) <= 15.1 and not s["wall"] for s in third),
          str([(round(dist(s["cam"], s["eye"]), 2), s["wall"]) for s in third][:5]))
    ads = span(st, 4800, 5300)
    check(f"{key}: aiming down sights: first person (viewmodel, no own model)",
          ads and all(not s["view"] and s["viewmodel"] and not s["own"] and dist(s["cam"], s["eye"]) < 0.5 for s in ads), str(ads[:2]))
    back = span(st, 6500, 7000)
    check(f"{key}: aim released: third person again", back and all(s["view"] and not s["viewmodel"] for s in back), str(back[:2]))
    backed = span(st, 7600, 9000)
    check(f"{key}: backed against a wall: never inside or behind geometry",
          backed and all(s["view"] and not s["wall"] and not s["viewmodel"] for s in backed),
          str([s for s in backed if s["wall"] or not s["view"]][:3]))
    check(f"{key}: backed against a wall: the camera is pulled in",
          backed and all(s["keep"] < 14.0 for s in backed), str([s["keep"] for s in backed][:6]))
    again = span(st, 9600, 10000)
    check(f"{key}: firstperson: the viewmodel is back, no own model, camera at the eye",
          again and all(not s["view"] and s["viewmodel"] and not s["own"] and dist(s["cam"], s["eye"]) < 0.5 for s in again),
          str(again[:2]))
    chat = span(st, 11000, 11400)
    check(f"{key}: /thirdperson typed in the chat: the server's grant turns it on",
          chat and all(s["view"] and s["own"] and not s["viewmodel"] for s in chat), str(chat[:2]))
    check(f"{key}: the console answered", any("Third Person - Enabled" in l for l in lines)
          and any("Third Person - Disabled" in l for l in lines))
    missing = [n for n in ("1-first", "2-third", "4-ads", "5-backed", "7-first-again") if not os.path.isfile(shot(n))]
    check(f"{key}: frames written", not missing, f"missing {missing}")


def death():
    """Third person through a death: the death cam (no third-person view, no own model in the
    world), then the respawn is third person again (the grant lasts the round)."""
    shot = lambda n: os.path.join(FRAMES, f"kanji-death-{n}.png")  # noqa: E731
    lines = run_client("death", ["--offline", MAPS["kanji"][0]],
                       "0 play\n1000 con thirdperson\n2000 tpstate\n2500 con kill\n" + every(3000, 5000, 500) +
                       f"4000 shot {shot('1-deathcam')}\n9000 respawn\n" + every(12000, 40000, 500) +
                       f"39800 shot {shot('2-respawned')}\n40200 quit\n", timeout=360)
    st = tpstates(lines)
    check("death: third person before the kill", span(st, 2000, 2001) and span(st, 2000, 2001)[0]["view"], str(st[:1]))
    dead = span(st, 3000, 5000)
    check("death: the death cam is not the third-person camera, no own model", dead and all(not s["view"] and not s["own"] for s in dead),
          str(dead[:2]))
    # the offline respawn takes a while after `respawn` (death delay, Play again): from the first
    # third-person frame on, every one must be
    later = span(st, 12000, 40000)
    first = next((i for i, s in enumerate(later) if s["view"]), None)
    alive = later[first:] if first is not None else []
    moved = alive and dist(alive[0]["eye"], dead[0]["eye"] if dead else (0, 0, 0)) > 1.0
    check("death: respawned in third person (model drawn, no viewmodel)",
          alive and moved and all(s["view"] and s["own"] and not s["viewmodel"] and not s["wall"] for s in alive),
          f"{len(alive)} of {len(later)} late frames; {alive[:1]}")


def lan_address():
    """This machine's non-loopback IPv4 (the default route's source; nothing is sent)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            address = s.getsockname()[0]
        return None if address.startswith("127.") else address
    except OSError:
        return None


def public():
    # A loopback peer is the server's own operator (net_client_is_dev: client->local), so the
    # client joins over this machine's LAN address: an ordinary player on a public server.
    address = lan_address()
    if not address:
        print("skip public (no non-loopback IPv4 address)")
        return
    (port,) = free_ports(1)
    server_log = os.path.join(WORK, "server.log")
    with open(server_log, "w") as out:
        server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", "none", "--port", str(port)], stdout=out,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(server)
    lines = run_client("public", ["--connect", f"{address}:{port}"],
                       "0 play\n1500 con thirdperson\n" + every(2500, 4000, 250) +
                       f"3000 shot {os.path.join(FRAMES, 'public-refused.png')}\n"
                       "4000 con thirdperson\n5000 tpstate\n5200 quit\n")
    os.killpg(server.pid, signal.SIGTERM)
    server.wait(timeout=10)
    st = tpstates(lines)
    check("public: connected and played (not a local peer)", any(f"connected to server {address}:{port}" in l for l in lines) and st
          and f"connected from {address}" in read(server_log),
          f"{len(st)} tpstate lines")
    check("public: thirdperson refused: first person stays (viewmodel, no own model)",
          st and all(not s["view"] and not s["model"] and s["viewmodel"] and not s["own"] for s in st), str(st[:2]))
    log = read(server_log)
    check("public: the server logged the denial", "third person denied" in log)
    check("public: the player got the server's notify / reply",
          any("Third person is off on this server" in l or "Not allowed here" in l for l in lines))


def main():
    try:
        for key in MAPS:
            if not os.path.isfile(MAPS[key][0]):
                print(f"skip {key} (gitignored map assets)")
                continue
            offline(key)
        if os.path.isfile(MAPS["kanji"][0]):
            death()
        public()
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
        if all(results) and results and not FRAMES.startswith(WORK):
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")
    print(f"frames in {FRAMES}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()
