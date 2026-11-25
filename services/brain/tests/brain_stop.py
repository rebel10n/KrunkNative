"""Stopping a kn_brain the tests started, with a diagnosis when it does not exit in time.

  import brain_stop                      (the test puts its own directory on sys.path: they run with -I)
  ok = brain_stop.stop(proc, log_path)   SIGTERM, then wait up to `timeout` s (default 5)

On a timeout it prints the brain log's last lines and, per thread, its name, state, wchan and (when /proc lets
this user read it) its syscall line, then SIGKILLs the process and returns False. A slow exit on a loaded CI host
(CT 208, 2026-11) once showed only as a TimeoutExpired from proc.wait(5) with every check passed; this output says
which thread was still blocked and where.
"""
import glob
import os
import signal
import subprocess
import time

LOG_LINES = 40


def _read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read().strip()
    except OSError as e:
        return "(%s)" % e.strerror


def threads(pid):
    """[(tid, comm, state, wchan, syscall)] of a live process, from /proc/<pid>/task/*"""
    rows = []
    for task in sorted(glob.glob("/proc/%d/task/*" % pid), key=lambda p: int(os.path.basename(p))):
        stat = _read(os.path.join(task, "stat"))
        # the state is the field after the parenthesised comm, which may itself hold spaces or ")"
        state = stat[stat.rfind(")") + 2:].split(" ", 1)[0] if ")" in stat else "?"
        rows.append((os.path.basename(task), _read(os.path.join(task, "comm")), state,
                     _read(os.path.join(task, "wchan")) or "0", _read(os.path.join(task, "syscall"))))
    return rows


def diagnose(proc, log_path=None, name="kn_brain", waited=0.0):
    print("%s (pid %d) still running %.1f s after SIGTERM" % (name, proc.pid, waited), flush=True)
    if log_path:
        try:
            with open(log_path, errors="replace") as f:
                tail = f.read().splitlines()[-LOG_LINES:]
        except OSError as e:
            tail = ["(log %s: %s)" % (log_path, e.strerror)]
        print("---- last %d lines of %s" % (len(tail), log_path))
        for line in tail:
            print("  " + line)
    print("---- threads: tid comm state wchan syscall")
    for tid, comm, state, wchan, syscall in threads(proc.pid):
        print("  %s %s %s %s %s" % (tid, comm, state, wchan, syscall))
    print("----", flush=True)


def stop(proc, log_path=None, timeout=5.0, name="kn_brain"):
    """SIGTERM and wait; True when the process exited within `timeout` s. Otherwise print the diagnosis, SIGKILL it
    (and reap it) and return False. A process that already exited is True."""
    if proc is None:
        return True
    if proc.poll() is not None:
        return True
    t0 = time.monotonic()
    try:
        proc.send_signal(signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout)
        return True
    except subprocess.TimeoutExpired:
        diagnose(proc, log_path, name, time.monotonic() - t0)
        proc.kill()
        proc.wait()
        return False
