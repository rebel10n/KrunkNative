#!/usr/bin/env python3
"""Every class draws: the menu's class preview and the first-person view in play
(client/src/ui/class_preview.c, menu_ui.c menu_select_class).

  python3 -I client/tests/class_preview_all_e2e.py BUILD_DIR      (run from the repository root)

"the class list appears blank", "the camera is blank for some classes": one offline Kanji client
(headless, KRUNKNATIVE_UI_CMDS) picks every class the menu has in turn (selectClass(i) until the
class box comes back empty) and
  1. on the menu: the preview's render target (KRUNKNATIVE_PREVIEW_DUMP with %d: one dump per
     class) holds a model (enough lit pixels, a player-sized box) and the class box names the class;
  2. in play: dies (`con kill`), takes the class, plays again and screenshots the view, which must
     not be blank (no single colour over most of the frame, the weapon / hands area has detail).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
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
WORK = tempfile.mkdtemp(prefix="kn-class-all-", dir=CACHE if os.path.isdir(CACHE) else None)
CMDS = os.path.join(WORK, "cmds")
LOG = os.path.join(WORK, "client.log")
MAX_CLASSES = 40
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def lines():
    try:
        with open(LOG, errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def wait_for(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return fn()


def cmd(line):
    mark = len(lines())
    with open(CMDS, "a") as f:
        f.write(line + "\n")
    return mark


def after(mark, pattern):
    return [l for l in lines()[mark:] if re.search(pattern, l)]


def text_of(elem_id):
    mark = cmd(f"textof {elem_id}")
    got = wait_for(lambda: after(mark, rf"\[ui_cmds\] textof {elem_id} = "), 5.0)
    m = re.search(r"= '(.*)'", got[-1]) if got else None
    return m.group(1).strip() if m else None


def load(path):
    from PIL import Image
    import numpy as np
    return np.asarray(Image.open(path).convert("RGB"), dtype=float)


def model_stats(img):
    """Lit pixels (the render target is premultiplied over black) and their bounding box."""
    import numpy as np
    lit = img.sum(2) > 24
    n = int(lit.sum())
    if not n:
        return 0.0, 0, 0
    ys, xs = np.nonzero(lit)
    return n / lit.size, int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1)


def blank_share(img):
    """Share of the frame within 10 of its most common colour (a blank view is ~1)."""
    import numpy as np
    q = (img // 8).astype(int)
    keys = q[..., 0] * 1024 + q[..., 1] * 32 + q[..., 2]
    vals, counts = np.unique(keys, return_counts=True)
    mode = vals[counts.argmax()]
    ref = np.array([mode // 1024, (mode // 32) % 32, mode % 32]) * 8 + 4
    return float((np.abs(img - ref).max(2) <= 10).mean())


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin", file=sys.stderr)
        return 2
    open(CMDS, "w").close()
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": CMDS, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_PREVIEW_DUMP": os.path.join(WORK, "preview-%d.ppm"), "KRUNKNATIVE_PREVIEW_LOG": "1"})
    with open(LOG, "w") as out:
        proc = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    try:
        run()
    finally:
        cmd("quit")
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        if results and all(results) and not os.environ.get("KN_TEST_KEEP"):
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


def run():
    check("the menu's class preview comes up", wait_for(lambda: any("[preview] first frame" in l for l in lines()), 90.0))
    time.sleep(1.0)
    names = []
    # 1. the menu: every class's preview model
    for i in range(MAX_CLASSES):
        dump = os.path.join(WORK, f"preview-{i}.ppm")
        cmd(f"ui selectClass({i})")
        time.sleep(0.5)
        name = text_of("menuClassName")
        if not name:
            break  # past the last class: the class box is empty
        names.append(name)
        ok = wait_for(lambda: os.path.isfile(dump) and os.path.getsize(dump) > 64, 15.0)
        time.sleep(0.3)  # the dump is written in one go; let the file close
        share, w, h = model_stats(load(dump)) if ok else (0.0, 0, 0)
        check(f"menu {i} {name}: the preview draws the model", share > 0.02 and h > 80 and w > 30,
              f"lit {share:.3f}, box {w}x{h}{'' if ok else ', no dump'}")
    check("the menu lists the classes", len(names) >= 10, f"{len(names)}: {', '.join(names)}")

    # 2. in play: each class's first-person view
    cmd("ui selectClass(0)")
    time.sleep(1.0)
    mark = cmd("click 640 200")  # CLICK TO PLAY
    check("Play enters the game", wait_for(lambda: after(mark, r"listener instructionHolder mousedown"), 10.0))
    time.sleep(3.0)
    for i, name in enumerate(names):
        if i:
            cmd("con kill")
            time.sleep(3.5)
            cmd(f"ui selectClass({i})")
            time.sleep(1.0)
            cmd("click 640 200")
            time.sleep(3.5)
        shot = os.path.join(WORK, f"play-{i}.png")
        mark = cmd(f"shot {shot}")
        ok = wait_for(lambda: after(mark, re.escape(f"wrote {shot}")), 10.0)
        if not ok:
            check(f"play {i} {name}: a screenshot", False)
            continue
        img = load(shot)
        whole = blank_share(img)
        hands = img[360:720, 380:1280]  # where the weapon / hands sit
        check(f"play {i} {name}: the view is drawn", whole < 0.6 and hands.std() > 12,
              f"single-colour share {whole:.2f}, weapon area std {hands.std():.1f}")


if __name__ == "__main__":
    sys.exit(main())
