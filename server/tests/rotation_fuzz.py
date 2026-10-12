#!/usr/bin/env python3
"""Look-rotation regression + fuzz test for server_bin (the "spinbot / server crash" report).

Starts a standalone guest server_bin, spawns two raw protocol clients and:
  1. turns client A right for a long time the way a real client accumulates yaw (an
     unbounded, ever-decreasing CONTROLS yaw) and checks that client B only ever sees A's yaw
     inside [-2pi, 2pi] and finite, with no per-state jumps that would make the remote
     interpolation spin;
  2. fuzzes INPUT / INPUT_BATCH frames with huge, NaN, inf, denormal and out-of-range yaw,
     pitch, delta, move_dir and seq values (shooting and jumping too), from both clients;
  3. checks the server is still alive and still answering after all of it, and that every
     STATE it sent had finite positions and directions.

  server/tests/rotation_fuzz.py [BUILD_DIR]   (default cmake-build-debug; run from the repo root)

Build server_bin with -fsanitize=address,undefined -fno-sanitize-recover=all to turn any UB
into a failure (the server log is printed when the process dies). Env: KN_FUZZ_PORT,
KN_FUZZ_SEED, KN_FUZZ_FRAMES (random frames per client, default 4000).
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORT = int(os.environ.get("KN_FUZZ_PORT", "23417"))
SEED = int(os.environ.get("KN_FUZZ_SEED", "1337"))
FRAMES = int(os.environ.get("KN_FUZZ_FRAMES", "4000"))

INIT, ENT, SPAWN, INPUT, STATE, INPUT_BATCH = 2, 3, 4, 5, 6, 111
INPUT_FMT = "<iifffBBBb"          # NetInputPacket (24 bytes)
STATE_FMT = "<iiBi3f3f2ffff"      # NetStatePacket (57 bytes)
SHOOT, SCOPE, JUMP, CROUCH, RELOAD = 1, 2, 4, 8, 16
PI2 = math.pi * 2.0

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print(f"[rotation_fuzz] FAIL: {msg}")


def f32(v):
    """Python float -> the float32 bits struct would write (NaN/inf pass through)."""
    return struct.unpack("<f", struct.pack("<f", v))[0] if math.isfinite(v) and abs(v) < 3.4e38 else v


def input_frame(seq, x_dir, y_dir, delta=1 / 60, move_dir=-1, flags=0, swap=0, slowdown=0, scroll=0):
    return struct.pack(INPUT_FMT, seq, move_dir, f32(delta), f32(x_dir), f32(y_dir), flags, swap, slowdown, scroll)


def raw_input_frame(seq, raw_x, raw_y, raw_delta, move_dir=-1, flags=0):
    """Bit patterns straight onto the wire (NaN payloads, denormals, -0)."""
    return struct.pack("<ii", seq, move_dir) + raw_delta + raw_x + raw_y + bytes([flags, 0, 0, 0])


def states_of(conn, uid):
    out = []
    for t, payload in conn.packets:
        if t == STATE and len(payload) == struct.calcsize(STATE_FMT):
            v = struct.unpack(STATE_FMT, payload)
            if v[0] == uid:
                out.append(v)
    return out


def connect(name):
    c = kn_proto.Conn("127.0.0.1", PORT)
    c.hello()
    check(c.read(until=INIT, timeout=5) is not None, f"{name}: no INIT")
    c.ent()
    for _ in range(50):
        p = c.read(until=SPAWN, timeout=5)
        if p is None:
            break
        info = kn_proto.spawn_info(p)
        if info and info["is_you"]:
            return c, info["uid"]
    check(False, f"{name}: no SPAWN of our own player")
    return c, -1


def alive(proc):
    return proc.poll() is None


def main():
    work = tempfile.mkdtemp(prefix="kn-rotfuzz-")
    with open(os.path.join(work, "none.json"), "w") as f:
        f.write("{}")
    log_path = os.path.join(work, "server.log")
    log = open(log_path, "w")
    env = dict(os.environ, ASAN_OPTIONS="abort_on_error=1:detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--no-cli", "--cfg", "none", "--config", os.path.join(work, "none.json"),
                             "--offline-mode", "--port", str(PORT), "--mode", "ffa", "assets/maps/burg.json"],
                            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env)
    rng = random.Random(SEED)
    try:
        for _ in range(100):
            if open(log_path).read().count("Listening on port"):
                break
            time.sleep(0.1)
        a, uid_a = connect("A")
        b, uid_b = connect("B")
        if failures:
            return

        # 1. A real client turning right: CONTROLS yaw keeps decreasing, never wrapped by the
        #    sender (the bug), 0.05 rad per 60 Hz frame, batched like the client's "q".
        seq_a, yaw = 1, 0.0
        for chunk in range(300):
            frames = []
            for _ in range(20):
                yaw -= 0.05 + rng.random() * 0.2
                frames.append(input_frame(seq_a, 0.1, yaw))
                seq_a += 1
            a.send(INPUT_BATCH, bytes([len(frames)]) + b"".join(frames))
            if chunk % 10 == 0:
                b.read(timeout=0.02)
                a.read(timeout=0.01)
        b.read(timeout=1.0)
        a.read(timeout=0.2)
        print(f"[rotation_fuzz] turned A right to yaw {yaw:.1f} rad ({seq_a - 1} frames)")
        seen = states_of(b, uid_a)
        check(len(seen) > 10, f"B saw only {len(seen)} states of A")
        last = None
        for v in seen:
            pitch, d_yaw = v[10], v[11]
            check(math.isfinite(d_yaw) and math.isfinite(pitch), f"non-finite direction {pitch} {d_yaw}")
            check(-PI2 - 1e-3 <= d_yaw <= PI2 + 1e-3, f"remote yaw {d_yaw} outside [-2pi, 2pi]")
            check(-math.pi / 2 - 1e-3 <= pitch <= math.pi / 2 + 1e-3, f"remote pitch {pitch} outside [-pi/2, pi/2]")
            last = d_yaw
        if seen:
            want = math.fmod(f32(yaw), PI2)
            diff = abs(math.remainder(last - want, PI2))
            check(diff < 0.01, f"A's final yaw {last} does not match the sent heading {want} (diff {diff})")
        check(alive(proc), "server died while A turned right")

        # 2. Fuzz: special values for yaw / pitch / delta, then random garbage.
        specials = [0.0, -0.0, 1e3, -1e3, 1e6, -1e6, 1e9, -1e9, 1.5e19, -1.5e19, 3.0e38, -3.0e38, 3.4028235e38,
                    -3.4028235e38, float("inf"), float("-inf"), float("nan"), 1e-45, -1e-45, 1e-38, math.pi * 1e7, 2.1e9, -2.2e9]
        raw_bits = [b"\x00\x00\xc0\x7f", b"\x01\x00\x80\x7f", b"\xff\xff\xff\xff", b"\x00\x00\x80\x7f", b"\x00\x00\x80\xff",
                    b"\x01\x00\x00\x00", b"\xff\xff\x7f\x7f", b"\xff\xff\x7f\xff", b"\x00\x00\x00\x80"]
        seq_b = 1
        for i, x in enumerate(specials):
            for y in specials:
                for flags in (0, SHOOT | SCOPE, JUMP | CROUCH | SHOOT):
                    a.send(INPUT, input_frame(seq_a, x, y, flags=flags, move_dir=i % 9 - 1))
                    seq_a += 1
            a.read(timeout=0.01)
            b.read(timeout=0.01)
        for rx in raw_bits:
            for ry in raw_bits:
                for rd in raw_bits:
                    b.send(INPUT, raw_input_frame(seq_b, rx, ry, rd, flags=SHOOT))
                    seq_b += 1
        for delta in specials:
            b.send(INPUT, input_frame(seq_b, 0.0, 1e30, delta=delta, flags=SHOOT | JUMP))
            seq_b += 1
        a.read(timeout=0.3)
        b.read(timeout=0.3)
        check(alive(proc), "server died on special rotation values")

        for _ in range(FRAMES // 20):
            for conn, uid in ((a, uid_a), (b, uid_b)):
                frames = []
                for _ in range(20):
                    pick = rng.random()
                    if pick < 0.4:
                        x, y = rng.choice(specials), rng.choice(specials)
                    elif pick < 0.8:
                        x, y = rng.uniform(-1e12, 1e12), rng.uniform(-1e12, 1e12)
                    else:
                        x, y = rng.uniform(-10, 10), rng.uniform(-1e5, 1e5)
                    seq = rng.choice([rng.randint(-2**31, 2**31 - 1), (seq_a if conn is a else seq_b)])
                    frames.append(input_frame(seq, x, y, delta=rng.choice(specials + [1 / 60, 0.5]),
                                              move_dir=rng.randint(-50, 50), flags=rng.randint(0, 255),
                                              swap=rng.randint(0, 255), slowdown=rng.randint(0, 255),
                                              scroll=rng.randint(-128, 127)))
                    if conn is a:
                        seq_a += 1
                    else:
                        seq_b += 1
                conn.send(INPUT_BATCH, bytes([len(frames)]) + b"".join(frames))
            a.read(timeout=0.005)
            b.read(timeout=0.005)
            if not alive(proc):
                break
            if rng.random() < 0.02:  # respawn now and then (a death from the fuzzed shots, a new life)
                (a if rng.random() < 0.5 else b).ent()
        a.read(timeout=0.5)
        b.read(timeout=0.5)
        check(alive(proc), "server died while fuzzing rotation input")

        # 3. Every STATE either client got stayed sane; the server still accepts a new player.
        for conn, who in ((a, "A"), (b, "B")):
            for t, payload in conn.packets:
                if t == STATE and len(payload) == struct.calcsize(STATE_FMT):
                    v = struct.unpack(STATE_FMT, payload)
                    if not v[2]:
                        continue
                    bad = [n for n in v[4:12] if not math.isfinite(n)]
                    check(not bad, f"{who} got a STATE of uid {v[0]} with non-finite pos/vel/dir {v[4:12]}")
                    check(-PI2 - 1e-3 <= v[11] <= PI2 + 1e-3, f"{who} got yaw {v[11]} of uid {v[0]}")
                    if bad:
                        break
        c, uid_c = connect("C")
        check(uid_c >= 0, "server stopped spawning new players after the fuzz")
        c.close()
        a.close()
        b.close()
        time.sleep(0.3)
        check(alive(proc), "server died after the fuzz clients left")
    finally:
        if alive(proc):
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
        text = open(log_path, errors="replace").read()
        if "runtime error" in text or "AddressSanitizer" in text or proc.returncode not in (0, -15, None):
            failures.append(f"server exited {proc.returncode} / sanitizer report")
        if failures:
            print(text[-6000:])


if __name__ == "__main__":
    main()
    if failures:
        print(f"[rotation_fuzz] {len(failures)} failure(s)")
        sys.exit(1)
    print("[rotation_fuzz] OK")
