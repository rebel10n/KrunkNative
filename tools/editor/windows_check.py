#!/usr/bin/env python3
"""The editor's API windows and publishing popups vs Krunker v7.1.4 (doc/v7/editor.md WP6): windows
2 Assets, 5 Mods, 7 Textures, 9 Sounds, 14 Maps (My Games); the popups of v7ref.js's net-* states
(publish / update game, publish asset, My Games edit, mapResp, delete confirmation).

    python3 -I tools/editor/windows_check.py --bin <build>/client_bin [--root .] [--keep DIR]

One headless native editor run (EGL, no window on any display; no brain: --offline and
KRUNKNATIVE_BRAIN_URL=off) dumps each window with the editor script's dump:window (the format of
tools/editor/v7ref.js) and compares it with doc/ref/krunker-v7.1.4/editor/window<N>.dump.txt.

The popups are opened with the editor script's net:popup:<name> (a stand-in free account, like the
captures' faked KE.account) and dumped with dump:popup.

Documented differences (editor_net.c's header): v7's capture has no API, so its fetch never answers
and the list shows "Loading..." forever; the native list shows the window's empty text when there
is no brain. An empty text / number field dumps as `null` (no value attribute) or empty: the
native engine gives a laid-out field an empty value attribute, Chromium does not; both count as
empty. tools/editor/
publish_e2e.py checks the same windows and popups with a kn_brain.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

WINDOWS = {2: "No Assets found", 5: "No Mods found", 7: "No Textures found", 9: "No Sounds found", 14: "No Games found!"}
POPUPS = ("publish-game", "update-game", "publish-asset", "publish-asset-sound", "edit-map", "map-resp", "delete-map")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="client_bin")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--keep", help="keep the dumps and the client log in this directory")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    ref = os.path.join(root, "doc", "ref", "krunker-v7.1.4", "editor")
    out = a.keep or tempfile.mkdtemp(prefix="kn-windows-check-", dir=os.path.expanduser("~/.cache"))
    os.makedirs(out, exist_ok=True)
    script = []
    for n in WINDOWS:
        script += ["window:%d" % n, "wait:0.2", "dump:window:%s" % os.path.join(out, "window%d.txt" % n)]
    script.append("closewindow")
    for n in POPUPS:
        script += ["net:popup:%s" % n, "wait:0.2", "dump:popup:%s" % os.path.join(out, "popup-%s.txt" % n)]
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(k, None)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_BRAIN_URL": "off", "XDG_CONFIG_HOME": os.path.join(out, "cfg"),
                "KRUNKNATIVE_SCREENSHOT": os.path.join(out, "shot.png"),
                "KRUNKNATIVE_EDITOR_SCRIPT": ";".join(script) + ";wait:0.3"})
    with open(os.path.join(out, "client.log"), "w") as log:
        r = subprocess.run([os.path.abspath(a.bin), "--offline", "--editor", "new"], cwd=root, env=env,
                           stdout=log, stderr=subprocess.STDOUT, timeout=300)
    if r.returncode != 0:
        print("client_bin exited with %d (log: %s)" % (r.returncode, os.path.join(out, "client.log")))
        return 1
    failed = same = 0
    for n, none in WINDOWS.items():
        exp = open(os.path.join(ref, "window%d.dump.txt" % n)).read().splitlines()
        try:
            got = open(os.path.join(out, "window%d.txt" % n)).read().splitlines()
        except OSError:
            got = None
        if got == exp:
            same += 1
        documented = [("X " + none) if l == "X Loading..." else l for l in exp]
        if got == documented:
            print("ok   window %d" % n)
        else:
            failed += 1
            print("FAIL window %d (diff %s %s)" % (n, os.path.join(ref, "window%d.dump.txt" % n), os.path.join(out, "window%d.txt" % n)))
    print("windows: %d/%d identical to v7.1.4, %d/%d with the documented difference" % (same, len(WINDOWS), len(WINDOWS) - failed, len(WINDOWS)))
    psame = pfail = 0
    for n in POPUPS:
        theirs = os.path.join(ref, "net", n + ".dump.txt")
        exp = open(theirs).read().splitlines()
        try:
            got = open(os.path.join(out, "popup-%s.txt" % n)).read().splitlines()
        except OSError:
            got = None
        if got == exp:
            psame += 1
        def empty_null(lines):
            return [re.sub(r"^(I (text|number)\(.*\))=null$", r"\1=", l) for l in lines]
        if got == exp or (got is not None and empty_null(got) == empty_null(exp)):
            print("ok   popup %s" % n)
        else:
            pfail += 1
            print("FAIL popup %s (diff %s %s)" % (n, theirs, os.path.join(out, "popup-%s.txt" % n)))
    print("popups: %d/%d identical to v7.1.4, %d/%d with the documented difference" % (psame, len(POPUPS), len(POPUPS) - pfail, len(POPUPS)))
    failed += pfail
    if not a.keep and not failed:
        shutil.rmtree(out, ignore_errors=True)
    elif not a.keep:
        print("dumps and client log kept in %s" % out)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
