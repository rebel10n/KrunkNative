#!/usr/bin/env python3
"""Tests for tools/strike/extract_strike.py (module S0 strike-ref).

  1. safety: the extractor never runs anything: no subprocess / os.system / exec family, no
     eval / exec / compile / __import__, no network modules, and no `node` anywhere in its code
     (checked on the AST and on the code tokens, comments and docstrings excluded)
  2. golden data in the committed tools/strike/strike_data.json: the 7-map pool, the six-class
     set, the packet names of STRIKE-REWRITE-PLAN section 2, known Strike-vs-v10 deltas
  3. when the sources are present (the local 1.37.2 build and the S0 archive mirror): two
     extractions give byte-identical output, and it equals the committed strike_data.json

usage: python3 -I tools/strike/test_extract.py [--build DIR] [--live DIR]
Exit 0 on success; section 3 is skipped (reported) when the sources are not on this machine.
"""

import argparse
import ast
import contextlib
import io
import json
import os
import re
import sys
import tokenize

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "extract_strike.py")
DATA = os.path.join(HERE, "strike_data.json")

FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print("FAIL: " + msg)


# ------------------------------------------------------------------------------- 1. safety

BANNED_MODULES = {"subprocess", "socket", "urllib", "http", "ctypes", "multiprocessing", "pty",
                  "requests", "shutil", "webbrowser", "asyncio"}
BANNED_CALLS = {"eval", "exec", "compile", "__import__", "execfile", "breakpoint"}
BANNED_ATTRS = {"system", "popen", "spawnl", "spawnv", "spawnve", "spawnlp", "execv", "execve",
                "execl", "execlp", "execvp", "fork", "startfile", "run", "Popen", "check_output",
                "call"}


def test_safety():
    with open(SCRIPT, encoding="utf-8") as f:
        src = f.read()
    tree = ast.parse(src)
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                check(a.name.split(".")[0] not in BANNED_MODULES, "imports %s" % a.name)
        elif isinstance(node, ast.ImportFrom):
            check((node.module or "").split(".")[0] not in BANNED_MODULES, "imports from %s" % node.module)
        elif isinstance(node, ast.Call):
            fn = node.func
            if isinstance(fn, ast.Name):
                check(fn.id not in BANNED_CALLS, "calls %s()" % fn.id)
            elif isinstance(fn, ast.Attribute):
                base = fn.value.id if isinstance(fn.value, ast.Name) else None
                check(not (base == "os" and fn.attr in BANNED_ATTRS), "calls os.%s()" % fn.attr)
                check(fn.attr not in ("Popen", "check_output", "system", "popen"), "calls .%s()" % fn.attr)
    # code tokens only (comments and the docstrings are prose about what the tool does NOT do)
    code = []
    toks = tokenize.generate_tokens(io.StringIO(src).readline)
    prev = None
    for t in toks:
        if t.type == tokenize.COMMENT:
            continue
        if t.type == tokenize.STRING and (prev is None or prev.type in (tokenize.INDENT, tokenize.NEWLINE,
                                                                         tokenize.NL, tokenize.DEDENT)):
            prev = t
            continue  # a docstring / bare string statement
        code.append(t.string)
        if t.type not in (tokenize.NL, tokenize.COMMENT):
            prev = t
    text = " ".join(code)
    for word in (r"\bnode(?:js)?\b", r"subprocess", r"\beval\b", r"\bexec\b", r"os\.system", r"popen",
                 r"urlopen", r"\bsocket\b"):
        check(not re.search(word, text, re.I), "extractor code mentions %s" % word)
    print("safety: checked %d AST nodes, %d code tokens" % (sum(1 for _ in ast.walk(tree)), len(code)))


# ------------------------------------------------------------------------------- 2. golden data

GOLDEN_POOL = ["burg", "habitat", "lostworld", "oasis", "sandstorm", "skytemple", "subzero"]
GOLDEN_CLASSES = [(0, "Triggerman"), (1, "Hunter"), (2, "Run N Gun"), (4, "Shotgunner"),
                  (5, "Detective"), (7, "Rocketeer")]
GOLDEN_S2C = {"RPC_SUCCESS": "_0", "RPC_ERROR": "_1", "INIT_GAME": "init", "GAME_INSTANCE_ID": "inst-id",
              "INIT_LOAD": "load", "READY_LOAD": "ready", "CUSTOM_RESP": "cust",
              "UPDATE_CODE_TIMER": "codeT", "CODE_PASSED": "codeP", "CAPTCHA_REQUEST": "cpt"}
