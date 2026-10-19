#!/usr/bin/env python3
"""Client defaults, the Beautiful preset and a clean exit on signals, end to end
(client/src/ui/ui_settings.c, ui_render_settings.c, ui_client_settings.c, client/src/main.c).

  python3 -I client/tests/settings_perf_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin runs (offline Kanji, private XDG_CONFIG_HOME):
  1. a fresh store paces with vsync on (the native default); KRUNKNATIVE_VSYNC=0 turns it off.
  2. KRUNKNATIVE_FRAME_CAP beats a saved updateRate for the whole run (before, the saved value
     replaced it as soon as the page pushed the stored settings).
  3. selectSettingPre(4) (Beautiful) stores resolution 1, not 2.
  4. SIGINT and SIGTERM end the client through its normal shutdown: exit code 0, a
     "[client] SIG...: shutting down" line, no crash.
  5. a windowed run (xvfb-run, a private X server; needs xvfb-run) with vsync off and no frame cap
     is uncapped: its fps lines show cap 0, and KRUNKNATIVE_HEADLESS=false does not make it headless.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-settings-perf-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def cfg_dir(name):
    return os.path.join(WORK, name, "cfg")


def store_path(name, key):
    return os.path.join(cfg_dir(name), "krunknative", f"{key}.json")


def write_store(name, key, value):
    path = store_path(name, key)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(value)


def read_store(name, key):
    try:
        with open(store_path(name, key)) as f:
            return f.read().strip()
    except OSError:
        return None


def env_for(name, script=None, extra=None):
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_VSYNC", "KRUNKNATIVE_FRAME_CAP")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg_dir(name),
                "XDG_STATE_HOME": os.path.join(WORK, name, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_FPS_LOG": "1",
                "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    if script is not None:
        path = os.path.join(WORK, name, "input.script")
        with open(path, "w") as f:
            f.write(script)
        env["KRUNKNATIVE_INPUT_SCRIPT"] = path
        env["KRUNKNATIVE_FIXED_DELTA"] = "16.667"
    env.update(extra or {})
    return env


def start(name, script=None, extra=None, windowed=False):
    os.makedirs(cfg_dir(name), exist_ok=True)
    log = os.path.join(WORK, name, "client.log")
    out = open(log, "w")
    # windowed: a window on a private Xvfb (xvfb-run -a), never the desktop's display
    cmd = [f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"]
    if windowed:
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24"] + cmd
    p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         env=env_for(name, script, extra), start_new_session=True)
    return p, out, log


def finish(p, out, log, timeout):
    try:
        code = p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        code = None
    out.close()
    with open(log, errors="replace") as f:
        return code, f.read()


def run(name, script, extra=None, timeout=180, windowed=False):
    p, out, log = start(name, script, extra, windowed)
    return finish(p, out, log, timeout)


def fps_lines(text):
    return re.findall(r"\[render\] fps \d+ \(cap (\d+)(?: headless)?, vsync (\d), swap \d(?:, display \d+ Hz)?\)", text)


def wait_for(log, pattern, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(log, errors="replace") as f:
                if re.search(pattern, f.read()):
                    return True
        except OSError:
            pass
        time.sleep(0.25)
    return False


def main():
    # 1. vsync: on by default, KRUNKNATIVE_VSYNC=0 still turns it off
    code, text = run("vsync-default", "0 play\n2500 quit\n")
    lines = fps_lines(text)
    check("fresh store: the client paces with vsync on", lines and all(v == "1" for _, v in lines), str(lines[:3]))
    code, text = run("vsync-off", "0 play\n2500 quit\n", {"KRUNKNATIVE_VSYNC": "0"})
    lines = fps_lines(text)
    check("KRUNKNATIVE_VSYNC=0 turns vsync off", lines and all(v == "0" for _, v in lines), str(lines[:3]))

    # 2. KRUNKNATIVE_FRAME_CAP beats a saved updateRate
    write_store("cap-override", "knSettings", json.dumps({"updateRate": 30}))
    code, text = run("cap-override", "0 play\n3000 quit\n", {"KRUNKNATIVE_FRAME_CAP": "0"})
    lines = fps_lines(text)
    check("KRUNKNATIVE_FRAME_CAP=0 wins over a saved updateRate 30", lines and all(c == "0" for c, _ in lines), str(lines[:3]))
    try:
        saved = json.loads(read_store("cap-override", "knSettings") or "{}")
    except ValueError:
        saved = {}
    check("the saved updateRate stays in the store", saved.get("updateRate") == 30, str(saved.get("updateRate")))

    # 3. Beautiful preset: resolution 1
    code, text = run("beautiful", "0 menu\n800 call selectSettingPre 4\n1800 quit\n")
    try:
        stored = json.loads(read_store("beautiful", "knSettings") or "{}")
    except ValueError:
        stored = {}
    check("Beautiful is the selected preset", read_store("beautiful", "preIndex") in ("4", '"4"'), str(read_store("beautiful", "preIndex")))
    check("Beautiful stores resolution 1", stored.get("resolution") == 1, f"resolution={stored.get('resolution')!r}")
    check("Beautiful still stores its quality keys (antiAlias, lighting 3)",
          stored.get("antiAlias") is True and stored.get("lighting") == 3, f"antiAlias={stored.get('antiAlias')!r} lighting={stored.get('lighting')!r}")
    check("the client applied resolution=1", re.search(r"\[ui\] setting resolution=1\b", text), "no [ui] setting resolution=1")

    # 4. SIGINT / SIGTERM: the normal shutdown, exit 0
    for sig in (signal.SIGINT, signal.SIGTERM):
        name = f"signal-{sig.name}"
        p, out, log = start(name)
        up = wait_for(log, r"\[render\] fps \d+", 120)
        check(f"{sig.name}: the client is running", up)
        os.kill(p.pid, sig)
        t0 = time.time()
        code, text = finish(p, out, log, 30)
        took = time.time() - t0
        check(f"{sig.name}: exit code 0", code == 0, f"code {code}, {took:.1f}s, log {log}")
        check(f"{sig.name}: the shutdown line is logged", f"[client] {sig.name}: shutting down" in text)
        check(f"{sig.name}: no crash in the shutdown", not re.search(r"Segmentation|AddressSanitizer|Aborted|core dumped", text))

    # 5. A windowed run with vsync off and no frame cap is uncapped: no headless cap (120) in its fps
    #    lines. KRUNKNATIVE_HEADLESS=false is not headless (a "false" that enabled headless gave a
    #    windowless run capped at 120 fps). Needs xvfb-run; skipped without it.
    if shutil.which("xvfb-run"):
        code, text = run("windowed-off", "0 play\n4000 quit\n",
                         {"KRUNKNATIVE_HEADLESS": "false", "KRUNKNATIVE_VSYNC": "0"}, windowed=True)
        lines = fps_lines(text)
        check("KRUNKNATIVE_HEADLESS=false opens a window, not a headless pbuffer",
              "pbuffer, no window" not in text and not re.search(r"\[headless\] using EGL", text), "see " + WORK)
        check("windowed, vsync off, no frame cap: the fps lines carry no headless cap",
              lines and not re.search(r"\(cap \d+ headless", text) and all(c == "0" for c, _ in lines), str(lines[:3]))
        check("windowed, vsync off: the fps lines say vsync 0", lines and all(v == "0" for _, v in lines), str(lines[:3]))
    else:
        print("SKIP  windowed checks: xvfb-run is not installed", flush=True)

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
