#!/usr/bin/env python3
"""kn_brain exits promptly on SIGTERM while a worker thread is in a network request (services/brain/main.c on_signal,
modules_shutdown; releases.c's poll thread).

  shutdown_test.py <kn_brain> [repo root]

The release poll (releases.c) starts about 2 s after startup and runs on a thread that releases_shutdown joins. Its
URL here is a local socket that accepts the connection and never answers, as a stalled GitHub API on a loaded CI host
does. The test waits until the poll's connection is accepted (the thread is then inside curl_easy_perform), sends
SIGTERM and times the exit. Before the poll had a cancel flag the join waited out curl's 10 s timeout (8.9 s measured
on devbox, 2026-11-25); with it the brain exits in about 1 s. No database (--no-db).

Exit 0 when every check passes, 1 otherwise, 77 when kn_brain is missing.
"""
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import brain_stop  # noqa: E402

BRAIN = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug/services/brain/kn_brain"
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
EXIT_LIMIT_S = 4.0  # the fixed exit is about 1 s (curl's idle progress call); the unfixed one is 8 to 10 s
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(bool(ok))
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not detail else "  -- " + detail), flush=True)


def wait_listening(log_path, proc, secs=20):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if proc.poll() is not None:
            return None
        with open(log_path, errors="replace") as f:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", f.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    return None


def main():
    if not os.access(BRAIN, os.X_OK):
        print("shutdown test: no kn_brain at %s: skipped" % BRAIN)
        sys.exit(77)
    hole = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    hole.bind(("127.0.0.1", 0))
    hole.listen(4)
    work = tempfile.mkdtemp(prefix="kn-shutdown.")
    log_path = os.path.join(work, "brain.log")
    proc = None
    held = []
    try:
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--no-db", "--brain.host=127.0.0.1", "--brain.port=0",
                                     "--brain.id=shutdown-%d" % os.getpid(), "--debug",
                                     "--servers.releases_url=http://127.0.0.1:%d/releases" % hole.getsockname()[1],
                                     "--servers.latest_version=", "--servers.latest_poll_s=60"],
                                    cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        check("kn_brain starts listening", wait_listening(log_path, proc) is not None, log_path)
        # the poll's connection: the thread is now blocked in the request (the hole never answers)
        ready, _, _ = select.select([hole], [], [], 15)
        if ready:
            conn, _ = hole.accept()
            held.append(conn)
        check("the release poll connects to the stalled endpoint", bool(ready), "no connection within 15 s")
        t0 = time.monotonic()
        exited = brain_stop.stop(proc, log_path, timeout=EXIT_LIMIT_S)
        took = time.monotonic() - t0
        check("SIGTERM with the poll in flight: kn_brain exits within %.0f s" % EXIT_LIMIT_S, exited, "%.1f s" % took)
        print("exit took %.2f s" % took)
        check("the exit is clean (status 0)", exited and proc.returncode == 0, "status %r" % proc.returncode)
        with open(log_path, errors="replace") as f:
            text = f.read()
        check("the brain logged its last line", "bye" in text, text[-400:])
        proc = None
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        for c in held:
            c.close()
        hole.close()
    ok = all(RESULTS)
    print("shutdown test: " + ("OK" if ok else "%d failure(s)" % RESULTS.count(False)))
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("log kept in " + log_path)
    sys.exit(0 if ok else 1)


main()
