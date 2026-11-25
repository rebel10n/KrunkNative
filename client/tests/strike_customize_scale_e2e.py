#!/usr/bin/env python3
"""Krunker Strike's Customize Controls layer is scaled by its clientHeight, and its joysticks are whole px
(client/src/ui/strike_menu.c customize_controls_tick).

FRVR's a5.oncreate sets .control-buttons' --base-scale to `calc(${clientHeight} / 360 * 1px` and the static
nipplejs joysticks to --control-joystick-size at that scale, rounded. Chrome's capture of the screen
(doc/ref/krunker-strike-1.37.2/strike-customize-controls.holder.html, FRVR 1.37.2 at 640 x 360) has
`calc(282 / 360 * 1px` on the 284 px box with its 1px dashed border, .back 83px / 69px (106 and 88 x 282 / 360
= 83.03 / 68.93) and .front 41.5px / 34.5px.

One headless render of the uidiff scenario strike/customize-controls with KRUNKNATIVE_SCENARIO_HTML (the
captured DOM): the box's --base-scale and every .back / .front width, height and margins must be the
capture's. The engine used offsetHeight truncated to an int (283) and the unrounded size (83.33 / 69.18), so
the movement nipple was 1 CSS px larger and higher than Chrome's (uidiff move-joystick 4.93 %).

  python3 -I client/tests/strike_customize_scale_e2e.py BUILD_DIR [SRC]

Headless client_bin --render-scenario (KRUNKNATIVE_HEADLESS=1). Exit 77 without client_bin.
"""
import os
import re
import signal
import subprocess
import sys
import tempfile

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.getcwd())
BIN = os.path.join(BUILD, "client_bin")
if not os.access(BIN, os.X_OK):
    print("strike_customize_scale_e2e: no client_bin: skipped")
    sys.exit(77)

SCENARIO = os.path.join(SRC, "tools", "uidiff", "scenarios", "strike", "customize-controls.json")
CAPTURE = os.path.join(SRC, "doc", "ref", "krunker-strike-1.37.2", "strike-customize-controls.holder.html")
FAILS = []


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % str(detail)[:400]), flush=True)
    if not ok:
        FAILS.append(what)


def style_of(tag):
    m = re.search(r'style="([^"]*)"', tag)
    out = {}
    for decl in (m.group(1) if m else "").split(";"):
        if ":" in decl:
            k, v = decl.split(":", 1)
            out[k.strip()] = v.strip()
    return out


def scale_and_sticks(doc):
    """--base-scale's px count (the N of calc(N / 360 * 1px)) and each joystick part's
    (class, width, height, margin-left, margin-top), in document order."""
    box = re.search(r'<div[^>]*class="[^"]*control-buttons--customize[^"]*"[^>]*>', doc)
    scale = None
    if box:
        m = re.search(r"calc\((\d+(?:\.\d+)?) / 360 \* 1px", style_of(box.group(0)).get("--base-scale", ""))
        scale = float(m.group(1)) if m else None
    parts = []
    for tag in re.findall(r'<div[^>]*class="(?:back|front)"[^>]*>', doc):
        st = style_of(tag)
        parts.append((re.search(r'class="(\w+)"', tag).group(1),) +
                     tuple(st.get(k) for k in ("width", "height", "margin-left", "margin-top")))
    return scale, parts


def main():
    with open(CAPTURE, encoding="utf-8") as f:
        want_scale, want = scale_and_sticks(f.read())
    check("the capture: --base-scale 282 and two joysticks (back + front each)", want_scale == 282.0 and len(want) == 4,
          (want_scale, want))

    work = tempfile.mkdtemp(prefix="kn-strike-customize.", dir=os.path.expanduser("~/.cache"))
    html_out = os.path.join(work, "page.html")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_EGL_DEVICE=os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"),
               LIBGL_ALWAYS_SOFTWARE="1", KRUNKNATIVE_SCENARIO_HTML=html_out, KRUNKNATIVE_AUDIO_BACKEND="null",
               KRUNKNATIVE_NO_BROWSER="1", KRUNKNATIVE_UPDATES_URL="off", XDG_CONFIG_HOME=os.path.join(work, "cfg"),
               XDG_STATE_HOME=os.path.join(work, "state"))
    log = os.path.join(work, "client.log")
    with open(log, "w") as out:
        p = subprocess.Popen([BIN, "--render-scenario", SCENARIO, os.path.join(work, "frame.png")], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, cwd=SRC, start_new_session=True)
        try:
            rc = p.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = -9
    check("client_bin exits 0", rc == 0, "rc=%s, %s" % (rc, work))
    doc = ""
    if os.path.isfile(html_out):
        with open(html_out, encoding="utf-8", errors="replace") as f:
            doc = f.read()
    scale, parts = scale_and_sticks(doc)
    check("--base-scale is the box's clientHeight (282, the capture's)", scale == want_scale, scale)
    check("the joysticks' back / front sizes and margins are the capture's", parts == want, (parts, want))
    if FAILS:
        print("strike_customize_scale_e2e: %d failure(s)" % len(FAILS))
        return 1
    print("strike_customize_scale_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