GOLDEN_C2S = {"RPC": "_0", "VALIDATE_FRVR_AUTH_TOKEN": "_1", "ENTER_GAME": "_2",
              "CAPTCHA_RESPONSE": "cptR", "LOAD": "load"}
GOLDEN_LITERALS = ["cpt", "cptR", "io-init"]


def test_golden(d):
    s = d["summary"]
    check(d["meta"]["game_version"] == "1.37.2", "primary build is %s, not 1.37.2" % d["meta"]["game_version"])
    check(s["map_pool"] == GOLDEN_POOL, "map pool %r" % s["map_pool"])
    check(len(d["maps"]["pool"]) == 7, "pool has %d maps" % len(d["maps"]["pool"]))
    check(all(p["catalogue_index"] is not None and p["build_file"] for p in d["maps"]["pool"]),
          "a pool map is not in the catalogue / build")
    check([(c["id"], c["name"]) for c in d["classes"]] == GOLDEN_CLASSES,
          "classes %r" % [(c["id"], c["name"]) for c in d["classes"]])
    for k, v in GOLDEN_S2C.items():
        check(d["packets"]["server_to_client"].get(k) == v, "server->client %s=%r" % (k, v))
    for k, v in GOLDEN_C2S.items():
        check(d["packets"]["client_to_server"].get(k) == v, "client->server %s=%r" % (k, v))
    check(d["packets"]["literal_names"] == GOLDEN_LITERALS, "literal frame names %r" % d["packets"]["literal_names"])
    w = d["packets"]["wire"]
    check(w.get("trailer_bytes") == 2 and w.get("ahnum_step") == 1 and w.get("ahnum_skip") == "cptR",
          "wire framing %r" % w)
    check(s["modes"] == 35 and d["modes"]["list"][0]["id"] == "ffa" and d["modes"]["list"][0]["scoreLimit"] == 1500,
          "modes")
    check(s["weapons"] == 29 and d["weapons"][1]["name"] == "Assault Rifle", "weapon table")
    ar = d["weapons"][1]["data"]
    check(ar.get("dmg") == 20 and ar.get("ammo") == 35, "Strike Assault Rifle dmg/ammo %r/%r" % (ar.get("dmg"), ar.get("ammo")))
    arv = d["weapons_vs_v10"]["per_weapon"][1]["changed"]
    check(arv.get("dmg") == {"strike": 20, "v10": 23}, "AR dmg vs v10 %r" % arv.get("dmg"))
    tm = d["classes"][0]
    check(tm["health"] == 180 and tm["loadout"] == [1], "Triggerman health/loadout")
    check(d["config"]["constants"]["maxPlayers"].get("value") == 10, "client default maxPlayers")
    check(d["config"]["player_cap"]["discord_activity_max_participants"] == 12, "Discord cap")
    check(d["config"]["player_cap"]["private_match_capacity"] == 12, "private match capacity")
    routes = {(r["method"], r["path"]) for r in d["gateway"]["routes"]}
    for r in (("GET", "/v1/game-config"), ("GET", "/v1/item-definitions/all"), ("POST", "/v1/shop/purchase"),
              ("GET", "/v1/missions/me"), ("POST", "/v1/seasonal-events/progression/claim"),
              ("GET", "/v1/players/me/classes")):
        check(r in routes, "gateway route %s %s" % r)
    check(set(d["gateway"]["directors"]) >= {"usc1a", "euw3a", "sae1a", "sea1a"}, "director regions")
    check(s["i18n_strings"] > 1500 and d["i18n"].get("menu.btn.customize") == "Customize", "i18n table")
    ids = {x["id"] for x in d["ui"]["screens"]}
    check({"MAIN_MENU", "CLASS_SELECTION", "STORE", "SETTINGS", "IN_GAME_UI", "TAP_TO_PLAY"} <= ids, "screens")
    mob = d["ui"]["mobile"]
    check(mob["detect"]["global"] == "window._isMobile" and mob["detect"]["user_agent_regexes"]
          and mob["frvr_menu_constant"] is True, "mobile detection")
    check(any(f["family"] == "ChakraPetch" for f in d["ui"]["css"]["build"]["fonts"]), "ChakraPetch font")
    check("loading-screen__curtains" in (d["ui"]["entries"]["build"]["body_markup"] or ""), "loading screen")
    # gameplay-parity sections (strike-gameplay2): physics, combat, input
    ph = d["physics"]
    check(ph["jumpVel"] == 0.072 and ph["gravity"] == 0.00015 and ph["playerSpeed"] == 0.00042, "physics movement constants")
    check(abs(ph["crouchLean"] + 0.1 * 3.141592653589793) < 1e-9, "crouchLean is v7's -(PI * 0.1): %r" % ph["crouchLean"])
    check(ph["fov"] == 50 and ph["mouseSens"] == 0.0024 and ph["dltMx"] == 33, "fov / mouseSens / dltMx")
    cb = d["combat"]
    check(cb["hit_test"] == "aabb" and not any(cb["posed_markers"].values()), "combat: v7 AABB hit test, no posed boxes")
    inp = d["input"]
    binds = {(b["bind"], b["index"]): (b["def"], b["alt"]) for b in inp["binds"]}
    check(binds.get(("aim", -1)) == (10003, 16) and binds.get(("shoot", -1)) == (10001, 220), "aim / shoot binds")
    check(("swap", -1) not in binds and ("reset", -1) not in binds, "Strike has no swap / reset bind")
    check(inp["scroll_to_swap"] is False, "scroll never swaps weapons")
    st = inp["settings"]
    check(st["fov"]["val"] == 70 and st["fpsFOV"]["val"] == 65, "fov defaults %r %r" % (st["fov"]["val"], st["fpsFOV"]["val"]))
    check(st["sensitivityXMbl"]["val"] == 1.4 and st["sensitivityXMbl"]["store_mlt"] == 4, "mobile sensitivity")
    check(st["autoFireMbl"]["val"] is True and st["aimAssist"]["val"] is True, "auto-fire / aim assist on by default")
    aa = inp["aim_assist"]
    check(aa["pull"]["pull_initial"] == 2 and aa["pull"]["pull_locked"] == 3 and aa["pull"]["locked_look_mlt"] == 0.75
          and aa["box"]["grow_width"] == 1.5 and aa["box"]["grow_height"] == 1.1 and aa["autofire"]["buffer_ms"] == 260,
          "aim assist constants %r" % aa)
    check(abs(inp["controls"]["pitch_limit"] - 3.141592653589793 / 2) < 1e-9, "pitch limit PI/2")
    t = inp["touch"]
    check(t["base_width"] == 640 and t["base_height"] == 360 and t["move_joystick"]["size"] == 106
          and t["aim_zone"]["tap_jump_max_travel_px"] == 9, "touch layout base")
    print("golden: %s" % json.dumps(s, ensure_ascii=False))


