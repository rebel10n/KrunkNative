#!/usr/bin/env python3
"""Crash capture end to end (client/src/crash_report.c): a crash report, the next start's notice, a freeze dump.

  python3 -I client/tests/crash_capture_e2e.py BUILD_DIR      (run from the repository root)

Four headless client_bin runs (offline Kanji, fixed 60 fps step, private XDG_CONFIG_HOME):
  1. KRUNKNATIVE_TEST_CRASH_AFTER_FRAMES=60: the client dereferences NULL on frame 60. It must
     die of SIGSEGV, and <config>/krunknative/crash/ must hold crash-*.txt with the build line,
     the signal, the console log part and a frame list ("module(+0x...) [0x...]"), latest.txt
     naming it.
  2. The next start (no crash) names that report: "[crash] A crash report was saved to <path>",
     once. seen.txt holds the path.
  3. The start after that (still no crash) names nothing.
  4. KRUNKNATIVE_TEST_STALL_AFTER_FRAMES=120: the main loop sleeps 6 s on frame 120. The watchdog
     (5 s) must write freeze-*.txt with the main thread's stack, which is inside the stall (the
     frames include the sleep), and the client must keep running: it quits by its input script and
     exits 0.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CLIENT = os.path.join(BUILD, "client_bin")
MAP = "assets/maps/official/10_kanji.json"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-crash-capture-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

FRAME = re.compile(r"\[0x[0-9a-f]+\]")
FRAME_OFFSET = re.compile(r"\(\+0x[0-9a-f]+\)")
NOTICE = "[crash] A crash report was saved to "


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def client_env(config):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": config,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    return env


def run(name, config, extra, script, timeout):
    """One client run; returns (returncode or None when it had to be killed, log text)."""
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    env = client_env(config)
    env.update(extra)
    env["KRUNKNATIVE_INPUT_SCRIPT"] = script_path
    log_path = os.path.join(WORK, f"{name}.log")
    with open(log_path, "w") as out:
        p = subprocess.Popen([CLIENT, "--offline", MAP], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = None
    with open(log_path, errors="replace") as f:
        return rc, f.read()


def reports(config, prefix):
    folder = os.path.join(config, "krunknative", "crash")
    if not os.path.isdir(folder):
        return []
    return sorted(os.path.join(folder, n) for n in os.listdir(folder) if n.startswith(prefix) and n.endswith(".txt"))


def section(text, header):
    """The lines after `header` up to the next blank line that starts a new section."""
    lines = text.splitlines()
    out, on = [], False
    for line in lines:
        if line.startswith(header):
            on = True
            continue
        if on:
            if line.strip() == "" and out:
                break
            out.append(line)
    return out


def test_crash():
    config = os.path.join(WORK, "cfg-crash")
    rc, log = run("crash", config, {"KRUNKNATIVE_TEST_CRASH_AFTER_FRAMES": "60"}, "60000 quit\n", 240)
    check("crashing run dies of SIGSEGV", rc == -signal.SIGSEGV, f"returncode {rc}")
    files = reports(config, "crash-")
    check("crash report file appears", len(files) == 1, f"{len(files)} crash-*.txt")
    if not files:
        return None
    with open(files[0], errors="replace") as f:
        text = f.read()
    check("report names the build and the signal", "build: " in text and "signal: SIGSEGV (11)" in text)
    frames = [line for line in section(text, "stack of the thread that crashed") if FRAME.search(line)]
    check("report holds a frame list", len(frames) >= 3, f"{len(frames)} frames")
    check("frames carry a module offset", any(FRAME_OFFSET.search(line) for line in frames))
    check("report has the console log part", "log (the last lines of the in-game console" in text)
    latest = os.path.join(config, "krunknative", "crash", "latest.txt")
    with open(latest) as f:
        check("latest.txt names the report", f.read().strip() == files[0])
    return config


def test_notice(config):
    rc, log = run("notice", config, {}, "2000 quit\n", 240)
    check("next start runs to its quit", rc == 0, f"returncode {rc}")
    notices = [line for line in log.splitlines() if line.startswith(NOTICE)]
    check("next start names the report once", len(notices) == 1, f"{len(notices)} notices")
    latest = os.path.join(config, "krunknative", "crash", "latest.txt")
    with open(latest) as f:
        named = f.read().strip()
    check("the notice names latest.txt's report", bool(notices) and notices[0] == NOTICE + named)
    seen = os.path.join(config, "krunknative", "crash", "seen.txt")
    with open(seen) as f:
        check("seen.txt records the shown report", f.read().strip() == named)

    rc, log = run("quiet", config, {}, "2000 quit\n", 240)
    check("start after the notice runs to its quit", rc == 0, f"returncode {rc}")
    check("the report is not named again", NOTICE not in log)


def test_freeze():
    config = os.path.join(WORK, "cfg-freeze")
    rc, log = run("freeze", config, {"KRUNKNATIVE_TEST_STALL_AFTER_FRAMES": "120"}, "9000 quit\n", 300)
    check("the stall run shows its stall", "KRUNKNATIVE_TEST_STALL_AFTER_FRAMES" in log)
    check("the client keeps running after the freeze and exits 0", rc == 0, f"returncode {rc}")
    files = reports(config, "freeze-")
    check("freeze report file appears", len(files) == 1, f"{len(files)} freeze-*.txt")
    if not files:
        return
    with open(files[0], errors="replace") as f:
        text = f.read()
    check("freeze report is complete", "KrunkNative freeze report" in text and "end of the freeze report" in text)
    stacks = section(text, "stacks (the main thread first):")
    main_at = [i for i, line in enumerate(stacks) if "(main thread)" in line]
    check("the main thread's stack is in the report", len(main_at) == 1)
    if main_at:
        main = []
        for line in stacks[main_at[0] + 1:]:
            if line.startswith("--- thread"):
                break
            if FRAME.search(line):
                main.append(line)
        check("the main thread's stack has frames", len(main) >= 2, f"{len(main)} frames")
        check("the main thread's frames carry module offsets", any(FRAME_OFFSET.search(line) for line in main))
    if sys.platform.startswith("linux"):
        threads = [line for line in stacks if line.startswith("--- thread")]
        check("every thread of the process is listed", len(threads) >= 2, f"{len(threads)} threads")


def main():
    if not os.path.isfile(CLIENT):
        print(f"FAIL  {CLIENT} is not built", flush=True)
        return 1
    try:
        config = test_crash()
        if config:
            test_notice(config)
        else:
            check("next start names the report", False, "no crash report to name")
        test_freeze()
    finally:
        if all(results):
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}", flush=True)
    passed = sum(results)
    print(f"{passed}/{len(results)} passed", flush=True)
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
