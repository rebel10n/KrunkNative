#!/usr/bin/env python3
"""Server billboard ad end-to-end test (server.cfg billboard / billboard_image).

  python3 -I server/tests/billboard_e2e.py BUILD_DIR      (run from the repository root)

1. billboard + billboard_image set: a player joining a running round (late joiner) gets
   NET_PACKET_BILLBOARD (33) and NET_PACKET_BILLBOARD_IMAGE (36) before its INIT, and again
   before the INIT of the next map (map change).
2. billboard "" (unset): no billboard packet before INIT (the maps keep their posters).
3. billboard_image with a URL only: refused (asset paths only), no packet.
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
BILLBOARD, BILLBOARD_IMAGE = 33, 36
WORK = tempfile.mkdtemp(prefix="kn-billboard-e2e-")
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def start_server(name, port, lines, round_seconds=None):
    cfg = os.path.join(WORK, f"{name}.cfg")
    with open(cfg, "w") as f:
        f.write(f"port {port}\nmode ffa\nbots 0\n" + "".join(line + "\n" for line in lines))
    argv = [f"{BUILD}/server_bin", "--headless", "--no-cli", "--cfg", cfg, "--port", str(port)]
    if round_seconds:
        argv += ["--round-seconds", str(round_seconds)]
    log = open(os.path.join(WORK, f"{name}.log"), "w")
    p = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    for _ in range(50):
        try:
            kn_proto.Conn("127.0.0.1", port, timeout=0.5).close()
            return p
        except OSError:
            time.sleep(0.1)
    return p


def join(port, name):
    c = kn_proto.Conn("127.0.0.1", port)
    c.hello()
    c.join(name=name)
    return c


def before_init(conn, start=0):
    """The packet types between packets[start] and the next INIT, and that INIT's index."""
    types = []
    for i in range(start, len(conn.packets)):
        t, payload = conn.packets[i]
        if t == kn_proto.INIT:
            return types, i
        types.append((t, payload))
    return types, None


def text_of(types):
    for t, p in types:
        if t == BILLBOARD and len(p) == 80:
            return kn_proto.uncstr(p[:64]), kn_proto.uncstr(p[64:72]), kn_proto.uncstr(p[72:80])
    return None


def image_of(types):
    for t, p in types:
        if t == BILLBOARD_IMAGE and len(p) == 128:
            return kn_proto.uncstr(p)
    return None


def main():
    pa, pb, pc = kn_proto.free_ports(3)
    try:
        start_server("ad", pa, ['mapcycle Burg,Kanji', 'billboard "FRVR please hire me"', 'billboard_image "textures/pubs/b_3.png"'], round_seconds=4)
        time.sleep(1.0)  # the round is running: the player below joins late
        a = join(pa, "late")
        a.read(until=kn_proto.INIT, timeout=8)
        types, first = before_init(a)
        check("late joiner: INIT arrives", first is not None)
        check("late joiner: BILLBOARD before INIT", text_of(types) == ("FRVR please hire me", "#e3e3e3", "#000000"), str(text_of(types)))
        check("late joiner: BILLBOARD_IMAGE before INIT", image_of(types) == "textures/pubs/b_3.png", str(image_of(types)))

        # the next map: the round ends (4 s + the end screen), the server sends a new INIT
        second = None
        end = time.time() + 60
        while time.time() < end and second is None:
            a.read(until=kn_proto.INIT, timeout=5)
            if first is not None:
                types2, second = before_init(a, first + 1)
        check("map change: a second INIT", second is not None)
        if second is not None:
            maps = [struct.unpack("<i", a.packets[i][1][3:7])[0] if len(a.packets[i][1]) >= 7 else -1 for i in (first, second)]
            check("map change: BILLBOARD again before the new INIT", text_of(types2) is not None and text_of(types2)[0] == "FRVR please hire me")
            check("map change: BILLBOARD_IMAGE again", image_of(types2) == "textures/pubs/b_3.png")
            print(f"      INIT map indices {maps}")
        a.close()

        start_server("unset", pb, ['mapcycle Kanji', 'billboard ""'])
        b = join(pb, "unset")
        b.read(until=kn_proto.INIT, timeout=8)
        types, idx = before_init(b)
        check("unset: INIT arrives", idx is not None)
        check("unset: no billboard packets", not any(t in (BILLBOARD, BILLBOARD_IMAGE) for t, _ in types))
        b.close()

        start_server("url", pc, ['mapcycle Kanji', 'billboard_image "https://example.com/ad.png"'])
        c = join(pc, "url")
        c.read(until=kn_proto.INIT, timeout=8)
        types, idx = before_init(c)
        check("url image: INIT arrives", idx is not None)
        check("url image: refused, no billboard packets", not any(t in (BILLBOARD, BILLBOARD_IMAGE) for t, _ in types))
        c.close()
        with open(os.path.join(WORK, "url.log"), errors="replace") as f:
            check("url image: logged", "not an asset path" in f.read())
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
    ok = all(results)
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs in {WORK}")
    print(f"billboard_e2e: {sum(results)}/{len(results)} passed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
