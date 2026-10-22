#!/usr/bin/env python3
"""The developer console keeps drawing once open, through the match end (client/src/ui/ui_console.c,
the HTML engine's paint cache and top layer: client/src/html/engine/paint.c, engine.c).

  python3 -I client/tests/console_end_paint_e2e.py BUILD_DIR      (run from the repository root)

The report: after the victory / defeat end screen appears while the console is open, the console
stops drawing (invisible) but still absorbs the pointer and changes the cursor. This test runs a
headless offline client (Kanji, 1280x720): a match starts, the console is opened in the game, the
match ends (`con end`, the server's match timer; the player is respawned first, so the END shows the end
screen), the script waits for the results (html.kn-end-open), and a frame is captured every 600 ms for
POST_FRAMES frames after that. Every frame must show the console painted: its log area's empty lower part
(no text in it) is the console's grey, and it looks the same as in the frame taken just before the end
(the translucent console lets the scene through a little, so the check is on a mean difference).
The console probe (input_script `console`) must say open, shown and topmost after the end too.

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when every check passed.
"""
import glob
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
from kn_png import read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-end-paint-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
POST_FRAMES = 14           # a frame every 600 ms after the results show (the title, the results, the next round)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and (not ok or len(detail) < 160) else ''}",
          flush=True)


def script():
    # respawn: the player is alive in the match, so the END shows the end screen (events_end: a player
    # who is not in the game gets none). waitfor html.kn-end-open: the clock holds until the results
    # show (the match timer runs in game time and the headless frame rate varies), then the frames.
    lines = ["0 menu", "1000 console @menu",
             "1300 play", "1800 respawn",
             "2800 down GRAVE", "2840 up GRAVE", "3200 console @open",
             f"3300 rects @logs .modal[data-kn-pointer] .logs", f"3350 frames {WORK}/pre 1",
             "3500 con end",
             "3600 waitfor html.kn-end-open", "3650 attr @results class html"]
    t = 3700
    for k in range(POST_FRAMES):
        lines += [f"{t} console @post{k}", f"{t + 30} rects @logs{k} .modal[data-kn-pointer] .logs",
                  f"{t + 50} frames {WORK}/post{k:02d} 1"]
        t += 600
    lines.append(f"{t + 300} quit")
    return "\n".join(lines) + "\n"


def run_client(script_text):
    script_path = os.path.join(WORK, "script.txt")
    with open(script_path, "w") as f:
        f.write(script_text)
    log = os.path.join(WORK, "run.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": CFG,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=600)
        except subprocess.TimeoutExpired:
            check("the client finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:  # only the client this test started (by its own process group)
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def probes(text):
    out = {}
    for m in re.finditer(r"\[input_script\] console t=\d+ @(\S+) (.*)$", text, re.M):
        out[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
    return out


def rect_of(text, label):
    m = None
    for m in re.finditer(r"\[input_script\] rects t=\d+ @" + re.escape(label) + r" n=(\d+)(.*)$", text, re.M):
        pass
    if not m:
        return None
    b = re.search(r"@(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)", m.group(2))
    return tuple(float(v) for v in b.groups()) if b else None


def empty_log_samples(png, logs):
    """RGB samples of the log area's lower part (no text there): the console's background"""
    w, h, ch, rows = read_rgba(png)
    x, y, bw, bh = logs
    out = []
    for j in range(8):
        for i in range(24):
            px = int(x + bw * (0.04 + 0.92 * i / 23))
            py = int(y + bh * (0.55 + 0.4 * j / 7))
            if 0 <= px < w and 0 <= py < h:
                out.append((rows[py][px * ch], rows[py][px * ch + 1], rows[py][px * ch + 2]))
    return out


def grey_share(samples):
    return sum(1 for r, g, b in samples if abs(r - g) < 8 and abs(g - b) < 8 and 70 <= r <= 150) / max(1, len(samples))


def mean_diff(a, b):
    n = min(len(a), len(b))
    return sum(abs(a[i][c] - b[i][c]) for i in range(n) for c in range(3)) / (3.0 * max(1, n))


def main():
    if not os.path.exists(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin")
        return 1
    os.makedirs(CFG, exist_ok=True)
    text = run_client(script())
    p = probes(text)
    opened = p.get("open", {})
    check("the console is open in the game before the end", opened.get("open") == "1" and opened.get("top") == "5",
          " ".join(f"{k}={v}" for k, v in opened.items()))
    check("the match ended (the end probe ran)", "post0" in p, "no post-end probe")
    results_cls = re.search(r"attr t=\d+ @results n=\d+ class=(.*)$", text, re.M)
    check("the end screen is up (html.kn-end-open, the results)", bool(results_cls) and "kn-end-open" in results_cls.group(1),
          results_cls.group(1) if results_cls else "no results probe")
    logs = rect_of(text, "logs")
    pre = sorted(glob.glob(f"{WORK}/pre_*.png"))
    if not logs or not pre:
        check("the log area and the frame before the end were captured", False, f"logs {logs}, frames {len(pre)}")
    else:
        base = empty_log_samples(pre[0], logs)
        check("before the end: the log area is the console's grey", grey_share(base) >= 0.9, f"grey share {grey_share(base):.2f}")
        for k in range(POST_FRAMES):
            found = sorted(glob.glob(f"{WORK}/post{k:02d}_*.png"))
            frame = found[0] if found else ""
            pk = p.get(f"post{k}", {})
            shown = pk.get("open") == "1" and pk.get("shown") == "1" and pk.get("top") == "5"
            if not shown:
                check(f"after the end, frame {k}: the console is open, shown and topmost", False,
                      " ".join(f"{a}={b}" for a, b in pk.items()))
                continue
            rk = rect_of(text, f"logs{k}")
            if not rk or not frame:
                check(f"after the end, frame {k}: captured with the log area", False, f"logs {rk}, frame {bool(frame)}")
                continue
            samples = empty_log_samples(frame, rk)
            share, diff = grey_share(samples), mean_diff(base, samples)
            check(f"after the end, frame {k}: the console is drawn (log area grey, unchanged)",
                  share >= 0.9 and diff <= 10.0, f"grey share {share:.2f}, mean diff {diff:.1f}")

    passed = sum(results)
    print(f"{passed}/{len(results)} passed")
    if passed == len(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs and frames kept in {WORK}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
