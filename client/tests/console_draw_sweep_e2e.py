#!/usr/bin/env python3
"""Every frame the developer console is open, it is drawn: a sweep of the console's toggle across
the whole round-end sequence (client/src/ui/ui_console.c; the HTML engine's paint cache / top
layer: client/src/html/engine/paint.c, engine.c; the client's draw order: client/src/main.c).

  python3 -I client/tests/console_draw_sweep_e2e.py BUILD_DIR [scenario ...]   (run from the repository root)

The report: "pressing backtick to open console then another screen immediately cancelling it out
like the defeat/victory screen playing seems to neutralize the console ... the console clearly just
stops drawing": the console still takes the pointer (its hit test is fine) but nothing of it shows.
The exact trigger is unknown, so this test exercises every way the console can be open across the
end: each scenario is one headless offline Kanji client (1280x720, KRUNKNATIVE_FIXED_DELTA 16.667 ms:
one frame = one 60 Hz step of game time) whose `frames` capture writes EVERY finished frame from
before `con end` through the END, the title (5 s), the results (25 s), the next round and the
respawn; each frame's log line carries the console probe (open / shown / rect) as that frame was
painted, so the test knows which frames must show the console and checks their pixels: the empty
lower part of the log area must be the console's grey (#6a6a6a at opacity .95 over the scene),
and must stay the grey of the first open frame (mean difference). Frames are consumed and deleted
while the client runs (the disk), the failing ones are kept.

Scenarios (the sweep: ` pressed every 100 ms of game time, open for 50 ms, closed for 50 ms, across
the whole sequence; two phases 50 ms apart cover every 50 ms step):
  hold         the console opened before the end and left open through everything
  sweep        the toggle sweep across the END, the title, the results, the next round and the respawn
  sweep2       the same 50 ms later (every 50 ms step of the sequence is a toggle in one of the two)
  menu         the Esc menu open before the end, then the sweep (the menu is up at the END)
  dead         `con kill` before the end (the death menu; a dead player's end shows the title alone), then the sweep
  after        the console opened only after the next round started (the respawn), then held
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
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
try:  # Pillow decodes a 1280x720 frame in a few ms (the pure-Python reader takes a second: the frames would pile up)
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None
from kn_png import read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ONLY = sys.argv[2:]
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-sweep-", dir=CACHE if os.path.isdir(CACHE) else None)
PARALLEL = int(os.environ.get("KN_SWEEP_PARALLEL", "3"))
KEEP_FAILS = 8                 # failing frames kept per scenario
PENDING_MAX = 600              # frames on disk not yet analysed: past this the run is stopped (the disk)
END_SEQUENCE_MS = 3000 + 25000 + 2000   # `con end` (3 s) + the END's endTimer (25 s: the title then the results) + slack
results = []
lock = threading.Lock()


def check(name, ok, detail=""):
    ok = bool(ok)
    with lock:
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and (not ok or len(detail) < 160) else ''}",
              flush=True)


def grave(lines, t):
    lines += [f"{t} down GRAVE", f"{t + 40} up GRAVE"]


def sweep(lines, t0, t1, phase=0):
    """` every 100 ms from t0 + phase to t1: open at t, closed again at t + 50"""
    t = t0 + phase
    while t < t1:
        grave(lines, t)
        grave(lines, t + 50)
        t += 100


def scenario_script(name, work):
    # 0 menu / play / spawn: the menu, Play, the clock held until our player is in the game.
    lines = ["0 menu", "1000 play", "1001 spawn", "1500 respawn"]
    t = 2200
    if name == "hold":
        grave(lines, t)                      # open before the end, left open
        lines += [f"{t + 400} console @open", f"{t + 500} frames {work}/f 100000", f"{t + 600} con end"]
        t_end_seq = t + 600 + END_SEQUENCE_MS
        lines += [f"{t_end_seq} respawn", f"{t_end_seq + 2500} quit"]
    elif name in ("sweep", "sweep2"):
        lines += [f"{t} frames {work}/f 100000", f"{t + 100} con end"]
        t_end_seq = t + 100 + END_SEQUENCE_MS
        sweep(lines, t + 200, t_end_seq, 50 if name == "sweep2" else 0)
        lines += [f"{t_end_seq} respawn"]
        sweep(lines, t_end_seq + 100, t_end_seq + 3000)
        lines += [f"{t_end_seq + 3500} quit"]
    elif name == "menu":
        lines += [f"{t} down ESC", f"{t + 40} up ESC", f"{t + 300} frames {work}/f 100000", f"{t + 400} con end"]
        t_end_seq = t + 400 + END_SEQUENCE_MS
        sweep(lines, t + 500, t_end_seq)
        lines += [f"{t_end_seq} respawn"]
        sweep(lines, t_end_seq + 100, t_end_seq + 3000)
        lines += [f"{t_end_seq + 3500} quit"]
    elif name == "dead":
        lines += [f"{t} con kill", f"{t + 300} frames {work}/f 100000", f"{t + 400} con end"]
        t_end_seq = t + 400 + END_SEQUENCE_MS
        sweep(lines, t + 500, t_end_seq)
        lines += [f"{t_end_seq} respawn"]
        sweep(lines, t_end_seq + 100, t_end_seq + 3000)
        lines += [f"{t_end_seq + 3500} quit"]
    elif name == "after":
        lines += [f"{t} con end"]
        t_end_seq = t + END_SEQUENCE_MS
        lines += [f"{t_end_seq} respawn", f"{t_end_seq + 1000} frames {work}/f 100000"]
        grave(lines, t_end_seq + 1200)
        lines += [f"{t_end_seq + 1300} console @open", f"{t_end_seq + 5000} quit"]
    else:
        raise SystemExit(f"unknown scenario {name}")
    return "\n".join(lines) + "\n"


FRAME_RE = re.compile(r"^\[input_script\] frame (\S+) t=(-?\d+) (.*vp=\S+)$", re.M)  # whole lines only (the client is still writing)


def parse_fields(s):
    return dict(kv.split("=", 1) for kv in s.split() if "=" in kv)


def log_samples(png, rect):
    """RGB samples of the log area's lower, empty part: x 5..90 % of the modal, y 45..72 % (below the
    titlebar and the lines logged, above the hints / input row)"""
    x, y, bw, bh = rect
    points = [(int(x + bw * (0.05 + 0.85 * i / 23)), int(y + bh * (0.45 + 0.27 * j / 7))) for j in range(8) for i in range(24)]
    if Image is not None:
        with Image.open(png) as im:
            rgb = im.convert("RGB")
            w, h = rgb.size
            return [rgb.getpixel((px, py)) for px, py in points if 0 <= px < w and 0 <= py < h]
    w, h, ch, rows = read_rgba(png)
    return [(rows[py][px * ch], rows[py][px * ch + 1], rows[py][px * ch + 2]) for px, py in points if 0 <= px < w and 0 <= py < h]


def grey_share(samples):
    return sum(1 for r, g, b in samples if abs(r - g) < 8 and abs(g - b) < 8 and 70 <= r <= 150) / max(1, len(samples))


def mean_diff(a, b):
    n = min(len(a), len(b))
    return sum(abs(a[i][c] - b[i][c]) for i in range(n) for c in range(3)) / (3.0 * max(1, n))


class Run:
    def __init__(self, name):
        self.name = name
        self.work = os.path.join(WORK, name)
        os.makedirs(os.path.join(self.work, "cfg"), exist_ok=True)
        self.log = os.path.join(self.work, "run.log")
        self.done = set()
        self.base = None
        self.open_frames = self.closed_frames = 0
        self.fails = []          # (frame path, detail)
        self.closed_grey = []    # grey share of closed frames (the check has power when these are low)
        self.last_t = None
        self.open_runs = 0       # how many open stretches were seen

    def consume(self, final):
        """analyse (and delete) every frame whose log line is written"""
        try:
            with open(self.log, errors="replace") as f:
                text = f.read()
        except OSError:
            return
        for m in FRAME_RE.finditer(text):
            path, t, fields = m.group(1), int(m.group(2)), parse_fields(m.group(3))
            if path in self.done:
                continue
            self.done.add(path)
            if not os.path.exists(path):
                continue
            rect = tuple(float(v) for v in fields.get("rect", "0,0,0,0").split(","))
            is_open = fields.get("open") == "1"
            shown = fields.get("shown") == "1"
            keep = False
            if is_open:
                self.open_frames += 1
                if self.last_t is None or self.last_t != "open":
                    self.open_runs += 1
                detail = f"t={t} " + " ".join(f"{k}={v}" for k, v in fields.items() if k in ("open", "shown", "top", "last", "inside", "rect"))
                if not shown or fields.get("top") != "5" or rect[2] < 100 or rect[3] < 100:
                    self.fails.append((path, "open but not shown / topmost / boxed: " + detail))
                    keep = True
                else:
                    samples = log_samples(path, rect)
                    share = grey_share(samples)
                    # 0.7, not 0.85: the panel is .95 opacity, so a bright scene behind it lifts a few samples
                    # (sweep2 f_0185: 0.84 on a correctly drawn console); the failing build reads 0.00
                    if self.base is None and share >= 0.7:
                        self.base = samples
                    diff = mean_diff(self.base, samples) if self.base else 0.0
                    if share < 0.7 or diff > 16.0:
                        self.fails.append((path, f"grey share {share:.2f}, mean diff {diff:.1f}: " + detail))
                        keep = True
                self.last_t = "open"
            else:
                self.closed_frames += 1
                if rect[2] >= 100 and rect[3] >= 100 and self.closed_frames % 7 == 0:
                    self.closed_grey.append(grey_share(log_samples(path, rect)))
                self.last_t = "closed"
            if not keep or len([f for f in self.fails if os.path.exists(f[0])]) > KEEP_FAILS:
                try:
                    os.remove(path)
                except OSError:
                    pass
        if final:
            for path in glob.glob(f"{self.work}/f_*.png"):   # frames without a line (the client died)
                if path not in self.done:
                    os.remove(path)

    def run(self):
        script_path = os.path.join(self.work, "script.txt")
        with open(script_path, "w") as f:
            f.write(scenario_script(self.name, self.work))
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(self.work, "cfg"),
                    "XDG_STATE_HOME": os.path.join(self.work, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_FINDER_URL": "off",
                    "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FIXED_DELTA": "16.667"})
        with open(self.log, "w") as out:
            p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, env=env, start_new_session=True)
            deadline = time.monotonic() + 1500
            try:
                while p.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.5)
                    self.consume(False)
                    pending = len(glob.glob(f"{self.work}/f_*.png")) - len([f for f in self.fails if os.path.exists(f[0])])
                    if pending > PENDING_MAX:
                        check(f"{self.name}: the frames are analysed as fast as they come", False, f"{pending} frames waiting")
                        break
                if p.poll() is None:
                    check(f"{self.name}: the client finished its script", False, f"timeout or stopped, log {self.log}")
            finally:
                if p.poll() is None:  # only the client this test started (by its own process group)
                    os.killpg(p.pid, signal.SIGKILL)
                    p.wait()
        self.consume(True)
        with open(self.log, errors="replace") as f:
            text = f.read()
        check(f"{self.name}: the match ended and the results showed (html.kn-end-open)",
              re.search(r"kn-end-open", text) is not None or "end" in text, "no end in the log")
        check(f"{self.name}: frames with the console open were captured", self.open_frames > 0,
              f"{self.open_frames} open, {self.closed_frames} closed")
        if self.name != "hold" and self.name != "after":
            check(f"{self.name}: the sweep toggled the console many times", self.open_runs >= 50, f"{self.open_runs} open stretches")
        if self.closed_grey:
            low = sum(1 for g in self.closed_grey if g < 0.6)
            check(f"{self.name}: the check tells a closed console from an open one", low >= len(self.closed_grey) * 0.5,
                  f"{low}/{len(self.closed_grey)} closed samples below the grey threshold")
        check(f"{self.name}: every frame with the console open draws it ({self.open_frames} frames)", not self.fails,
              f"{len(self.fails)} frames failed, first: {self.fails[0][1] if self.fails else ''} ({self.fails[0][0] if self.fails else ''})")
        for path, detail in self.fails[1:4]:
            print(f"      also: {detail} ({path})")


def main():
    if not os.path.exists(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin")
        return 1
    names = ONLY or ["hold", "sweep", "sweep2", "menu", "dead", "after"]
    runs = [Run(n) for n in names]
    threads = []
    for r in runs:
        while sum(1 for t in threads if t.is_alive()) >= PARALLEL:
            time.sleep(1.0)
        t = threading.Thread(target=r.run, daemon=True)
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    passed = sum(results)
    print(f"{passed}/{len(results)} passed")
    if passed == len(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs and failing frames kept in {WORK}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
