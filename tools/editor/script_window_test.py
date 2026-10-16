#!/usr/bin/env python3
"""KrunkScript window end-to-end test (doc/v7/editor.md WP5): headless `client_bin --editor new`
runs driven by KRUNKNATIVE_EDITOR_SCRIPT (script:..., dump:script:FILE, export:FILE).

    python3 -I tools/editor/script_window_test.py [--root DIR] [--bin client_bin] [--ksc ksc] [-k NAME]

1. parity: the window's dump in four states equals v7.1.4's (doc/ref/krunker-v7.1.4/editor/
   scripting*.dump.txt, tools/editor/v7ref.js on the recovered scripting.html); KrunkNative's own
   diagnostics list (D lines) is left out of the comparison.
2. diagnostics: the list for a broken script (client/tests/data/editor_script_bad.krnk) is what
   `ksc check` reports for it.
3. editing: closebrackets, Enter between {} (explode + indent), the hint pick, Validate;
   Shift-Tab (indentAuto), Tab over a selection, Ctrl-/ both ways, Backspace inside a pair.
4. storage: edited code (non-ASCII) exported as UTF-8 base64, the template omitted.
5. test play: Test drops the server script and keeps the client one; a client script that does
   not compile stops the test.
Every run is headless (EGL pbuffer, no DISPLAY) with a private XDG_CONFIG_HOME under a temp dir
in ~/.cache. Exit status 0 = all passed.
"""
import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
REF = os.path.join("doc", "ref", "krunker-v7.1.4", "editor")
BAD = os.path.join("client", "tests", "data", "editor_script_bad.krnk")


class Ctx:
    pass


def run_editor(c, script, name, timeout=90):
    """client_bin --editor new with KRUNKNATIVE_EDITOR_SCRIPT; returns (cfg dir, log text)."""
    cfg = os.path.join(c.tmp, name + "-cfg")
    shutil.rmtree(cfg, ignore_errors=True)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "KRUNKNATIVE_WINDOW_SIZE": "1920x937",
        "KRUNKNATIVE_EDITOR_SCRIPT": "closewindow;" + script,
        # the screenshot ends the run once the script is done (the page is "ready" then)
        "KRUNKNATIVE_SCREENSHOT": os.path.join(c.tmp, name + ".png"),
    })
    if c.egl:
        env["KRUNKNATIVE_EGL_DEVICE"] = c.egl
    p = subprocess.run([c.bin, "--editor", "new"], cwd=c.root, env=env, capture_output=True, text=True, timeout=timeout)
    return cfg, p.stdout + p.stderr


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def strip_kn(dump):
    return "".join(l + "\n" for l in dump.splitlines() if not l.startswith("D "))


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def out(c, name):
    return os.path.join(c.tmp, name)


# v10.0.2 also has a Shared tab (after Server; tools/editor/script_e2e.py covers it): every dump
# state gets its "T Shared" line here.
# v10.0.2 changed the KrunkScript templates (E10:11795-11799, tools/editor/kss_v10.json): the
# client one gained the three entity hooks, the server one shouldTrigger, onDepositBoxChange and
# the entity hooks. v7.1.4's dumps count the template lines ("C client lines=89"); the v10 count
# is the v7 one with the template's own line count, the only dump line the templates change.
V7_TEMPLATE_LINES = {"client": 89, "server": 69}


def v10_template_lines(side):
    with open(os.path.join(HERE, "kss_v10.json"), encoding="utf-8") as f:
        return json.load(f)["templates"][side].count("\n") + 1


def v10_dump(want, state):
    """v7.1.4's dump with the v10 template line count where the shown code is the template."""
    # v10.0.2's third tab (scripting.html: client, server, shared): not selected in these states
    want = "".join(l + "\n" + ("T Shared\n" if l.startswith("T Server") else "") for l in want.splitlines())
    side = "server" if state == "scripting-server" else "client"
    if state == "scripting-error":
        return want
    old = "C %s lines=%d " % (side, V7_TEMPLATE_LINES[side])
    if state == "scripting-hint":
        old = "C client lines=%d " % (V7_TEMPLATE_LINES["client"] + 1)  # one line added by the hint test
        new = "C client lines=%d " % (v10_template_lines("client") + 1)
    else:
        new = "C %s lines=%d " % (side, v10_template_lines(side))
    check(old in want, "v7 template line count in %s: %r" % (state, want))
    want = want.replace(old, new)
    if state == "scripting-hint":
        # the caret is at line 8 of the template: v10's copyright line (line 2) is 16 characters
        # shorter ("Yendis Entertainment Pty Ltd" -> "FRVR Limited"), so the selection moves with it
        want = want.replace("S 195 195", "S %d %d" % (195 - 16, 195 - 16))
    return want


