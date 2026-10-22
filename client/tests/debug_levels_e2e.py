#!/usr/bin/env python3
"""The client's debug output levels (shared/include/klog.h): level 0 is the default, --debug is 1, --debug=2 is 2.

  python3 -I client/tests/debug_levels_e2e.py BUILD_DIR      (run from the repository root)

Checks, with headless client_bin runs (offline, private XDG_CONFIG_HOME):
  1. the default run (no flag) prints the level-0 lines a player needs ([startup] platform, [client] Krunker v)
     and no level-2 line (the per-frame, per-click and per-packet tags listed in VERBOSE_TAGS);
  2. --debug=2 prints at least one more line than the default run, and it shows a verbose tag;
  3. a bad level is refused on stderr with exit 2 (--debug=7, -d x), and KRUNKNATIVE_DEBUG=2 behaves like --debug=2.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-debug-levels-", dir=CACHE if os.path.isdir(CACHE) else None)
CLIENT = os.path.join(BUILD, "client_bin")
RUN_SECONDS = 8
# Tags whose lines are all level 2 (klog.h): none may appear in a default run.
VERBOSE_TAGS = ("ui", "controls", "render", "audio", "gamepad", "preview", "nav", "sprays", "tooltip", "udp",
                "viewer", "vfx", "voice", "http", "remote-image", "emoji", "scenario", "hud_ui", "skin_fx",
                "recon", "assets", "notify")
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def env_for(tag, extra=None):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_DEBUG")}
    env["KRUNKNATIVE_HEADLESS"] = "1"
    env["XDG_CONFIG_HOME"] = os.path.join(WORK, tag)
    os.makedirs(env["XDG_CONFIG_HOME"], exist_ok=True)
    env.update(extra or {})
    return env


def run_client(tag, args, seconds=RUN_SECONDS, extra_env=None):
    """Runs client_bin --offline plus args for a few seconds; returns (exit code or None, stdout lines, stderr)."""
    out_path = os.path.join(WORK, tag + ".out")
    err_path = os.path.join(WORK, tag + ".err")
    with open(out_path, "w") as out, open(err_path, "w") as err:
        # stdbuf: line-buffered stdout, so the lines written before the kill are in the file
        proc = subprocess.Popen(["stdbuf", "-oL", CLIENT, "--offline"] + args, stdout=out, stderr=err,
                                env=env_for(tag, extra_env), cwd=os.getcwd())
        try:
            time.sleep(seconds)
            code = proc.poll()
            if code is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    code = proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    code = proc.wait()
        finally:
            if proc.poll() is None:
                proc.kill()
    with open(out_path, errors="replace") as f:
        lines = f.read().splitlines()
    with open(err_path, errors="replace") as f:
        stderr = f.read()
    return code, lines, stderr


def has_tag(lines, tag):
    return any(line.startswith("[" + tag + "] ") for line in lines)


def main():
    if not os.path.exists(CLIENT):
        print(f"FAIL  {CLIENT} is missing: run make build first")
        return 1

    # 1. the default run: level-0 lines present, no level-2 tag
    _, default, _ = run_client("default", [])
    check("default: the level-0 startup platform line is printed", any(l.startswith("[startup] platform ") for l in default))
    check("default: the level-0 client version line is printed", any(l.startswith("[client] Krunker v") for l in default))
    leaked = [t for t in VERBOSE_TAGS if has_tag(default, t)]
    check("default: no level-2 tag is printed", not leaked, ", ".join(leaked))

    # 2. --debug=2 adds lines, and shows a verbose tag
    _, verbose, _ = run_client("verbose", ["--debug=2"])
    check("--debug=2: more lines than the default run", len(verbose) > len(default),
          f"default {len(default)} lines, --debug=2 {len(verbose)} lines")
    check("--debug=2: a level-2 tag is printed", any(has_tag(verbose, t) for t in VERBOSE_TAGS))

    # 3. a bad level is refused; the environment variable is read
    code, _, err = run_client("bad", ["--debug=7"], seconds=3)
    check("--debug=7: refused on stderr with exit 2", code == 2 and "--debug" in err, f"exit {code}")
    code, _, err = run_client("bad-d", ["-d", "x"], seconds=3)
    check("-d x: refused on stderr with exit 2", code == 2 and "-d" in err, f"exit {code}")
    _, env_verbose, _ = run_client("env", [], extra_env={"KRUNKNATIVE_DEBUG": "2"})
    check("KRUNKNATIVE_DEBUG=2 behaves like --debug=2", len(env_verbose) > len(default),
          f"{len(env_verbose)} lines")

    shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"{len(results) - failed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
