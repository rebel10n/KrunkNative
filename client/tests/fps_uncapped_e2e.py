#!/usr/bin/env python3
"""Every launch path runs uncapped when V-Sync and the frame cap are off in the stored settings
(Makefile launch targets, client/src/main.c, gl_driver_relaunch.c, ui/ui_render_settings.c).

  python3 -I client/tests/fps_uncapped_e2e.py BUILD_DIR      (run from the repository root)

  1. make -n of each launch target (run, run-release, run-strike, launcher, launcher-release,
     smoke-native, smoke-ui, uidiff) passes no KRUNKNATIVE_VSYNC, KRUNKNATIVE_FRAME_CAP or
     __GL_SYNC_TO_VBLANK by default.
  2. no display and KRUNKNATIVE_HEADLESS unset: the client does not go headless (GLFW refuses to
     start without a display; a headless run would be capped at 120 fps).
  3. a windowed run (a private X server from xvfb-run -a, never the desktop's display), headless
     unset, the stored vsync false and updateRate 0 and no pacing in the environment: every
     "[render] fps" line says cap 0, vsync 0, swap 0. On NVIDIA the client relaunches once with
     __GL_SYNC_TO_VBLANK=0 (gl_driver_relaunch.c), and the relaunched process keeps it.
  4. --flavor strike reads its own store (krunknative-strike): its stored vsync false relaunches on
     NVIDIA. Before, the relaunch read the Box Office store ("no knSettings", V-Sync on).

Needs make, and xvfb-run for 3 and 4 (skipped without it). The NVIDIA relaunch checks run only
where /proc/driver/nvidia/version exists. Prints PASS/FAIL lines; exit code 0 only when all passed.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-fps-uncapped-", dir=CACHE if os.path.isdir(CACHE) else None)
NVIDIA = os.path.exists("/proc/driver/nvidia/version")
LAUNCH_TARGETS = ["run", "run-release", "run-strike", "launcher", "launcher-release", "smoke-native", "smoke-ui", "uidiff"]
PACING = ("KRUNKNATIVE_VSYNC", "KRUNKNATIVE_FRAME_CAP", "__GL_SYNC_TO_VBLANK")
FPS_LINE = re.compile(r"\[render\] fps \d+ \(cap (\d+)(?: headless)?, vsync (\d), swap (\d)(?:, display \d+ Hz)?\)")
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def clean_env(name, extra=None):
    """The environment of a player's launch: no display from the desktop, no pacing, no headless, no
    shell-level __GL_SYNC_TO_VBLANK (the relaunch decision is the thing under test)."""
    drop = ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_HEADLESS", "KRUNKNATIVE_VSYNC", "KRUNKNATIVE_FRAME_CAP",
            "__GL_SYNC_TO_VBLANK", "KRUNKNATIVE_BRAIN_URL", "KN_FLAVOR", "KRUNKNATIVE_EGL_DEVICE")
    env = {k: v for k, v in os.environ.items() if k not in drop}
    env.update({"XDG_CONFIG_HOME": os.path.join(WORK, name, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, name, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_FPS_LOG": "1", "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    env.update(extra or {})
    return env


def run_client(name, args, stores=None, windowed=True, extra=None, timeout=120):
    base = os.path.join(WORK, name)
    cfg = os.path.join(base, "cfg")
    os.makedirs(base, exist_ok=True)
    for sub, text in (stores or {}).items():
        path = os.path.join(cfg, sub, "knSettings.json")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    script = os.path.join(base, "input.script")
    with open(script, "w") as f:
        f.write("0 play\n4000 quit\n")
    env = clean_env(name, {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667", **(extra or {})})
    cmd = [f"{BUILD}/client_bin", *args, "assets/maps/kanji.json"]
    if windowed:
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24"] + cmd
    log = os.path.join(base, "client.log")
    os.makedirs(base, exist_ok=True)
    with open(log, "w") as out:
        try:
            code = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  env=env, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            code = None
    with open(log, errors="replace") as f:
        return code, f.read(), log


def main():
    # 1. the launch targets pass no pacing by default (make -n; MAKE=: keeps sub-makes from running)
    env = {k: v for k, v in os.environ.items() if k not in ("VSYNC", "FRAME_CAP")}
    for target in LAUNCH_TARGETS:
        p = subprocess.run(["make", "-n", "MAKE=:", "BACKEND=0", target], capture_output=True, text=True, env=env, timeout=300)
        out = p.stdout + p.stderr
        bad = [line.strip() for line in out.splitlines() if any(k in line for k in PACING)]
        check(f"make -n {target}: no pacing variable by default",
              p.returncode == 0 and ("client_bin" in out or "krunknative" in out) and not bad, "; ".join(bad[:2]) or f"rc {p.returncode}")

    # 2. no display, headless unset: no window and no silent headless run
    code, text, _ = run_client("no-display", ["--offline"], windowed=False, timeout=60)
    check("no display, KRUNKNATIVE_HEADLESS unset: the client does not start headless",
          code not in (0, None) and "[render] fps" not in text and "[headless]" not in text, f"exit {code}")

    if not shutil.which("xvfb-run"):
        print("SKIP  windowed checks: xvfb-run is not installed", flush=True)
    else:
        # 3. windowed, stored V-Sync off and Frame Cap 0: uncapped in every fps line, and on NVIDIA relaunched once
        code, text, log = run_client("stored-off", ["--offline"], stores={"krunknative": '{"vsync": false, "updateRate": 0}'})
        lines = FPS_LINE.findall(text)
        check("windowed, stored vsync false and updateRate 0: the fps lines say cap 0, vsync 0, swap 0",
              lines and all(c == "0" and v == "0" and s == "0" for c, v, s in lines) and "headless" not in text,
              f"{lines[:2]} log {log}")
        if NVIDIA:
            check("NVIDIA: the relaunch sets __GL_SYNC_TO_VBLANK=0 for the stored V-Sync off",
                  "[gl] V-Sync relaunch: yes" in text and "stored: vsync false" in text and "set to 0" in text, log)
        else:
            print("SKIP  relaunch check: no NVIDIA driver (/proc/driver/nvidia/version)", flush=True)

        # 4. --flavor strike: its own store decides the relaunch (krunknative-strike, not krunknative)
        code, text, log = run_client("strike-store", ["--flavor", "strike", "--offline"],
                                     stores={"krunknative-strike": '{"vsync": false, "updateRate": 0}'})
        lines = FPS_LINE.findall(text)
        check("--flavor strike: the fps lines say cap 0, vsync 0, swap 0",
              lines and all(c == "0" and v == "0" and s == "0" for c, v, s in lines), f"{lines[:2]} log {log}")
        if NVIDIA:
            check("--flavor strike: the relaunch reads the Strike store (stored: vsync false, flavor strike)",
                  "[gl] V-Sync relaunch: yes" in text and "stored: vsync false" in text and "store (strike)" in text, log)
        else:
            print("SKIP  Strike relaunch check: no NVIDIA driver", flush=True)

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
