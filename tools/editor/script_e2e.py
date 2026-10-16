#!/usr/bin/env python3
"""KrunkScript window, Krunker v10.0.2 parity (doc/v10/editor-social.md 1.5): the v10 autocomplete
entries, the members they name are accepted by the checker, and the window's validation of them.
Runs on the same headless `client_bin --editor new` as script_window_test.py.

    python3 -I tools/editor/script_e2e.py [--bin client_bin] [-k NAME]

1. api: a script that calls v10-only GAME members (ENTITIES, LIVE_OBJECTS, PAYMENTS.giveCurrency,
   AI.removeBySid, OBJECTS.getByInterface, ...) validates in the window ("Success!"), as v10's
   compiler accepts them; a v7 member that does not exist still fails.
2. hints: the autocomplete lists the v10 entries (text and type as v10's list), e.g.
   "GAME.ENTITIES.list() | obj[] action()".
3. shared: the Shared tab (v10.0.2's third tab) shows the v10 template (16 lines) and its own
   hints (sharedAutoComplete); its checker takes GAME.UTILS and the entity hooks only; the
   script round-trips through the map's "scripts.shared" (base64, the template omitted); Test
   play validates it and keeps it in the test map, and a broken one stops the test.
Exit status 0 = all passed.
"""
import argparse
import base64
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import script_window_test as sw  # noqa: E402  (run_editor, read, check, Ctx: the same harness)

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def write(c, name, text):
    path = os.path.join(c.tmp, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


@test
def api(c):
    body = "\n".join("\t" + s for s in [
        "obj[] list = GAME.ENTITIES.list();",
        "obj ent = GAME.ENTITIES.spawn(\"crate\", 0, 10, 0, {});",
        "GAME.ENTITIES.despawn(\"id\");",
        "GAME.LIVE_OBJECTS.addCube(0, 0, 0, 1, 1, 1, 0, {});",
        "GAME.PAYMENTS.giveCurrency(\"p\", \"coins\", 5, 1);",
        "GAME.OBJECTS.getByInterface(3);",
        "GAME.AI.removeBySid(4);",
        "GAME.COOKIES.removeByKey(\"k\");",
    ]) + "\n"
    src = write(c, "v10api.krnk", "public action start() {\n" + body + "}\n")
    run_out = sw.out(c, "v10api.txt")
    sw.run_editor(c, "script:open;script:load:%s;script:validate;dump:script:%s" % (src, run_out), "v10api")
    d = sw.read(run_out)
    sw.check("B Success!" in d and "T Client * [V]" in d, "v10 members validate: %s" % d)
    bad = write(c, "v7bad.krnk", "public action start() {\n\tGAME.ENTITY.list();\n}\n")
    run_out = sw.out(c, "v7bad.txt")
    sw.run_editor(c, "script:open;script:load:%s;script:validate;dump:script:%s" % (bad, run_out), "v7bad")
    sw.check("B Failed - errors found!" in sw.read(run_out), "an unknown GAME member still fails")


@test
def hints(c):
    src = write(c, "hints.krnk", "public action start() {\n\t\n}\n")
    out_h = sw.out(c, "hints-ent.txt")
    sw.run_editor(c, "script:open;script:load:%s;script:caret:2:2;script:type:GAME.ENTITIES.l;dump:script:%s" % (src, out_h), "hints-ent")
    d = sw.read(out_h)
    sw.check("K * GAME.ENTITIES.list() | obj[] action()" in d, "the v10 ENTITIES entry is hinted: %s" % d)
    out_p = sw.out(c, "hints-pay.txt")
    sw.run_editor(c, "script:open;script:load:%s;script:caret:2:2;script:type:GAME.PAYMENTS.giveC;dump:script:%s" % (src, out_p), "hints-pay")
    sw.check("K * GAME.PAYMENTS.giveCurrency(player, currencyName, val, callb) | action()" in sw.read(out_p),
             "the v10 PAYMENTS entry is hinted: %s" % sw.read(out_p))


@test
def shared_tab(c):
    run_out = sw.out(c, "shared-tpl.txt")
    sw.run_editor(c, "script:open;script:tab:shared;dump:script:%s" % run_out, "shared-tpl")
    d = sw.read(run_out)
    sw.check("T Shared *" in d and "C shared lines=16 font=13px" in d, "the Shared tab shows the v10 template: %s" % d)
    # its hints are sharedAutoComplete (GAME.UTILS only): GAME.PLAYERS is not offered, GAME.UTILS.get is
    src = write(c, "shared-hint.krnk", "public action start() {\n\t\n}\n")
    h_out = sw.out(c, "shared-hint.txt")
    sw.run_editor(c, "script:open;script:tab:shared;script:load:%s;script:caret:2:2;script:type:GAME.PLAYERS.ge;dump:script:%s" % (src, h_out), "shared-hint-none")
    sw.check(not any(l.startswith("K ") for l in sw.read(h_out).splitlines()), "no GAME.PLAYERS hints on the shared tab")
    h_out = sw.out(c, "shared-hint2.txt")
    sw.run_editor(c, "script:open;script:tab:shared;script:load:%s;script:caret:2:2;script:type:GAME.UTILS.getDi;dump:script:%s" % (src, h_out), "shared-hint-utils")
    sw.check("K * GAME.UTILS.getDir2D(x, y, x2, y2) | num action()" in sw.read(h_out), "GAME.UTILS hints on the shared tab: %s" % sw.read(h_out))
    # the checker: GAME.UTILS and the two entity hooks, nothing else of the API
    ok = write(c, "shared-ok.krnk", "public obj action onEntityInit(str id, str archetype, obj spawn) {\n"
                                    "    obj state = {};\n    state.d = GAME.UTILS.getDist2D(1, 2, 3, 4);\n    return state;\n}\n"
                                    "public obj action simulateEntity(str id, str archetype, obj state, obj ctx, num delta) {\n"
                                    "    return state;\n}\n")
    v = sw.out(c, "shared-ok.txt")
    sw.run_editor(c, "script:open;script:tab:shared;script:load:%s;script:validate;dump:script:%s" % (ok, v), "shared-ok")
    sw.check("B Success!" in sw.read(v) and "T Shared * [V]" in sw.read(v), "a shared script with UTILS and the entity hooks checks: %s" % sw.read(v))
    bad = write(c, "shared-bad.krnk", "public action start() {\n\tGAME.CHAT.send(1, \"x\", 1);\n}\n")
    v = sw.out(c, "shared-bad.txt")
    sw.run_editor(c, "script:open;script:tab:shared;script:load:%s;script:validate;dump:script:%s" % (bad, v), "shared-bad")
    sw.check("B Failed - errors found!" in sw.read(v) and 'Unknown member "GAME.CHAT".' in sw.read(v),
             "the shared side has no GAME.CHAT: %s" % sw.read(v))
    arity = write(c, "shared-hook.krnk", "public action onEntityInit(str a, str b, obj c, num d) {\n}\n")
    v = sw.out(c, "shared-hook.txt")
    sw.run_editor(c, "script:open;script:tab:shared;script:load:%s;script:validate;dump:script:%s" % (arity, v), "shared-hook")
    sw.check("Hook \"onEntityInit\" takes at most 3 parameters." in sw.read(v), "the entity hook arity: %s" % sw.read(v))


@test
def shared_storage(c):
    code = "public obj action onEntityInit(str id, str archetype, obj spawn) {\n\tobj s = {};\n\treturn s;\n}\n"
    src = write(c, "shared-store.krnk", code)
    out_map = sw.out(c, "shared-store.json")
    sw.run_editor(c, "script:open;script:tab:shared;script:load:%s;script:flush;export:%s" % (src, out_map), "shared-store")
    m = json.loads(sw.read(out_map))
    sw.check(base64.b64decode(m["scripts"]["shared"]).decode("utf-8") == code, "shared exported as base64: %s" % m.get("scripts"))
    sw.check("client" not in m["scripts"] and "server" not in m["scripts"], "untouched client and server templates are not exported")


@test
def shared_test_play(c):
    # Test Game validates the map first (v10's validateMap: "Missing objects." for an empty map)
    good = write(c, "play-client.krnk", "public action start() {\n\tGAME.log(\"hi\");\n}\n")
    sh = write(c, "play-shared.krnk", "public obj action onEntityInit(str id, str archetype, obj spawn) {\n\tobj s = {};\n\treturn s;\n}\n")
    cfg, _ = sw.run_editor(c, "add:0;add:5;script:open;script:load:%s;script:tab:shared;script:load:%s;ui:KE.testMap();wait:1" % (good, sh), "play-shared")
    cm = json.loads(sw.read(os.path.join(cfg, "krunknative", "custMap.json")))
    sw.check("shared" in cm.get("scripts", {}) and "client" in cm["scripts"], "Test keeps the shared script: %s" % cm.get("scripts"))
    broken = write(c, "play-shared-bad.krnk", "public obj action onEntityInit(str id, str archetype, obj spawn) {\n\tnum x = \"a\";\n\treturn {};\n}\n")
    cfg, log = sw.run_editor(c, "add:0;add:5;script:open;script:load:%s;script:tab:shared;script:load:%s;ui:KE.testMap();wait:1" % (good, broken), "play-shared-bad")
    sw.check(not os.path.exists(os.path.join(cfg, "krunknative", "custMap.json")), "a broken shared script stops the test")
    sw.check("Shared Script: " in log, "the shared compile error is reported")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=None)
    ap.add_argument("-k", default=None)
    a = ap.parse_args()
    c = sw.Ctx()
    c.root = os.path.dirname(os.path.dirname(HERE))
    c.bin = a.bin or os.path.join(c.root, "cmake-build-debug", "client_bin")
    c.ksc = os.path.join(os.path.dirname(c.bin), "ksc")
    c.egl = os.environ.get("KRUNKNATIVE_EGL_DEVICE")
    if not os.path.isfile(os.path.join(c.root, "assets", "css", "fonts", "font2.ttf")) or not os.access(c.bin, os.X_OK):
        print("skipped: client_bin or the game content (assets/css/fonts) missing")
        return 0
    base = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "krunknative")
    os.makedirs(base, exist_ok=True)
    c.tmp = tempfile.mkdtemp(prefix="script-e2e-", dir=base)
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
