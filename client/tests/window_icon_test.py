#!/usr/bin/env python3
"""The client's window icon and window class (client/src/main.c, the window created before the first frame).

  python3 -I client/tests/window_icon_test.py BUILD_DIR      (run from the repository root)

A headless run (client_bin --offline, no window: KRUNKNATIVE_HEADLESS=1, no DISPLAY) prints
  [window] title "Krunker Box Office", class "<wm class>", icon <path> (WxH)
and this checks that line and the file it names:
  1. the icon is the one under the checkout's assets (assets/img/icon.png, 256 x 256), and its pixels are
     the Box Office art (tools/branding/box-office/icon-192.png, resized to 256 the way fetch_branding.py
     renders it): a krunker.io copy, or a stale one, fails here;
  2. the window class is not the window title. Taskbars (XFCE's docklike, GNOME) look a window up in the
     installed .desktop entries by its class, and a GIO search for "Krunker Box Office" finds the user's own
     Brave shortcut of that name, whose Icon= then wins over the window's _NET_WM_ICON. The class is
     krunknative (krunknative-strike for Strike): no desktop entry of ours matches it.

Exit 0 when both hold, 1 when not, 77 when Pillow is missing (the pixel check needs it).
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TITLE = "Krunker Box Office"
ICON_LINE = re.compile(r'\[window\] title "(.*)", class "(.*)", icon (.*) \((\d+)x(\d+)\)$')
CLASSES = ("krunknative", "krunknative-strike")


def fail(msg):
    print("FAIL %s" % msg)
    return 1


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    try:
        from PIL import Image
    except ImportError:
        print("SKIP window_icon_test: Pillow is not installed")
        return 77

    build = os.path.abspath(argv[1])
    client = os.path.join(build, "client_bin")
    # a git worktree lacks the untracked assets (fonts): KRUNKNATIVE_ASSETS_DIR names the checkout's
    assets = os.path.abspath(os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets"))
    art = os.path.join(ROOT, "tools", "branding", "box-office", "icon-192.png")

    env = dict(os.environ, KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_ASSETS_DIR=assets)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    with tempfile.TemporaryDirectory(prefix="kn-window-icon-") as tmp:
        env["XDG_CONFIG_HOME"] = os.path.join(tmp, "cfg")
        env["KRUNKNATIVE_SCREENSHOT"] = os.path.join(tmp, "shot.png")
        run = subprocess.run([client, "--offline"], cwd=build, env=env, capture_output=True, text=True, timeout=120)
    if run.returncode != 0:
        return fail("client_bin exited %d" % run.returncode)

    lines = [m for m in (ICON_LINE.match(l) for l in run.stdout.splitlines()) if m]
    if len(lines) != 1:
        old = [l for l in run.stdout.splitlines() if l.startswith("[window] title")]
        if old:
            return fail("the window line has no class (%s): its WM_CLASS is the title" % old[0])
        return fail("expected one '[window] title, class, icon' line, got %d" % len(lines))
    title, wm_class, icon_path, w, h = lines[0].groups()
    w, h = int(w), int(h)
    print("[window] title %r, class %r, icon %s (%dx%d)" % (title, wm_class, icon_path, w, h))

    status = 0
    if title != TITLE:
        status |= fail("window title %r, not %r" % (title, TITLE))
    icon_file = os.path.realpath(os.path.join(build, icon_path))
    if icon_file != os.path.realpath(os.path.join(assets, "img", "icon.png")):
        status |= fail("icon read from %s, not the checkout's assets/img/icon.png" % icon_file)
    if (w, h) != (256, 256):
        status |= fail("icon is %dx%d, not 256x256" % (w, h))

    loaded = Image.open(icon_file).convert("RGBA")
    want = Image.open(art).convert("RGBA").resize((256, 256), Image.LANCZOS)
    if loaded.size != (256, 256) or loaded.tobytes() != want.tobytes():
        status |= fail("icon pixels are not the Box Office art (tools/branding/box-office/icon-192.png at 256)")
    else:
        print("icon pixels: the Box Office art at 256 (pixel(128,128) %s)" % (loaded.getpixel((128, 128)),))

    if wm_class == title or wm_class not in CLASSES:
        status |= fail("window class %r: must be one of %s, not the title (taskbars match a desktop entry by it)"
                       % (wm_class, ", ".join(CLASSES)))

    print("PASS window_icon_test" if status == 0 else "FAILED window_icon_test")
    return 1 if status else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
