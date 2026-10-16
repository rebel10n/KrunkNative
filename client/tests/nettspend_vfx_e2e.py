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
         The cup is the white styrofoam double cup (`wsel`: model=cup in the foam colour, never the
         Piss's hose), and its wall shows as near-white pixels between the drawn rim and the fist.
  money: the fan of cash is held (`wsel`: model=cash, no Pistol stand-in), a throw bursts the bills
         from its drawn top (`[vfx] money burst` at the probe's tip) with no muzzle flash, and the next
         frame differs from the rest frame. The sky stays the sky (user, 2026-10-14: "seems to change
         the skybox"): a patch of open sky above the street keeps its colour from the frame before
         the Money Spread came out (the class's rifle in hand) through holding it, the throw and the
         recoil after it (the first version drew the Pistol's model ~3000x too big around the camera).
  dust:  the money run aimed at the street's ground (`look 0 -0.9`): a throw leaves no grey dust cloud at
         the feet (the Pistol's impact puffs, one per bill that hit the ground, were a cloud of their own):
         the grey-white pixels of the ground box grow by no more than GREY_MAX_DELTA after the throw.
  tp:    third person (`thirdperson`): the body holds the cup, then the cash (wsel tp=1 model=cup /
         cash), never the hose.

Frames are kept in WORK (printed): <run>_rest.png, drank_pour.png, drank_steady_a/b.png, drank_laced.png,
money_base.png, money_throw.png, money_after_<ms>.png, tp_drank.png, tp_money.png; KN_TEST_KEEP=1 keeps the logs on success too. The weapon ids are read from
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
HANDS_RE = re.compile(rf"\[input_script\] hands t=(\d+) trigger={VEC} off={VEC} screen_trigger=(-?[\d.]+),(-?[\d.]+) "
                      rf"screen_off=(-?[\d.]+),(-?[\d.]+) bottle={VEC} visible=(\d) screen_bottle=(-?[\d.]+),(-?[\d.]+) off_visible=(\d)")
POUR_NONE_RE = re.compile(r"\[input_script\] pour t=(\d+) none")
LACED_RE = re.compile(r"\[input_script\] laced t=(\d+) intensity=(-?[\d.]+) left=(-?[\d.]+) time=(-?[\d.]+)")
SOURCE_RE = re.compile(r"\[vfx\] stream source (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")
BURST_RE = re.compile(r"\[vfx\] money burst (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")
DROPS_RE = re.compile(r"\[vfx\] pour (-?[\d.]+) mouth")
SHOT_RE = re.compile(r"\[vfx\] shot (\S+) scoped")
MUZZLE_RE = re.compile(r"\[vfx\] muzzle")
WSEL_RE = re.compile(r"\[input_script\] wsel t=\d+ @(\S+) who=me slot=(-?\d+) tp=(-?\d+) (.*)")
SKY_BOX = (285, 5, 345, 70)  # open sky above the street at the pose below (640x360)
SKY_MAX_SHIFT = 6.0          # mean RGB change of the sky patch allowed (recoil moves the view a little)
FOAM_MIN = 165               # a "white" foam pixel: every channel at least this ...
FOAM_SPREAD = 45             # ... and at most this far apart (not tinted)
FOAM_PIXELS = 120            # near-white pixels the cup's wall shows at least (640x360)
GREY_BOX = (180, 40, 460, 280)  # the ground in front of the player when aimed down (640x360)
GREY_MAX_DELTA = 250         # grey-white pixels a throw may add there (a dust cloud adds ~800 on origin/main)
CUP_X = (0.3, 0.7)           # the cup's rim across the window: in front of the chest, not out at the camera's edge
BOTTLE_FROM_HAND = 0.05      # units: the bottle's grip (where it is held) from the off hand's centre, every pour-up frame
OFF_HAND_RISE = 0.5          # units the off hand at least lifts the bottle above its rest (over the cup) during the pour-up
TP_SCALE = 1.8               # NETTSPEND_TP_SCALE: third person's cup and bottle (the mouth's reach over the rim scales too)
HAND_BELOW = 0.12            # window fraction a fist's centre may sit below the bottom edge (the view cuts the arms there)
TP_OFF_HAND_DROP = 3.0       # third person, outside the pour-up: the off hand hangs at least this far below the cup hand
POUR_TIPPED = (0.3, 0.75)    # reload progress where the bottle is over the cup (both hands up, in the frame)


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
             "1450 hose", "1450 hands", "1460 wsel @drank", "1500 mdown left", "1900 mup left"]
    lines += frames_between(1500.0, 1900.0, "hose")
    # two steady frames a frame apart (the baseline of the frame diff), then the pour-up and the laced dose
    lines += [f"2200 shot {shot('steady_a')}", f"{2200 + FRAME_MS:.3f} shot {shot('steady_b')}",
              "2300 down R", f"{2300 + 2 * FRAME_MS:.3f} up R", f"{2300 + RELOAD_MS * 0.5:.0f} shot {shot('pour')}"]
    lines += frames_between(2300.0, 2300.0 + RELOAD_MS + 100.0, "pour")
    lines += frames_between(2300.0, 2300.0 + RELOAD_MS + 100.0, "hands")
    # walking and aiming with the cup after the pour-up: one arm still (the off arm out of view)
    lines += ["3720 down W", f"3800 shot {shot('walk')}", "3800 hands", "3850 up W", "3860 mdown right", "3885 hands", "3895 mup right"]
    lines += ["3900 laced 8000", "4600 laced", f"4600 shot {shot('laced')}", "4700 quit"]
    return "\n".join(sorted(lines, key=lambda line: float(line.split()[0]))) + "\n"


AFTER_MS = (50, 120, 200, 400)


def dust_script(work, weapon):
    shot = lambda name: f"{os.path.join(work, 'dust_' + name)}.png"
    lines = ["0 play", "300 pose -343 -143 -28 0 0", f"800 om add {weapon}", "1000 pose -343 -143 -28 0 0", "1050 look 0 -0.9",
             "1100 look 0 0", f"1400 shot {shot('held')}", "1500 mdown left", "1516 mup left", f"1550 shot {shot('fire50')}",
             f"1620 shot {shot('fire120')}", "1900 quit"]
    return "\n".join(sorted(lines, key=lambda line: float(line.split()[0]))) + "\n"


def money_script(work, weapon):
    shot = lambda name: f"{os.path.join(work, 'money_' + name)}.png"
    # the base frame: the same pose with the class's rifle, before the Money Spread comes out
    lines = ["0 play", "300 pose -343 -143 -28 0 0", f"700 shot {shot('base')}", f"800 om add {weapon}", "1000 pose -343 -143 -28 0 0",
             "1450 state", f"1400 shot {shot('rest')}", "1450 hands", "1460 wsel @money",
             "1500 mdown left", f"{1500 + FRAME_MS:.3f} mup left", f"{1500 + FRAME_MS:.3f} shot {shot('throw')}", "1900 quit"]
    lines += [f"{1500 + ms} shot {shot('after_%d' % ms)}" for ms in AFTER_MS]
    lines += frames_between(1450.0, 1530.0, "hose")
    return "\n".join(sorted(lines, key=lambda line: float(line.split()[0]))) + "\n"


def tp_script(work, drank, money):
    shot = lambda name: f"{os.path.join(work, 'tp_' + name)}.png"
    # the Drank in third person, then its pour-up (a reload after a sip) from the third-person camera, then the cash
    lines = ["0 play", f"300 om add {drank}", "700 con thirdperson", "1000 pose -343 -143 -28 0 0", "1500 wsel @tpdrank",
             f"1500 shot {shot('drank')}", "1500 hands", "1550 mdown left", "1700 mup left", "2300 down R", f"{2300 + 2 * FRAME_MS:.3f} up R",
             f"{2300 + RELOAD_MS * 0.5:.0f} shot {shot('pour')}", f"{2300 + RELOAD_MS + 300:.0f} om add {money}",
             f"{2300 + RELOAD_MS + 1100:.0f} wsel @tpmoney", f"{2300 + RELOAD_MS + 1100:.0f} shot {shot('money')}", f"{2300 + RELOAD_MS + 1200:.0f} quit"]
    lines += frames_between(2300.0, 2300.0 + RELOAD_MS + 100.0, "pour")
    lines += frames_between(2300.0, 2300.0 + RELOAD_MS + 100.0, "hands")
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
            m = HANDS_RE.search(line)
            if m:
                g = [float(x) for x in m.groups()]
                events.append(("hands", {"t": g[0], "trigger": g[1:4], "off": g[4:7], "screen_trigger": g[7:9], "screen_off": g[9:11],
                                         "bottle": g[11:14], "visible": int(g[14]), "screen_bottle": g[15:17], "off_visible": int(g[17])}))
                continue
            m = WSEL_RE.search(line)
            if m:
                events.append(("wsel", {"label": m.group(1), "slot": int(m.group(2)), "tp": int(m.group(3)), "sel": m.group(4)}))
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


def hand_in_frame(uv):
    """a fist at the bottom of the window: its centre may sit up to HAND_BELOW (window fraction) under the edge"""
    return 0.0 < uv[0] < 1.0 and 0.0 < uv[1] < 1.0 + HAND_BELOW


def frame_diff(a, b):
    """(mean absolute RGB difference 0..255, mean blue - green of b minus a's: the purple cast)"""
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return 255.0, 0.0
    diff = ImageStat.Stat(ImageChops.difference(ia, ib)).mean
    sa, sb = ImageStat.Stat(ia).mean, ImageStat.Stat(ib).mean
    return sum(diff) / 3.0, (sb[2] - sb[1]) - (sa[2] - sa[1])


def sky_shift(a, b):
    """(mean RGB change of the sky patch, mean absolute per-pixel difference) between two frames"""
    ia, ib = (Image.open(p).convert("RGB").crop(SKY_BOX) for p in (a, b))
    ma, mb = ImageStat.Stat(ia).mean, ImageStat.Stat(ib).mean
    return sum(abs(ma[i] - mb[i]) for i in range(3)) / 3.0, sum(ImageStat.Stat(ImageChops.difference(ia, ib)).mean) / 3.0


def grey_pixels(path, box=GREY_BOX):
    """near-white, untinted pixels in a box of the frame (a dust cloud's colour)"""
    im = Image.open(path).convert("RGB").crop(box)
    data = im.tobytes()
    return sum(1 for i in range(0, len(data), 3) if min(data[i:i + 3]) > 150 and max(data[i:i + 3]) - min(data[i:i + 3]) < 30)


def foam_pixels(path, rim, hand):
    """near-white, untinted pixels in the box from the drawn rim down to the fist (window fractions)"""
    im = Image.open(path).convert("RGB")
    w, h = im.size
    x0, x1 = int(min(rim[0], hand[0]) * w) - 40, int(max(rim[0], hand[0]) * w) + 40
    y0, y1 = int(rim[1] * h) - 20, int(hand[1] * h) + 10
    box = im.crop((max(0, x0), max(0, y0), min(w, x1), min(h, y1)))
    data = box.tobytes()
    pixels = (data[i:i + 3] for i in range(0, len(data), 3))
    return sum(1 for r, g, b in pixels if min(r, g, b) >= FOAM_MIN and max(r, g, b) - min(r, g, b) <= FOAM_SPREAD)


def wsel_of(events, label):
    return next((e for kind, e in events if kind == "wsel" and e["label"] == label), None)


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
    sel = wsel_of(events, "drank")
    check(f"{tag}: the held model is the white styrofoam cup, not the hose", sel and "model=cup" in sel["sel"] and "#f6f5f1" in sel["sel"] and "hose" not in sel["sel"],
          sel["sel"] if sel else "no wsel line")
    rest = os.path.join(WORK, "drank", "drank_rest.png")
    if os.path.exists(rest):
        white = foam_pixels(rest, still["screen_tip"], still["screen_hand"])
        check(f"{tag}: the cup's wall shows white (styrofoam) pixels", white >= FOAM_PIXELS, f"{white} near-white pixels (>= {FOAM_PIXELS})")
    check(f"{tag}: the cup is held in front of the chest (its rim in the middle third of the window)", CUP_X[0] <= still["screen_tip"][0] <= CUP_X[1],
          f"rim at x {still['screen_tip'][0]:.3f} (allowed {CUP_X[0]}..{CUP_X[1]})")
    hands = [e for kind, e in events if kind == "hands"]
    check(f"{tag}: the hands probe ran", hands)
    if hands:
        rest_hands = hands[0]
        # one arm holds the cup out (user, 2026-10-15): the off arm only shows for the refill
        check(f"{tag}: the cup hand is in the frame while the cup is held (its fist at the bottom edge)", hand_in_frame(rest_hands["screen_trigger"]),
              f"trigger at {rest_hands['screen_trigger']}")
        idle = [h for h in hands if not h["visible"]]  # idle, after the pour-up, walking, aiming
        check(f"{tag}: outside the pour-up the off arm is not drawn and its hand is out of view (idle, walking, aiming)",
              len(idle) >= 3 and all(not h["off_visible"] and not hand_in_frame(h["screen_off"]) for h in idle),
              f"{len(idle)} probes: " + ", ".join(f"t={h['t']:.0f} drawn={h['off_visible']} at y {h['screen_off'][1]:.2f}" for h in idle[:6]))
        verify_off_hand_pour(tag, hands, first_person=True)
        tipped = [h for h in hands if h["visible"] and POUR_TIPPED[0] <= (h["t"] - 2300.0) / RELOAD_MS <= POUR_TIPPED[1]]
        check(f"{tag}: the pour-up uses both hands (the off arm comes up with the bottle, the trigger hand holds the cup)",
              len(tipped) >= 5 and all(h["off_visible"] and hand_in_frame(h["screen_off"]) and hand_in_frame(h["screen_trigger"]) for h in tipped),
              f"{len(tipped)} frames with the bottle over the cup")

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

    verify_pour_probes(tag, events, 1.0)
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
    sel = wsel_of(events, "money")
    check(f"{tag}: the held model is the fan of cash (no Pistol stand-in)", sel and "model=cash" in sel["sel"] and "weapon_3" not in sel["sel"],
          sel["sel"] if sel else "no wsel line")
    hands = [e for kind, e in events if kind == "hands"]
    check(f"{tag}: both hands are in the frame while the cash is held (the fists at the bottom edge)",
          hands and hand_in_frame(hands[0]["screen_trigger"]) and hand_in_frame(hands[0]["screen_off"]),
          f"trigger {hands[0]['screen_trigger'] if hands else None}, off hand {hands[0]['screen_off'] if hands else None}")
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
    base = os.path.join(WORK, "money", "money_base.png")
    frames = [("held", rest), ("throw", throw)] + [(f"+{ms} ms", os.path.join(WORK, "money", f"money_after_{ms}.png")) for ms in AFTER_MS]
    if os.path.exists(base) and all(os.path.exists(p) for _, p in frames):
        worst = max(((name,) + sky_shift(base, p) for name, p in frames), key=lambda t: t[1])
        check(f"{tag}: the sky stays the sky while the Money Spread is held and thrown", worst[1] <= SKY_MAX_SHIFT,
              f"worst {worst[0]}: sky colour moved {worst[1]:.2f} (pixels {worst[2]:.2f}), allowed {SKY_MAX_SHIFT}")
    else:
        check(f"{tag}: the sky frames were written", False)


def verify_dust(events):
    tag = "dust"
    held, fire = (os.path.join(WORK, "dust", f"dust_{n}.png") for n in ("held", "fire50"))
    if not (os.path.exists(held) and os.path.exists(fire)):
        check(f"{tag}: the frames were written", False)
        return
    before, after = grey_pixels(held), grey_pixels(fire)
    check(f"{tag}: a throw at the ground leaves no grey dust cloud", after - before <= GREY_MAX_DELTA,
          f"grey-white pixels {before} held -> {after} after the throw (allowed +{GREY_MAX_DELTA})")


def verify_tp(events):
    tag = "tp"
    for label, model in (("tpdrank", "cup"), ("tpmoney", "cash")):
        sel = wsel_of(events, label)
        check(f"{tag}: third person holds the {model}", sel and sel["tp"] == 1 and f"model={model}" in sel["sel"] and "hose" not in sel["sel"],
              sel["sel"] if sel else "no wsel line")
    # the pour-up seen from the third-person camera: the same off-hand bottle, over the body's cup
    hands = [e for kind, e in events if kind == "hands"]
    verify_off_hand_pour(tag, hands)
    # one arm holds the cup out: outside the pour-up the off hand hangs at the side, holding nothing
    idle = [h for h in hands if not h["visible"]]
    drop = min((h["trigger"][1] - h["off"][1] for h in idle), default=0.0)
    check(f"{tag}: outside the pour-up the off hand hangs at the side, empty (at least {TP_OFF_HAND_DROP} below the cup hand)",
          len(idle) >= 2 and drop >= TP_OFF_HAND_DROP, f"{len(idle)} probes, the smallest drop {drop:.2f} units")
    tipped = [h for h in hands if h["visible"] and POUR_TIPPED[0] <= (h["t"] - 2300.0) / RELOAD_MS <= POUR_TIPPED[1]]
    check(f"{tag}: during the pour-up the off hand is up with the bottle (drawn, at the cup's height)",
          len(tipped) >= 5 and all(h["off_visible"] and h["off"][1] >= h["trigger"][1] - 1.0 for h in tipped), f"{len(tipped)} frames")
    verify_pour_probes(tag, events, TP_SCALE)
    rest, pour = (os.path.join(WORK, "tp", f"tp_{n}.png") for n in ("drank", "pour"))
    if os.path.exists(rest) and os.path.exists(pour):
        diff, _ = frame_diff(rest, pour)
        check(f"{tag}: the pour-up frame differs from the rest frame (the bottle is in view)", diff >= 0.3, f"{diff:.2f} mean abs difference")


def verify_off_hand_pour(tag, hands, first_person=False):
    """the bottle comes from the off hand, stays in it (its grip on the fist) while it is carried over the cup and tipped,
    and goes back: the off hand rises with it and is at its rest again once the reload is over (first person: back down out
    of view and not drawn; the away pose hangs ~4 units from the shoulder, so the view's sway moves it ~1 unit in the world)"""
    poured = [h for h in hands if h["visible"]]
    worst = max((length(sub(h["bottle"], h["off"])) for h in poured), default=None)
    check(f"{tag}: the bottle stays in the off hand through the pour-up (every frame)", poured and worst <= BOTTLE_FROM_HAND,
          f"{len(poured)} frames, worst {worst if worst is None else round(worst, 4)} units between the bottle's grip and the off hand "
          f"(allowed {BOTTLE_FROM_HAND})")
    if not hands or not poured:
        return
    rest = hands[0]["off"]
    rise = max(h["off"][1] for h in poured) - rest[1]
    check(f"{tag}: the off hand carries the bottle up over the cup", rise >= OFF_HAND_RISE, f"rises {rise:.3f} units (>= {OFF_HAND_RISE})")
    after = [h for h in hands if 2300 + RELOAD_MS <= h["t"] <= 2300 + RELOAD_MS + 100 and not h["visible"]]
    if first_person:
        back = after and not after[-1]["off_visible"] and not hand_in_frame(after[-1]["screen_off"])
        check(f"{tag}: the off hand is back down out of view after the pour-up", back,
              f"drawn={after[-1]['off_visible']} at y {after[-1]['screen_off'][1]:.2f}" if after else "no probe after the reload")
        return
    back = after and length(sub(after[-1]["off"], rest)) <= 0.3
    check(f"{tag}: the off hand is back at its rest after the pour-up", back,
          f"{length(sub(after[-1]['off'], rest)):.3f} units from its rest" if after else "no probe after the reload")


def verify_pour_probes(tag, events, scale):
    pours = [e for kind, e in events if kind == "pour"]
    strong = [e for e in pours if e["strength"] >= 0.5]
    check(f"{tag}: the reload is the pour-up (pour probes with the bottle tipped)", len(strong) >= 10, f"{len(pours)} pour probes, {len(strong)} at strength >= 0.5")
    if strong:
        above = all(e["mouth"][1] > e["rim"][1] for e in strong)
        near = max(math.hypot(e["mouth"][0] - e["rim"][0], e["mouth"][2] - e["rim"][2]) for e in strong)
        shown = all(in_window(e["screen_mouth"]) and in_window(e["screen_rim"]) for e in strong)
        check(f"{tag}: the bottle's mouth pours from above the rim", above and near <= 0.45 * scale,
              f"mouth over the rim within {near:.3f} on x/z (allowed {0.45 * scale:.2f}), above: {above}")
        check(f"{tag}: the pour shows (mouth and rim in the window)", shown)


ids = weapon_ids()
print(f"weapon ids: Drank {ids['DRANK']}, Money Spread {ids['MONEY_SPREAD']}; frames in {WORK}")
try:
    verify_drank(run("drank", drank_script(os.path.join(WORK, "drank"), ids["DRANK"])))
    verify_money(run("money", money_script(os.path.join(WORK, "money"), ids["MONEY_SPREAD"])))
    verify_dust(run("dust", dust_script(os.path.join(WORK, "dust"), ids["MONEY_SPREAD"])))
    verify_tp(run("tp", tp_script(os.path.join(WORK, "tp"), ids["DRANK"], ids["MONEY_SPREAD"])))
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs and frames kept in {WORK}")

print("nettspend_vfx_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
