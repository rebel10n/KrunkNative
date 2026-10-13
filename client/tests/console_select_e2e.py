#!/usr/bin/env python3
"""Text selection in the developer console (client/src/html/engine/selection.c).

  python3 -I client/tests/console_select_e2e.py BUILD_DIR      (run from the repository root)

The bug: "im not able to select anything in the developer console, it's like it has no user
select". The native HTML engine had no text selection outside form controls: mouse drags over the
console's .logs did nothing, and there was nothing for Ctrl+C to copy. (devconsole.css already set
user-select:text on .logs, as devConsole.ts does in v7.1.4, where the browser selects for free.)

An offline Kanji client (headless client_bin, input script, fixed 60 fps step, 1280x720, a fresh
config so the console sits at its default place) opens the console with the ` key, prints three
lines with `echo`, then:
  1. drags from the start of "alpha beta gamma" into "second line here" and presses Ctrl+C: the
     system clipboard (GLFW) holds the three selected lines;
  2. the frame after the drag shows the console's ::selection highlight (#f39c00) on the selected
     lines and none on the line below them;
  3. a double click on "gamma" + Ctrl+C copies that word;
  4. a click on an empty part of the logs (focus) + Ctrl+A + Ctrl+C copies every console line.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import zlib

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-select-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
# The console's log lines at 1280x720 (default geometry: right 8px, top 8px, 744 x 280): the probe
# below logs them, and the drags use these positions (a layout change fails check 0 first).
LINE_X, LINE_Y0, LINE_H = 533.0, 43.0, 15.0
CHAR_W = 7.2  # LuconDevConsole 12px
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def line_y(i):
    return LINE_Y0 + LINE_H * i + LINE_H / 2


def script_text(frames_prefix):
    # lines 0-1: the banner; 2: "] echo alpha beta gamma"; 3: "alpha beta gamma"; 4: "] echo second
    # line here"; 5: "second line here"; 6: "] echo third: x=1 y=2"; 7: "third: x=1 y=2"
    x0 = LINE_X + 1
    return "\n".join([
        "0 menu",
        "200 down GRAVE", "220 up GRAVE",
        "400 con echo alpha beta gamma", "420 con echo second line here", "440 con echo third: x=1 y=2",
        "580 rects @lines .logs>div",
        f"600 cursor {x0:.0f} {line_y(3):.0f}", "620 mdown left",
        f"640 cursor {x0 + 40:.0f} {line_y(4):.0f}",
        f"660 cursor {x0 + 30:.0f} {line_y(5):.0f}",
        f"680 cursor {x0 + CHAR_W * 6 + 1:.0f} {line_y(5):.0f}", "700 mup left",
        "720 down CTRL+C", "740 up CTRL+C", "760 clip @drag",
        f"780 frames {frames_prefix} 1",
        f"800 click {x0 + CHAR_W * 13.5:.0f} {line_y(3):.0f}", f"820 click {x0 + CHAR_W * 13.5:.0f} {line_y(3):.0f}",
        "860 down CTRL+C", "880 up CTRL+C", "900 clip @word",
        f"920 click {x0 + 360:.0f} {line_y(10):.0f}",
        "940 down CTRL+A", "960 up CTRL+A", "980 down CTRL+C", "1000 up CTRL+C", "1020 clip @all",
        "1100 quit",
    ]) + "\n"


def read_png(path):
    """8-bit RGB / RGBA PNG -> (w, h, channels, rows of bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    ch = 4 if ctype == 6 else 3
    raw, stride, rows, prev = zlib.decompress(idat), w * ch, [], bytearray(w * ch)
    for y in range(h):
        ftype, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b, c = prev[i], prev[i - ch] if i >= ch else 0
            if ftype == 1: line[i] = (line[i] + a) & 255
            elif ftype == 2: line[i] = (line[i] + b) & 255
            elif ftype == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, ch, rows


def orange(img, x0, y0, x1, y1):
    w, h, ch, rows = img
    n = 0
    for y in range(max(0, int(y0)), min(h, int(y1))):
        row = rows[y]
        for x in range(max(0, int(x0)), min(w, int(x1))):
            r, g, b = row[x * ch], row[x * ch + 1], row[x * ch + 2]
            if r > 200 and 120 < g < 175 and b < 70:
                n += 1
    return n


def run(timeout=180):
    frames = os.path.join(WORK, "sel")
    script = os.path.join(WORK, "select.script")
    with open(script, "w") as f:
        f.write(script_text(frames))
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS), "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check("client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        text = f.read()
    return text, frames + "_0000.png"


def clip(text, label):
    m = re.search(r"\[input_script\] clip t=\d+ @" + label + r" text=(.*)", text)
    return m.group(1).replace("\\n", "\n").replace("\\\\", "\\") if m else None


try:
    text, png = run()
    m = re.search(r"rects t=\d+ @lines n=(\d+)((?: \S+)*)", text)
    rects = [tuple(float(v) for v in r.split("@")[1].split(",")) for r in (m.group(2).split() if m else [])]
    check("0. console open with 8 lines where the drags expect them", len(rects) == 8 and
          abs(rects[3][0] - LINE_X) < 1 and abs(rects[3][1] - (LINE_Y0 + 3 * LINE_H)) < 1,
          f"{rects[:4]}")
    got = clip(text, "drag")
    check("1. drag across lines + Ctrl+C: the clipboard", got == "alpha beta gamma\n] echo second line here\nsecond",
          repr(got))
    if os.path.exists(png):
        img = read_png(png)
        sel_px = [orange(img, LINE_X, LINE_Y0 + LINE_H * i, LINE_X + 300, LINE_Y0 + LINE_H * (i + 1)) for i in (3, 4, 5)]
        below = orange(img, LINE_X, LINE_Y0 + LINE_H * 6, LINE_X + 300, LINE_Y0 + LINE_H * 8)
        above = orange(img, LINE_X, LINE_Y0, LINE_X + 300, LINE_Y0 + LINE_H * 3)
        check("2. the ::selection highlight on the selected lines only", min(sel_px) > 150 and below == 0 and above == 0,
              f"selected lines {sel_px} px, above {above}, below {below} ({png})")
    else:
        check("2. the ::selection highlight on the selected lines only", False, f"no frame {png}")
    got = clip(text, "word")
    check("3. double click selects a word", got == "gamma", repr(got))
    got = clip(text, "all")
    check("4. Ctrl+A in the logs selects every line", got is not None and got.startswith("Krunker developer console")
          and got.endswith("] echo third: x=1 y=2\nthird: x=1 y=2") and got.count("\n") == 7, repr(got))
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("console_select_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
