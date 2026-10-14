#!/usr/bin/env python3
"""The developer console is never hidden while it is on (client/src/ui/ui_console.c, the HTML
engine's top layer: html_dom_set_top_layer, html_ui_render_page / _top).

  python3 -I client/tests/console_visible_e2e.py BUILD_DIR      (run from the repository root)

The bug: "the developer console still randomly disappears sometimes. it should never be completely
hidden from the user." Its causes: devconsole.css's z-index 999999 lost to the v10 page's higher
layers (the view stack, #popupHolder, popup windows, the loader, a custom.css); the GL drawn over
the page natively (the item view's 3D showcase, the confetti) covered it; the editor / hub / viewer
pages had no console at all (and the game page's overlay was built again on return); the close
button left its hidden input focused, so the toggle key typed into it instead of opening the
console; its open state did not survive a reload or restart.

Run 1, a headless offline Kanji client (1280x720, a fresh config whose custom.css
puts a translucent red layer with the highest z-index over the whole page, under the console only
if the console is really on top) opens the console with ` in the menu, then goes through: Settings,
Customize, Market, the v10 login view stack (appended to <body> after the console), the item view
(the console is placed over the middle, where its GL showcase draws the item: the frame's pixels
there must be the console's), the game,
death (`kill`), respawn, the match end (`end`), F5 (full reload), the window at 800x600 and at
2560x1440, the editor page and back to the game page. After every step the console probe
(input_script `console`) must say: open, shown, its sheet applied, <body>'s last element, painted
topmost at 5 points of its box and the whole box inside the viewport. Then the close button hides
it and ` opens it again (game page and editor page).
Run 2 on the same config: the console comes back open (kn_dev_console_open), on top.
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

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
from kn_png import read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-visible-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
CONSOLE_X = ".modal[data-kn-pointer] .titlebar .x"  # '#' starts a comment in input scripts
COVER_CSS = ("#loadingBg1 { display: block !important; position: fixed !important; left: 0 !important; "
             "top: 0 !important; width: 100% !important; height: 100% !important; z-index: 2147483647 !important; "
             "background: rgba(255, 0, 0, 0.5) !important; pointer-events: none !important; opacity: 1 !important; }\n")
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and (not ok or len(detail) < 160) else ''}",
          flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def run_client(name, script, timeout=420):
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": CFG,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    return read(log)


def probes(text):
    """input_script `console @label ...` -> {label: {key: value}} (the last line per label)"""
    out = {}
    for m in re.finditer(r"\[input_script\] console t=\d+ @(\S+) (.*)$", text, re.M):
        out[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
    return out


def rects(text, label):
    m = None
    for m in re.finditer(r"\[input_script\] rects t=\d+ @" + re.escape(label) + r" n=(\d+)(.*)$", text, re.M):
        pass
    if not m:
        return None
    b = re.search(r"@(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)", m.group(2))
    return tuple(float(v) for v in b.groups()) if b else None


def visible(p, vp=None):
    """open, shown, styled, last in <body>, topmost at all 5 points, inside the viewport"""
    if not p:
        return False, "no probe"
    ok = all(p.get(k) == "1" for k in ("open", "built", "shown", "sheet", "last", "inside")) and p.get("top") == "5"
    if vp:
        ok = ok and p.get("vp") == vp
    return ok, " ".join(f"{k}={v}" for k, v in p.items())


def console_pixels(png, logs, frame):
    """share of sampled pixels in logs ∩ frame (CSS px = framebuffer px at dpr 1) that are the console's
    log background (#6a6a6a at opacity .95 over anything) rather than the GL showcase"""
    x0, y0 = max(logs[0], frame[0]) + 4, max(logs[1], frame[1]) + 4
    x1, y1 = min(logs[0] + logs[2], frame[0] + frame[2]) - 20, min(logs[1] + logs[3], frame[1] + frame[3]) - 4
    if x1 - x0 < 20 or y1 - y0 < 10:
        return None, 0
    w, h, ch, rows = read_rgba(png)
    good = total = 0
    for y in range(int(y0), int(y1), 3):
        for x in range(int(x0), int(x1), 7):
            r, g, b = rows[y][x * ch], rows[y][x * ch + 1], rows[y][x * ch + 2]
            total += 1
            # text lines are lighter / coloured: only the background is counted against the console
            good += 94 <= r <= 120 and 94 <= g <= 120 and 94 <= b <= 120 or r > 150 and g > 150 and b > 150
    return (good / total if total else None), total


def main():
    if not os.path.exists(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin")
        return 1
    os.makedirs(os.path.join(CFG, "krunknative"), exist_ok=True)
    with open(os.path.join(CFG, "krunknative", "custom.css"), "w") as f:
        f.write(COVER_CSS)
    # the console over the middle of the window, where the item view's 3D showcase draws its model
    with open(os.path.join(CFG, "krunknative", "krk_dev_console_geom.json"), "w") as f:
        f.write('{"left":300,"top":200,"width":700,"height":400}')
    shot = os.path.join(WORK, "itemview")
    steps = [  # (label, events before the probe, ms to wait before it)
        ("menu", ["down GRAVE", "up GRAVE"], 600),
        ("settings", ["call showWindow 1"], 600),
        ("customize", ["call clearPops", "call showWindow 3"], 600),
        ("market", ["call clearPops", "call openExchange"], 600),
        ("login", ["call clearPops", "call loginOrRegister 0"], 600),
        ("itemview", ["call knV10LoginClose", "call viewItem 1000 0"], 1200),
        ("game", ["call clearPops", "play"], 1500),
        ("dead", ["con kill"], 1500),
        ("respawn", ["respawn"], 6000),
        ("end", ["con end"], 4000),
        ("reload", ["down F5", "up F5"], 3000),
        ("w800", ["resize 800 600"], 1000),
        ("w2560", ["resize 2560 1440"], 1000),
        ("editor", ["resize 1280 720", "page editor"], 5000),
        ("back", ["page game"], 6000),
    ]
    lines, t = ["0 menu"], 200
    for label, events, wait in steps:
        for e in events:
            lines.append(f"{t} {e}")
            t += 50
        t += wait
        lines.append(f"{t} console @{label}")
        if label == "itemview":
            lines.append(f"{t} rects @logs .modal[data-kn-pointer] .logs")
            lines.append(f"{t} rects @frame [id=itemViewPop] iframe")
            lines.append(f"{t + 20} frames {shot} 1")
        if label == "end":
            lines.append(f"{t} vis @endscreen endUI")
        if label == "dead":
            lines.append(f"{t} vis @deathcard deathUIHolder")
        if label == "respawn":
            lines.append(f"{t} vis @alive inGameUI deathUIHolder")
        t += 100
    # the close button, then ` again: on the game page, then on the editor page
    lines += [f"{t} clicksel {CONSOLE_X}", f"{t + 300} console @closed", f"{t + 400} down GRAVE", f"{t + 450} up GRAVE",
              f"{t + 900} console @reopened", f"{t + 1000} page editor", f"{t + 6000} console @editor2",
              f"{t + 6100} clicksel {CONSOLE_X}", f"{t + 6400} console @edclosed", f"{t + 6500} down GRAVE",
              f"{t + 6550} up GRAVE", f"{t + 7000} console @edreopened", f"{t + 7100} page game",
              f"{t + 13000} console @final", f"{t + 13200} quit"]
    text = run_client("cycle", "\n".join(lines) + "\n")
    p = probes(text)
    sizes = {"w800": "800x600", "w2560": "2560x1440"}
    for label, _, _ in steps:
        ok, detail = visible(p.get(label), sizes.get(label))
        check(f"{label}: the console is shown, on top and inside the window", ok, detail)
    check("editor: the console is on the editor page", p.get("editor", {}).get("page") == "editor.html", str(p.get("editor")))
    check("back: on the game page again", p.get("back", {}).get("page") == "index.html", str(p.get("back")))
    dead = re.search(r"\[input_script\] vis t=\d+ @deathcard deathUIHolder=(\S+)", text)
    check("dead: the probe ran on the death card", dead and dead.group(1) == "1", dead.group(0) if dead else "no vis line")
    alive = re.search(r"\[input_script\] vis t=\d+ @alive inGameUI=(\S+) deathUIHolder=(\S+)", text)
    check("respawn: the probe ran alive again", alive and alive.groups() == ("1", "0"), alive.group(0) if alive else "no vis line")
    end = re.search(r"\[input_script\] vis t=\d+ @endscreen endUI=(\S+)", text)
    check("end: the probe ran on the end screen", end and end.group(1) == "1", end.group(0) if end else "no vis line")
    check("reload: F5 ran the full reload", "[reload] F5: full reload" in text)

    logs, frame = rects(text, "logs"), rects(text, "frame")
    png = f"{shot}_0000.png"
    share, n = console_pixels(png, logs, frame) if logs and frame and os.path.exists(png) else (None, 0)
    check("item view: its GL showcase overlaps the console", share is not None, f"logs {logs}, showcase {frame}")
    check("item view: the console is drawn over the GL showcase", share is not None and share >= 0.85,
          f"{share and round(share, 3)} of {n} samples are the console's")

    closed, reopened = p.get("closed", {}), p.get("reopened", {})
    check("close button: the console hides", closed.get("open") == "0" and closed.get("shown") == "0", str(closed))
    check("close button, then `: the console opens again", visible(reopened)[0], str(reopened))
    edc, edr = p.get("edclosed", {}), p.get("edreopened", {})
    check("editor page: the close button hides it", edc.get("open") == "0" and edc.get("page") == "editor.html", str(edc))
    check("editor page: ` opens it again", visible(edr)[0] and edr.get("page") == "editor.html", str(edr))
    check("game page again: still open and on top", visible(p.get("final"))[0], str(p.get("final")))

    text = run_client("restart", "0 menu\n800 console @restored\n1000 quit\n")
    ok, detail = visible(probes(text).get("restored"))
    check("restart: the console comes back open and on top", ok, detail)

    passed = sum(results)
    print(f"{passed}/{len(results)} passed")
    if passed == len(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs and frames kept in {WORK}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