@test
def parity(c):
    bad = os.path.join(c.root, BAD)
    runs = {
        "scripting": "script:open;dump:script:%s",
        "scripting-server": "script:open;script:tab:server;wait:0.8;dump:script:%s",
        "scripting-error": "script:open;script:load:" + bad + ";script:validate;dump:script:%s",
        # v7ref: replaceRange("\n") at line 8, then "\tGAME.PLAYERS.ge" at its start
        "scripting-hint": "script:open;script:caret:8:1;script:insert:\\n;script:caret:8:1;script:insert:\\tGAME.PLAYERS.ge;dump:script:%s",
    }
    for state, script in runs.items():
        ref = os.path.join(c.root, REF, state + ".dump.txt")
        check(os.path.exists(ref), "missing reference %s (node tools/editor/v7ref.js %s)" % (ref, state))
        _, log = run_editor(c, script % out(c, state + ".txt"), state)
        got = strip_kn(read(out(c, state + ".txt")))
        want = v10_dump(read(ref), state)
        check(got == want, "%s differs from v7.1.4:\n--- v7\n%s--- native\n%s(log tail: %s)" % (state, want, got, log[-300:]))


@test
def diagnostics_match_ksc(c):
    bad = os.path.join(c.root, BAD)
    # the live check runs 300 ms after the last edit
    run_editor(c, "script:open;script:load:%s;wait:1;dump:script:%s" % (bad, out(c, "diag.txt")), "diag")
    d = [l[2:] for l in read(out(c, "diag.txt")).splitlines() if l.startswith("D ")]
    k = json.loads(subprocess.run([c.ksc, "check", "--json", "--side", "client", bad], capture_output=True, text=True).stdout)
    s = k["scripts"][0]
    want = ["error %d:%d %s" % (e["line"], e["col"], e["message"]) for e in s["errors"]]
    want += ["warning %d:%d %s" % (e["line"], e["col"], e["message"]) for e in s["warnings"]]
    check(d == want, "window diagnostics %s != ksc %s" % (d, want))
    check(any(l == "E %d" % s["errors"][0]["line"] for l in read(out(c, "diag.txt")).splitlines()), "the error line is marked")


@test
def editing(c):
    # Enter at the end of a { line indents; ( and { pair up; ) types over the closer; Enter
    # between {} explodes; the hint list picks GAME.log(...args)
    src = os.path.join(c.tmp, "edit.krnk")
    with open(src, "w") as f:
        f.write("public action start() {\n\t\n}\n")
    script = ("script:open;script:load:%s;script:caret:2:2;script:type:if (x) {\\nGAME.lo;dump:script:%s;script:hint:0;"
              "script:text:%s;script:validate;dump:script:%s") % (src, out(c, "edit-hint.txt"), out(c, "edit.out"), out(c, "edit-val.txt"))
    run_editor(c, script, "edit")
    check(read(out(c, "edit.out")) == "public action start() {\n\tif (x) {\n\t\tGAME.log(...args)\n\t}\n}\n",
          "typed text: %r" % read(out(c, "edit.out")))
    check("K * GAME.log(...args) | action()" in read(out(c, "edit-hint.txt")), "hint list for GAME.lo")
    v = read(out(c, "edit-val.txt"))
    check("B Failed - errors found!" in v and "T Client * [E]" in v, "Validate with an error: %s" % v)


