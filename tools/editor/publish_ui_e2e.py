#!/usr/bin/env python3
"""Editor Game / Publish / Templates / start-up windows against Krunker v10.0.2 (E10 / IDX / CFG; doc/v10/editor-social.md).

  python3 -I tools/editor/publish_ui_e2e.py [--bin cmake-build-debug/client_bin] [-k NAME]

Headless `client_bin --editor` runs (EGL pbuffer, no window, no brain) driven by KRUNKNATIVE_EDITOR_SCRIPT,
then the windows and popups are dumped as text (dump:window / dump:popup / dump:toolbar / objdump) and
the stderr log is read. Checks, one per v10 behaviour:

  test_play_validation     Test Game (play button) runs v10's validateMap(export, cb, true) first: an empty
                           map shows "Missing objects." in the response popup and no game window opens.
  test_play_starts_game    a valid map opens the game window with ?testMap=1, which hosts the map there.
  test_play_at_camera      right click on play (testMap(false, true)) starts the same game from the camera.
  test_templates_window    Templates (window 10): 29 KrunkScript tiles (v10's const _2189), the game
                           templates, the default maps tagged "Default Map", Import / Load / New / Guide.
  test_script_templates    the KrunkScript templates load: "2D Car Game" (no data before v10) and "Particle
                           Bursts" (new in v10) import; the default map Burg imports (loadQuick with a map).
  test_quick_start         Quick Start (toolbar / welcome button) imports the Quickstart game template.
  test_toolbar_menus       Game / Publish / Help menus in v10's order (IDX:14434-14552).
  test_welcome_text        Welcome (window 8): v10's text: no "earn real money", no "$ paid to developers",
                           no Scripting Discord link; the Mapping Community link stays.
  test_publishing_guide    Publishing Guide (window 11): the five v10 steps.
  test_publish_asset_uv    Publish > Asset: "Advanced: per-map UV channel" and its ten 0..7 channel rows; the
                           uploadA uvc is {slot: channel} for the channels 1..7, null when the box is off.
  test_teleport_popup      Teleport (explore button): X / Y / Z, Teleport and Reset.

Exit status 0 = all passed. Some checks need v10's text in ui/editor.html's windows; no account, no brain.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


class Ctx:
    pass


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def run_editor(c, name, script, args=("new",), timeout=180):
    """One headless editor run. Returns (log, out-dir). The screenshot ends the run once the script is done."""
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    cfg = os.path.join(c.tmp, name + "-cfg")
    os.makedirs(cfg, exist_ok=True)
    shot = os.path.join(c.tmp, name + ".png")
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
        "KRUNKNATIVE_EDITOR_SCRIPT": script + ";wait:0.5", "KRUNKNATIVE_SCREENSHOT": shot,
    })
    if c.egl:
        env["KRUNKNATIVE_EGL_DEVICE"] = c.egl
    p = subprocess.run([c.bin, "--editor"] + list(args), cwd=c.root, env=env, capture_output=True, text=True, timeout=timeout)
    log = p.stdout + p.stderr
    check(os.path.exists(shot), "%s: the run did not finish its script (rc %d):\n%s" % (name, p.returncode, log[-800:]))
    return log, c.tmp


def dump(c, name, kind, script_before):
    """Runs script_before then dumps `kind` ('window', 'popup', 'toolbar') to a file; returns its text."""
    path = os.path.join(c.tmp, "%s-%s.txt" % (name, kind))
    log, _ = run_editor(c, name, "%s;dump:%s:%s" % (script_before, kind, path))
    return read(path), log


def folder(text, header):
    """The entries of a toolbar folder ("M Game"): its "  O" options and "  -" rules, up to the next top-level item."""
    ls = text.splitlines()
    if header not in ls:
        return None
    out = []
    for l in ls[ls.index(header) + 1:]:
        if not l.startswith("  "):
            break
        out.append(l.strip())
    return out


@test
def test_play_validation(c):
    text, log = dump(c, "play-empty", "popup", "clear;ui:KE.testMap();wait:1")
    check("X Missing objects." in text, "an empty map must fail validateMap with \"Missing objects.\": %r" % text[:300])
    check("[app] new game window" not in log, "an invalid map must not start a game window")


@test
def test_play_starts_game(c):
    log, _ = run_editor(c, "play-ok", "clear;add:0;add:5;ui:KE.testMap();wait:3")
    check("[app] new game window ?testMap=1" in log, "Test Game opened no game window: %s" % log[-400:])
    check("test map hosted in this window" in log, "the game window did not host the test map: %s" % log[-400:])


@test
def test_play_at_camera(c):
    log, _ = run_editor(c, "play-cam", "clear;add:0;add:5;ui:KE.testMap(0,1);wait:3")
    check("[app] new game window ?testMap=1" in log, "right click on Test Game did not start a game: %s" % log[-400:])


@test
def test_templates_window(c):
    text, _ = dump(c, "templ", "window", "window:10;wait:1")
    check(text.startswith("H Templates"), "window 10 is not Templates: %r" % text[:80])
    lines = text.splitlines()
    check(lines.count("X KrunkScript") == 29, "KrunkScript tiles: %d, v10 has 29" % lines.count("X KrunkScript"))
    check(lines.count("X Default Map") == 32, "default maps tagged 'Default Map': %d, want 32" % lines.count("X Default Map"))
    for want in ("X Payments & Skins", "X Particle Bursts", "X Simple .io Game", "X Quickstart", "X Brawl", "X Burg", "X Load", "X Import", "X New", "X Guide"):
        check(want in lines, "Templates window lacks %r" % want)


@test
def test_script_templates(c):
    run_editor(c, "tpl-load", "ui:loadQuick(1,true);wait:1;objdump:%s/car.txt;clear;ui:loadQuick(18,true);wait:1;"
               "objdump:%s/bursts.txt;clear;ui:loadQuick(0,false,true);wait:2;objdump:%s/burg.txt" % (c.tmp, c.tmp, c.tmp))
    car = [l for l in read(os.path.join(c.tmp, "car.txt")).splitlines() if l.strip()]
    check(any(l.startswith("CUBE") and '"t":5' in l for l in car) and any(l.startswith("SPAWN_POINT") for l in car),
          "2D Car Game did not import (no data before v10): %s" % car[:4])
    bursts = [l for l in read(os.path.join(c.tmp, "bursts.txt")).splitlines() if l.strip()]
    check(len(bursts) >= 2, "Particle Bursts (new in v10) did not import: %s" % bursts[:4])
    burg = [l for l in read(os.path.join(c.tmp, "burg.txt")).splitlines() if l.strip()]
    check(len(burg) > 500, "the default map Burg did not import (%d objects)" % len(burg))


@test
def test_quick_start(c):
    run_editor(c, "quick", "ui:KE.quickStart();wait:1;objdump:%s/qs.txt" % c.tmp)
    qs = [l for l in read(os.path.join(c.tmp, "qs.txt")).splitlines() if l.strip()]
    check(any(l.startswith("SPAWN_POINT") for l in qs) and len(qs) >= 2, "Quick Start should import the Quickstart template: %s" % qs)


@test
def test_toolbar_menus(c):
    text, _ = dump(c, "menus", "toolbar", "window:1;closewindow;wait:0.2")
    check(folder(text, "M Game") == ["O Test Game", "O Host Game", "-", "O Quick Start", "O Templates", "-", "O Tools"],
          "Game menu: %s" % folder(text, "M Game"))
    check(folder(text, "M Publish") == ["O Game", "O Asset", "-", "O Publishing Guide"], "Publish menu: %s" % folder(text, "M Publish"))
    check((folder(text, "M Account") or [""])[0] == "O Login", "Account menu: %s" % folder(text, "M Account"))
    check(folder(text, "M Help") == ["O Controls", "O Guide"], "Help menu: %s" % folder(text, "M Help"))
    check("O Documentation" in text.splitlines(), "Documentation item missing")


@test
def test_welcome_text(c):
    text, _ = dump(c, "welcome", "window", "window:8;wait:1")
    check(text.startswith("H Welcome"), "window 8 is not Welcome: %r" % text[:80])
    for want in ("X Welcome to the Krunker Editor", "X Quick Start", "X My Games", "X Templates", "X Publishing", "X Documentation",
                 "X Mapping Community", "X Join Now!"):
        check(want in text, "Welcome lacks %r" % want)
    for gone in ("earn real money", "paid to developers", "monetization", "Scripting"):
        check(gone not in text, "Welcome still says %r (v10 has no such item)" % gone)


@test
def test_publishing_guide(c):
    text, _ = dump(c, "pubguide", "window", "window:11;wait:1")
    check(text.startswith("H Publishing"), "window 11 is not Publishing: %r" % text[:80])
    check("X Publishing Guide" in text and "X 5." in text and "Partner" in text, "Publishing Guide lacks its steps")


@test
def test_publish_asset_uv(c):
    # net:popup:publish-asset opens the asset popup with a fake signed-in account (no brain), as the windows tests do
    text, log = dump(c, "uv-on", "popup", "net:popup:publish-asset;wait:0.5;net:uv:on;net:uv:0:2;net:uv:9:4;net:uvc")
    check("Advanced: per-map UV channel" in text, "the asset popup lacks the per-map UV channel option")
    rows = [l for l in text.splitlines() if l.endswith("UV Channel")]
    check(len(rows) == 10, "UV channel rows: %d, v10 has 10: %s" % (len(rows), rows))
    check('[editor] uvc {"diffuse":2,"light":4}' in log, "uploadA's uvc for diffuse 2 / light 4: %s" % log[-500:])
    log2, _ = run_editor(c, "uv-off", "net:popup:publish-asset;wait:0.5;net:uvc")
    check("[editor] uvc null" in log2, "uploadA's uvc with the box off must be null")
    log3, _ = run_editor(c, "uv-range", "net:popup:publish-asset;wait:0.5;net:uv:on;net:uv:0:0;net:uv:1:8;net:uv:2:7;net:uvc")
    check('[editor] uvc {"normal":7}' in log3, "channels outside 1..7 must not be sent: %s" % log3[-300:])


@test
def test_teleport_popup(c):
    text, _ = dump(c, "tele", "popup", "teleport;wait:0.3")
    for want in ("Change Position", "Teleport", "Reset"):
        check(want in text, "Teleport popup lacks %r: %s" % (want, text[:300]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "cmake-build-debug", "client_bin"))
    ap.add_argument("-k", default="", help="run only the tests whose name contains this")
    args = ap.parse_args()
    c = Ctx()
    c.root = ROOT
    c.bin = os.path.abspath(args.bin)
    c.egl = os.environ.get("KRUNKNATIVE_EGL_DEVICE", "")
    if not os.path.exists(c.bin):
        print("publish_ui_e2e: %s not built: skipped" % c.bin)
        return 77
    os.makedirs(os.path.expanduser("~/.cache"), exist_ok=True)
    c.tmp = tempfile.mkdtemp(prefix="publish-ui-e2e-", dir=os.path.expanduser("~/.cache"))
    failed = 0
    try:
        for fn in TESTS:
            if args.k and args.k not in fn.__name__:
                continue
            try:
                fn(c)
                print("ok   %s" % fn.__name__, flush=True)
            except Exception as e:  # report every test
                failed += 1
                print("FAIL %s: %s" % (fn.__name__, e), flush=True)
    finally:
        shutil.rmtree(c.tmp, ignore_errors=True)
    total = len([f for f in TESTS if not args.k or args.k in f.__name__])
    print("%d/%d passed" % (total - failed, total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