# ------------------------------------------------------------------------------- 3. determinism

def run_print(build, live):
    sys.path.insert(0, HERE)
    try:
        import extract_strike
    finally:
        sys.path.pop(0)
    extract_strike._ASSIGN_INDEX.clear()
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = extract_strike.main(["--build", build, "--live", live, "--print"])
    return rc, buf.getvalue()


def test_determinism(build, live):
    if not (os.path.isdir(build) and os.path.isdir(live)):
        print("determinism: SKIPPED (sources not here: %s, %s)" % (build, live))
        return
    rc1, a = run_print(build, live)
    rc2, b = run_print(build, live)
    check(rc1 == 0 and rc2 == 0, "extractor exit codes %d %d" % (rc1, rc2))
    check(a == b, "two runs differ")
    with open(DATA, encoding="utf-8") as f:
        cur = f.read()
    check(a == cur, "committed strike_data.json is stale (rerun extract_strike.py)")
    print("determinism: 2 runs, %d bytes, identical%s" % (len(a), "" if a == cur else " (but stale)"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.expanduser("~/src/KrunkerStrike_v1.37.2/public"))
    ap.add_argument("--live", default=os.path.expanduser("~/.cache/kn-strike/live"))
    a = ap.parse_args()
    test_safety()
    with open(DATA, encoding="utf-8") as f:
        test_golden(json.load(f))
    test_determinism(a.build, a.live)
    if FAILS:
        print("%d failure(s)" % len(FAILS))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
