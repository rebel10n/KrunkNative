#!/usr/bin/env python3
"""Nettspend (client/include/nettspend.h): the Purple Drank's cup and pour-up, the Money Spread's bills
and the "laced" post-effect, as drawn.

  python3 -I client/tests/nettspend_vfx_e2e.py BUILD_DIR      (run from the repository root)

An offline Kanji game (headless client_bin, input script, fixed 60 fps step, 640x360): the Object
Manager hands the weapon out (`om add <id>`), the player stands still facing down the open street.
Two runs:

  drank: the cup is held (the input script's `hose` probe: the rim, its tip, shows in the window above
         the fist), every stream shot leaves from the drawn rim (KRUNKNATIVE_VFX_LOG "stream source"),
         a reload (R) is the pour-up (`pour` probes: the bottle's mouth comes in above the rim and
         pours, droplets logged), then `laced 8000` starts the laced visuals: after the fade-in the
         intensity is 1 and the frame differs from the steady one far more than two steady frames
         differ from each other, with a purple cast (a "laced-effect screenshot diff").
  money: the cash stack is held, a throw bursts the bills from its drawn top (`[vfx] money burst` at
         the probe's tip) with no muzzle flash, and the next frame differs from the rest frame.

Frames are kept in WORK (printed): <run>_rest.png, drank_pour.png, drank_steady_a/b.png, drank_laced.png,
money_throw.png; KN_TEST_KEEP=1 keeps the logs on success too. The weapon ids are read from
shared/include/weapons.h (WEAPON_ID_DRANK / WEAPON_ID_MONEY_SPREAD).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it) and Pillow.
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageStat

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-nettspend-vfx-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
SOURCE_TOLERANCE = 0.05  # units between a shot's source and the probe's tip (idle sway between the probe and the shot)
RELOAD_MS = 1400.0       # the Drank's pour-up (kn_drank.c reload_time)
results = []

VEC = r"(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)"
HOSE_RE = re.compile(rf"\[input_script\] hose t=(\d+) tip={VEC} back={VEC} hand={VEC} cam={VEC} "
                     r"screen_tip=(-?[\d.]+),(-?[\d.]+) screen_hand=(-?[\d.]+),(-?[\d.]+)")
POUR_RE = re.compile(rf"\[input_script\] pour t=(\d+) strength=(-?[\d.]+) mouth={VEC} rim={VEC} "
                     r"screen_mouth=(-?[\d.]+),(-?[\d.]+) screen_rim=(-?[\d.]+),(-?[\d.]+)")
POUR_NONE_RE = re.compile(r"\[input_script\] pour t=(\d+) none")
LACED_RE = re.compile(r"\[input_script\] laced t=(\d+) intensity=(-?[\d.]+) left=(-?[\d.]+) time=(-?[\d.]+)")
SOURCE_RE = re.compile(r"\[vfx\] stream source (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")
BURST_RE = re.compile(r"\[vfx\] money burst (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")
DROPS_RE = re.compile(r"\[vfx\] pour (-?[\d.]+) mouth")
SHOT_RE = re.compile(r"\[vfx\] shot (\S+) scoped")
MUZZLE_RE = re.compile(r"\[vfx\] muzzle")


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def weapon_ids():
    """WEAPON_ID_DRANK / WEAPON_ID_MONEY_SPREAD from shared/include/weapons.h (WEAPON_ID_KN_BASE + n)."""
    ids = {"DRANK": 67, "MONEY_SPREAD": 68}
    try:
        text = open("shared/include/weapons.h").read()
    except OSError:
        return ids
    base = re.search(r"#define WEAPON_ID_KN_BASE (\d+)", text)
    for name in ids:
        m = re.search(rf"#define WEAPON_ID_{name} \(WEAPON_ID_KN_BASE \+ (\d+)\)", text)
        if m and base:
            ids[name] = int(base.group(1)) + int(m.group(1))
    return ids


def frames_between(start, end, cmd):
    lines, t = [], start
    while t < end:
        lines.append(f"{t:.3f} {cmd}")
        t += FRAME_MS
    return lines


def drank_script(work, weapon):
    shot = lambda name: f"{os.path.join(work, 'drank_' + name)}.png"
    lines = ["0 play", f"400 om add {weapon}", "1000 pose -343 -143 -28 0 0", "1450 state", f"1400 shot {shot('rest')}",
             "1450 hose", "1500 mdown left", "1900 mup left"]
    lines += frames_between(1500.0, 1900.0, "hose")
    # two steady frames a frame apart (the baseline of the frame diff), then the pour-up and the laced dose
    lines += [f"2200 shot {shot('steady_a')}", f"{2200 + FRAME_MS:.3f} shot {shot('steady_b')}",
              "2300 down R", f"{2300 + 2 * FRAME_MS:.3f} up R", f"{2300 + RELOAD_MS * 0.5:.0f} shot {shot('pour')}"]
    lines += frames_between(2300.0, 2300.0 + RELOAD_MS + 100.0, "pour")
    lines += ["3900 laced 8000", "4600 laced", f"4600 shot {shot('laced')}", "4700 quit"]
    return "\n".join(sorted(lines, key=lambda line: float(line.split()[0]))) + "\n"


def money_script(work, weapon):
    shot = lambda name: f"{os.path.join(work, 'money_' + name)}.png"
    lines = ["0 play", f"400 om add {weapon}", "1000 pose -343 -143 -28 0 0", "1450 state", f"1400 shot {shot('rest')}",
             "1500 mdown left", f"{1500 + FRAME_MS:.3f} mup left", f"{1500 + FRAME_MS:.3f} shot {shot('throw')}", "1800 quit"]
    lines += frames_between(1450.0, 1530.0, "hose")
    return "\n".join(sorted(lines, key=lambda line: float(line.split()[0]))) + "\n"


def run(tag, text, timeout=240):
    work = os.path.join(WORK, tag)
    cfg = os.path.join(work, "cfg")
    os.makedirs(cfg)
    script = os.path.join(work, f"{tag}.script")
    with open(script, "w") as f:
        f.write(text)
    log = os.path.join(work, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(work, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS),
                "KRUNKNATIVE_VFX_LOG": "1", "KRUNKNATIVE_WINDOW_SIZE": "640x360", "KRUNKNATIVE_AUDIO_BACKEND": "null"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{tag}: client_bin finished its script", False, f"timeout, log {log}")
            return []
    events = []  # (kind, payload) in log order
    with open(log, errors="replace") as f:
        for line in f:
            m = HOSE_RE.search(line)
            if m:
                g = [float(x) for x in m.groups()]
                events.append(("hose", {"t": g[0], "tip": g[1:4], "back": g[4:7], "hand": g[7:10], "cam": g[10:13],
                                        "screen_tip": g[13:15], "screen_hand": g[15:17]}))
                continue
            m = POUR_RE.search(line)
            if m:
                g = [float(x) for x in m.groups()]
                events.append(("pour", {"t": g[0], "strength": g[1], "mouth": g[2:5], "rim": g[5:8], "screen_mouth": g[8:10],
                                        "screen_rim": g[10:12]}))
                continue
            for kind, rx in (("pour_none", POUR_NONE_RE), ("laced", LACED_RE), ("source", SOURCE_RE), ("burst", BURST_RE),
                             ("drops", DROPS_RE), ("shot", SHOT_RE), ("muzzle", MUZZLE_RE)):
                m = rx.search(line)
                if m:
                    events.append((kind, [float(x) if re.fullmatch(r"-?[\d.]+", x) else x for x in m.groups()]))
                    break
    return events


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def length(a):
    return math.sqrt(sum(x * x for x in a))


def in_window(uv):
    return 0.0 < uv[0] < 1.0 and 0.0 < uv[1] < 1.0


def frame_diff(a, b):
    """(mean absolute RGB difference 0..255, mean blue - green of b minus a's: the purple cast)"""
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return 255.0, 0.0
    diff = ImageStat.Stat(ImageChops.difference(ia, ib)).mean
    sa, sb = ImageStat.Stat(ia).mean, ImageStat.Stat(ib).mean
    return sum(diff) / 3.0, (sb[2] - sb[1]) - (sa[2] - sa[1])


def verify_drank(events):
    tag = "drank"
    probes = [e for kind, e in events if kind == "hose"]
    check(f"{tag}: the cup probes ran (the Drank was handed out and held)", probes, WORK)
    if not probes:
        return
    still = probes[0]
    check(f"{tag}: the rim shows in the window, above the fist", in_window(still["screen_tip"]) and still["screen_tip"][1] < still["screen_hand"][1],
          f"rim at {still['screen_tip'][0]:.3f},{still['screen_tip'][1]:.3f}, hand at {still['screen_hand'][0]:.3f},{still['screen_hand'][1]:.3f}")
    check(f"{tag}: the rim is above the fist (world)", still["tip"][1] > still["hand"][1], f"rim y {still['tip'][1]:.3f}, hand y {still['hand'][1]:.3f}")

    last, worst, sprays, unmatched = None, 0.0, 0, 0
    for kind, e in events:
        if kind == "hose":
            last = e
        elif kind == "source":
            sprays += 1
            if last is None:
                unmatched += 1
                continue
            worst = max(worst, length(sub(e, last["tip"])))
    check(f"{tag}: the local player sprayed", sprays >= 5, f"{sprays} stream sources logged")
    check(f"{tag}: every stream shot leaves from the drawn rim", sprays and not unmatched and worst <= SOURCE_TOLERANCE,
          f"worst {worst:.4f} units from the rim, {unmatched} without a probe")

    pours = [e for kind, e in events if kind == "pour"]
    strong = [e for e in pours if e["strength"] >= 0.5]
    check(f"{tag}: the reload is the pour-up (pour probes with the bottle tipped)", len(strong) >= 10, f"{len(pours)} pour probes, {len(strong)} at strength >= 0.5")
    if strong:
        above = all(e["mouth"][1] > e["rim"][1] for e in strong)
        near = max(math.hypot(e["mouth"][0] - e["rim"][0], e["mouth"][2] - e["rim"][2]) for e in strong)
        shown = all(in_window(e["screen_mouth"]) and in_window(e["screen_rim"]) for e in strong)
        check(f"{tag}: the bottle's mouth pours from above the rim", above and near <= 0.45, f"mouth over the rim within {near:.3f} on x/z, above: {above}")
        check(f"{tag}: the pour shows (mouth and rim in the window)", shown)
    drops = sum(1 for kind, _ in events if kind == "drops")
    check(f"{tag}: the pour's droplets spawned", drops >= 10, f"{drops} frames with droplets")
    ended = any(kind == "pour_none" and e[0] >= 2300 + RELOAD_MS for kind, e in events)
    check(f"{tag}: the bottle goes when the reload ends", ended)

    laced = [e for kind, e in events if kind == "laced"]
    check(f"{tag}: the laced probe ran", laced)
    if laced:
        t, intensity, left, time = laced[-1]
        check(f"{tag}: the laced visuals are at full strength after the fade-in", intensity >= 0.99 and 7000.0 <= left <= 7400.0 and time >= 0.6,
              f"intensity {intensity:.3f}, {left:.0f} ms left, time {time:.3f}")
    shots = {n: os.path.join(WORK, "drank", f"drank_{n}.png") for n in ("rest", "steady_a", "steady_b", "pour", "laced")}
    have = all(os.path.exists(p) for p in shots.values())
    check(f"{tag}: the frames were written", have, ", ".join(n for n, p in shots.items() if not os.path.exists(p)))
    if have:
        base, _ = frame_diff(shots["steady_a"], shots["steady_b"])
        dose, cast = frame_diff(shots["steady_a"], shots["laced"])
        check(f"{tag}: the laced frame differs from the steady one (laced-effect screenshot diff)", dose >= 4.0 * base + 3.0,
              f"laced {dose:.2f} vs steady {base:.2f} mean abs difference")
        check(f"{tag}: the laced frame has the purple cast", cast >= 1.0, f"blue - green up by {cast:.2f}")
        pour, _ = frame_diff(shots["rest"], shots["pour"])
        check(f"{tag}: the pour-up frame differs from the rest frame (the bottle is in view)", pour >= 1.0, f"{pour:.2f} mean abs difference")


def verify_money(events):
    tag = "money"
    probes = [e for kind, e in events if kind == "hose"]
    check(f"{tag}: the cash probes ran (the Money Spread was handed out and held)", probes, WORK)
    if not probes:
        return
    still = probes[0]
    check(f"{tag}: the stack's top shows in the window", in_window(still["screen_tip"]),
          f"top at {still['screen_tip'][0]:.3f},{still['screen_tip'][1]:.3f}")
    shots = [e for kind, e in events if kind == "shot"]
    check(f"{tag}: the throw is the cash stack's (no model)", shots and shots[0][0] == "cash", f"{[s[0] for s in shots]}")
    last, bursts, worst = None, 0, 0.0
    for kind, e in events:
        if kind == "hose":
            last = e
        elif kind == "burst":
            bursts += 1
            if last is not None:
                worst = max(worst, length(sub(e, last["tip"])))
    check(f"{tag}: the throw bursts bills from the drawn stack's top", bursts >= 1 and worst <= SOURCE_TOLERANCE, f"{bursts} bursts, worst {worst:.4f} units from the top")
    check(f"{tag}: no muzzle flash", not any(kind == "muzzle" for kind, _ in events))
    rest, throw = (os.path.join(WORK, "money", f"money_{n}.png") for n in ("rest", "throw"))
    have = os.path.exists(rest) and os.path.exists(throw)
    check(f"{tag}: the frames were written", have)
    if have:
        d, _ = frame_diff(rest, throw)
        check(f"{tag}: the bills show in the frame after the throw", d >= 0.5, f"{d:.2f} mean abs difference")


ids = weapon_ids()
print(f"weapon ids: Drank {ids['DRANK']}, Money Spread {ids['MONEY_SPREAD']}; frames in {WORK}")
try:
    verify_drank(run("drank", drank_script(os.path.join(WORK, "drank"), ids["DRANK"])))
    verify_money(run("money", money_script(os.path.join(WORK, "money"), ids["MONEY_SPREAD"])))
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs and frames kept in {WORK}")

print("nettspend_vfx_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