@test
def editing_keys(c):
    # Shift-Tab (indentAuto) over everything, Tab with a selection, Ctrl-/ twice, Backspace
    # between a pair
    src = os.path.join(c.tmp, "keys.krnk")
    text = "public action start() {\nif (a) {\nb();\n}\n}\n"
    with open(src, "w") as f:
        f.write(text)
    n = len(text)
    script = ("script:open;script:load:%s;script:select:0:%d;script:key:shift+tab;script:text:%s;"
              "script:select:0:%d;script:key:tab;script:text:%s;script:key:shift+tab;"
              "script:select:0:0;script:key:ctrl+/;script:text:%s;script:key:ctrl+/;script:text:%s;"
              "script:caret:3:7;script:type:(;script:key:backspace;script:text:%s") % (
        src, n, out(c, "k1"), n + 6, out(c, "k2"), out(c, "k3"), out(c, "k4"), out(c, "k5"))
    run_editor(c, script, "keys")
    smart = "public action start() {\n\tif (a) {\n\t\tb();\n\t}\n}\n"
    check(read(out(c, "k1")) == smart, "Shift-Tab re-indents by brackets: %r" % read(out(c, "k1")))
    check(read(out(c, "k2")) == "".join("\t" + l + "\n" for l in smart.splitlines()), "Tab indents every selected line: %r" % read(out(c, "k2")))
    check(read(out(c, "k3")) == "# " + smart, "Ctrl-/ comments the line: %r" % read(out(c, "k3")))
    check(read(out(c, "k4")) == smart, "Ctrl-/ again uncomments it")
    check(read(out(c, "k5")) == smart, "Backspace inside () deletes the pair: %r" % read(out(c, "k5")))


@test
def storage_round_trip(c):
    code = "# héllo ✓\npublic action start() {\n\tGAME.log(\"ü\");\n}\n"
    src = os.path.join(c.tmp, "utf8.krnk")
    with open(src, "w", encoding="utf-8") as f:
        f.write(code)
    run_editor(c, "script:open;script:load:%s;script:validate;script:flush;export:%s" % (src, out(c, "utf8.json")), "utf8")
    m = json.loads(read(out(c, "utf8.json")))
    check(base64.b64decode(m["scripts"]["client"]).decode("utf-8") == code, "client script exported as UTF-8 base64")
    check("server" not in m["scripts"], "the untouched server template is not exported")


@test
def test_play(c):
    good = os.path.join(c.tmp, "good.krnk")
    with open(good, "w") as f:
        f.write("public action start() {\n\tGAME.log(\"hi\");\n}\n")
    srv = os.path.join(c.tmp, "srv.krnk")
    with open(srv, "w") as f:
        f.write("public action start() {\n}\n")
    cfg, _ = run_editor(c, "script:open;script:load:%s;script:tab:server;script:load:%s;ui:KE.testMap();wait:1" % (good, srv), "play")
    cm = json.loads(read(os.path.join(cfg, "krunknative", "custMap.json")))
    check("client" in cm.get("scripts", {}) and "server" not in cm["scripts"], "Test keeps the client script only: %s" % cm.get("scripts"))
    bad = os.path.join(c.root, BAD)
    cfg, log = run_editor(c, "script:open;script:load:%s;ui:KE.testMap();wait:1;dump:script:%s" % (bad, out(c, "play-bad.txt")), "play-bad")
    check(not os.path.exists(os.path.join(cfg, "krunknative", "custMap.json")), "a broken client script stops the test")
    check("Client Script: 5:1" in log, "the compile error is reported")
    check("E 5" in read(out(c, "play-bad.txt")), "the window shows the error line")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--bin", default=None)
    ap.add_argument("--ksc", default=None)
    ap.add_argument("--egl", default=os.environ.get("KRUNKNATIVE_EGL_DEVICE"), help="KRUNKNATIVE_EGL_DEVICE")
    ap.add_argument("-k", default=None)
    a = ap.parse_args()
    c = Ctx()
    c.root = os.path.abspath(a.root)
    c.bin = a.bin or os.path.join(c.root, "cmake-build-debug", "client_bin")
    c.ksc = a.ksc or os.path.join(os.path.dirname(c.bin), "ksc")
    c.egl = a.egl
    if not os.path.isfile(os.path.join(c.root, "assets", "css", "fonts", "font2.ttf")) or not os.access(c.bin, os.X_OK):
        print("skipped: client_bin or the game content (assets/css/fonts) missing")
        return 0
    base = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "krunknative")
    os.makedirs(base, exist_ok=True)
    c.tmp = tempfile.mkdtemp(prefix="script-window-test-", dir=base)
    failed = 0
    order = [t for t in TESTS if not a.k or a.k in t.__name__]
    for t in order:
        try:
            t(c)
            print("ok   %s" % t.__name__)
        except Exception as e:
            failed += 1
            print("FAIL %s: %s" % (t.__name__, e))
    print("%d/%d passed (tmp %s)" % (len(order) - failed, len(order), c.tmp))
    if not failed:
        shutil.rmtree(c.tmp, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
