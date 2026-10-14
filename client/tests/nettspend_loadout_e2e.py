#!/usr/bin/env python3
"""The Nettspend class in the menu and its sounds in the game (client/src/ui/ui_loadout.c,
client/src/nettspend_audio.c, sound_events.c):

  python3 -I client/tests/nettspend_loadout_e2e.py BUILD_DIR      (run from the repository root)

An offline Kanji session (headless client_bin, input script, fixed 60 fps step, miniaudio's null
backend with KRUNKNATIVE_AUDIO_LOG so every play is logged):
  1. Change Loadout > Class: a "Nettspend" class card is listed (the last card, after Krunker's
     classes) and names Purple Drank as its weapon;
  2. clicking it opens the Loadout tab with Purple Drank as the primary and Money Spread as the
     secondary; the secondary is the class's own (no Select Secondary button on its tile);
  3. the live class preview holds the Purple Drank (wsel: the preview's slot 0 is the Drank);
  4. in the game the class spawns with the Drank in hand and Money Spread as the secondary;
  5. firing the Drank plays the stream splash (kn_drank_splash) and not the pour; a reload plays the
     pour up (kn_drank_pour once + kn_drank_reload) and not Krunker's reload_1 / reload_2; a Money
     Spread shot plays kn_money_throw.
The weapon ids are read from shared/include/weapons.h (WEAPON_ID_DRANK / WEAPON_ID_MONEY_SPREAD).
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

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-nettspend-loadout-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def weapon_ids():
    """WEAPON_ID_DRANK / WEAPON_ID_MONEY_SPREAD as shared/include/weapons.h defines them: KN_BASE + offset."""
    ids = {}
    for path in ("shared/include/weapons.h",):
        try:
            src = open(path).read()
        except OSError:
            continue
        for name in ("WEAPON_ID_DRANK", "WEAPON_ID_MONEY_SPREAD"):
            m = re.search(r"#define %s \(WEAPON_ID_KN_BASE \+ (\d+)\)" % name, src)
            if m and name not in ids:
                ids[name] = 64 + int(m.group(1))
    return ids.get("WEAPON_ID_DRANK", 67), ids.get("WEAPON_ID_MONEY_SPREAD", 68)


DRANK, MONEY = weapon_ids()

MENU_SCRIPT = """0 menu
1500 call showWindow 3
2500 clicksel [onclick*="tabIndex=0"]
3500 rects @cards .classCard
3550 text @lastname .classCard:last-child .className
3560 text @lastweap .classCard:last-child .classWeap
3600 clicksel .classCard[onclick*="selectClass"]
4800 text @prim .custContainer[onclick*="skinSelector"] .settLabel
4810 text @sec .custContainer:not([onclick]) .settLabel
4820 nodes @secpick .custContainer[onclick*="showWindow(20)"]
4830 wsel @menu
5200 quit
"""

GAME_SCRIPT = """0 menu
1500 call showWindow 3
2500 clicksel [onclick*="tabIndex=0"]
3500 clicksel .classCard[onclick*="selectClass"]
4500 play
6000 state
6100 pose -343 -143 -28 0 0
6500 mdown left
6800 mup left
7000 state
7100 down R
7150 up R
7400 state
8900 state
9000 down E
9050 up E
9600 state
9700 mdown left
9800 mup left
10000 state
10600 quit
"""


def run(tag, script_text, timeout=240):
    work = os.path.join(WORK, tag)
    cfg = os.path.join(work, "cfg")
    os.makedirs(cfg)
    script = os.path.join(work, "run.script")
    with open(script, "w") as f:
        f.write(script_text)
    log = os.path.join(work, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(work, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS), "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_AUDIO_LOG": "1",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{tag}: client_bin finished its script", False, f"timeout, log {log}")
            return ""
    with open(log, errors="replace") as f:
        return f.read()


def text_of(log, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\S+ value=(.*?) text=(.*)" % re.escape(label), log)
    return (int(m.group(1)), m.group(3).strip()) if m else (0, None)


def states(log):
    return re.findall(r"\[input_script\] state t=(\d+) slot=(\d+)/(\d+) weapon=(.*?) menu=.*? ammo=(-?\d+) .*? reload=([\d.]+) ", log)


def menu_scenario():
    log = run("menu", MENU_SCRIPT)
    m = re.search(r"\[input_script\] rects t=\d+ @cards n=(\d+)", log)
    n_cards = int(m.group(1)) if m else 0
    check("the Class tab lists Krunker's classes plus Nettspend", n_cards >= 17, f"cards={n_cards}")
    n, name = text_of(log, "lastname")
    check("the last class card is Nettspend", n >= 1 and name == "Nettspend", f"n={n} text={name!r}")
    n, weap = text_of(log, "lastweap")
    check("its card names Purple Drank", n >= 1 and weap == "Purple Drank", f"text={weap!r}")
    n, prim = text_of(log, "prim")
    check("Loadout tab after the pick: the primary is Purple Drank", n >= 1 and prim == "Purple Drank", f"n={n} text={prim!r}")
    n, sec = text_of(log, "sec")
    check("the secondary tile shows Money Spread", n >= 1 and sec == "Money Spread", f"n={n} text={sec!r}")
    m = re.search(r"\[input_script\] nodes t=\d+ @secpick elements=\d+ match=(\d+)", log)
    check("the class's secondary has no Select Secondary button", bool(m) and m.group(1) == "0", m.group(0) if m else "no nodes line")
    m = re.search(r"\[input_script\] wsel t=\d+ @menu who=preview slot=0 .*?\bw=(\d+)", log)
    check("the live class preview holds the Purple Drank", bool(m) and int(m.group(1)) == DRANK, m.group(0) if m else "no wsel line")
    if not results[-1] or not all(results[-6:]):
        print("  log:", os.path.join(WORK, "menu", "client.log"))


def game_scenario():
    log = run("game", GAME_SCRIPT)
    st = states(log)
    check("the game logged the held weapon", len(st) >= 6, f"{len(st)} state lines")
    if len(st) < 6:
        print("  log:", os.path.join(WORK, "game", "client.log"))
        return
    t0, slot0, size0, w0, ammo0, _ = st[0]
    check("Nettspend spawns with the Purple Drank in hand and three slots", w0 == "Purple Drank" and slot0 == "0" and size0 == "3",
          f"weapon={w0!r} slot={slot0}/{size0}")
    t1, _, _, w1, ammo1, _ = st[1]
    check("the Drank fired (ammo went down)", w1 == "Purple Drank" and int(ammo1) < int(ammo0), f"ammo {ammo0} -> {ammo1}")
    t2, _, _, w2, _, reload2 = st[2]
    check("R reloads the Drank (reload timer running)", w2 == "Purple Drank" and float(reload2) > 0.0, f"reload={reload2}")
    # the pour up: both clips started after the R press, and no Krunker reload sound while the Drank was held
    pos_r = log.find("[input_script] state t=%s " % t2)
    pos_swap = log.find("[input_script] state t=%s " % st[4][0])
    drank_span = log[log.find("[input_script] state t=%s " % t1):pos_swap]
    check("the reload plays the pour (kn_drank_pour)", "play2d kn_drank_pour" in drank_span)
    check("the reload plays the scream + line (kn_drank_reload)", "play2d kn_drank_reload" in drank_span)
    check("no reload_1 / reload_2 while the Drank reloads", "play2d reload_1" not in drank_span and "play2d reload_2" not in drank_span)
    # the stream: the splash (every 220 ms while firing), never the pour per shot (the pour is the reload's, once)
    fire_span = log[log.find("[input_script] state t=%s " % t0):log.find("[input_script] state t=%s " % t1)]
    check("firing the Drank plays the stream splash (kn_drank_splash)", "play2d kn_drank_splash" in fire_span)
    check("the pour plays once, for the reload only", fire_span.count("play2d kn_drank_pour") == 0 and drank_span.count("play2d kn_drank_pour") == 1,
          "fire=%d drank=%d" % (fire_span.count("play2d kn_drank_pour"), drank_span.count("play2d kn_drank_pour")))
    t4, slot4, _, w4, _, _ = st[4]
    check("E swaps to Money Spread (the class's secondary)", w4 == "Money Spread" and slot4 == "1", f"weapon={w4!r} slot={slot4}")
    money_span = log[pos_swap:]
    check("a Money Spread shot plays the cash throw (kn_money_throw)", "play2d kn_money_throw" in money_span)
    check("the pour up is the Drank's only: no kn_drank_* after the swap", "play2d kn_drank_" not in money_span)
    if not all(results[-12:]):
        print("  log:", os.path.join(WORK, "game", "client.log"))


try:
    menu_scenario()
    game_scenario()
finally:
    if all(results) and os.environ.get("KN_KEEP_WORK") is None:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("work dir kept:", WORK)
ok = all(results) and results
print("%s: %d/%d" % ("PASS" if ok else "FAIL", sum(results), len(results)))
sys.exit(0 if ok else 1)
