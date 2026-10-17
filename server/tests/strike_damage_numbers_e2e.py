#!/usr/bin/env python3
"""Krunker Strike's damage numbers and Dynamic crosshair (client/src/ui/strike_hud.c fx_*, the rules of
client/src/ui/strike_hud_fx.c: FRVR 1.37.2's addDamageNumber / newDmgVal / dmgVal and updateCrosshair).

  python3 -I server/tests/strike_damage_numbers_e2e.py BUILD_DIR      (run from the repository root)

Live offline Strike clients (--flavor strike --offline), driven by input scripts; a hit is the script's
`damage` (the server's HIT on the first other player, put 20 units ahead of the camera), the numbers are
the page's .kn-strike-dmg elements (one per number on screen), the crosshair #knStrikeCross:
  1. the default (a fresh config: Legacy, FRVR's default_damage_numbers_mode 2): the crosshair is up in
     play; a hit of 40 shows "40", and a crit of 15.5 200 ms later is a second number (Legacy does not
     stack), both white with the #2e2e2e stroke, neither fades (the first still opaque at 970 ms) and both
     are gone after 1 s.
  2. the Display tab's Damage Number Display dropdown (a played config, so the main menu is up): the header
     reads Legacy, opening it lists Stacking / Individual / Legacy, picking Stacking sets the header and
     stores damageNumberDisplayMode 0 with the Strike settings (knSettings.json under krunknative-strike);
     then the same hits make one Stacking number "55.5" at the crit's size (33 px) that fades over its second
     half and is gone after 1 s.
  3. Individual (damageNumberDisplayMode 1 seeded in the store, the mobile layout: a touch first): two hits
     make two numbers; the first (750 ms) is gone before the second, both are gone after 1 s.
The Display tab's dropdown is driven with input_script's `call strikeAction` (the same action the header and
its options run): a click on a header under the scrolled panel lands on the tab bar in this harness.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). KRUNKNATIVE_ASSETS_DIR is passed through
(a worktree without the untracked asset dirs). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-dmg-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def env(cfg, extra=None):
    e = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    e.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
              "XDG_STATE_HOME": cfg + "-state", "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    e.update(extra or {})
    return e


def run(name, script_text, seed=None, played=False, timeout=240):
    """One client run: its script, the Strike settings seeded with `seed` (a dict), the FTUE's played flag
    set when `played` (a fresh config quick-connects into a game and never shows the main menu)."""
    cfg = os.path.join(WORK, name + "-cfg")
    store = os.path.join(cfg, "krunknative-strike")
    os.makedirs(store, exist_ok=True)
    if seed is not None:
        with open(os.path.join(store, "knSettings.json"), "w") as f:
            json.dump(seed, f)
    if played:
        with open(os.path.join(store, "knFtue.json"), "w") as f:
            json.dump({"guest": {"played": True}}, f)
    script = os.path.join(WORK, name + ".txt")
    with open(script, "w") as f:
        f.write(script_text)
    log = os.path.join(WORK, name + ".log")
    try:
        with open(log, "w") as f:
            rc = subprocess.run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], cwd=ROOT,
                                env=env(cfg, {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"}),
                                stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=timeout).returncode
    except subprocess.TimeoutExpired:
        rc = -9
    with open(log, errors="replace") as f:
        text = f.read()
    check(f"{name}: the client ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}")
    return text, os.path.join(store, "knSettings.json")


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else None


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d value=.*? text=(.*)" % re.escape(label), text)
    return (int(m.group(1)), m.group(2).strip()) if m else None


def style_of(text, label):
    """the first match's style attribute as {property: value}"""
    m = re.search(r"\[input_script\] attr t=\d+ @%s n=(\d+) style=(.*)" % re.escape(label), text)
    if not m or int(m.group(1)) < 1:
        return None
    out = {}
    for part in m.group(2).split(";"):
        if ":" in part:
            k, v = part.split(":", 1)
            out[k.strip()] = v.strip()
    return out


def vis_of(text, label, elem):
    m = re.search(r"\[input_script\] vis t=\d+ @%s .*?\b%s=(\S+)" % (re.escape(label), re.escape(elem)), text)
    return m.group(1) if m else None


def num(v, default=None):
    try:
        return float(re.match(r"-?[0-9.]+", v or "").group(0))
    except (AttributeError, ValueError):
        return default


# the two hits at t: 40 on the first other player, then 15.5 crit 200 ms later (the same player)
def hits(t):
    return [f"{t} damage 40 0", f"{t + 50} nodes @n1 .kn-strike-dmg", f"{t + 60} text @t1 .kn-strike-dmg",
            f"{t + 70} attr @s1 style .kn-strike-dmg",
            f"{t + 200} damage 15.5 1", f"{t + 250} nodes @n2 .kn-strike-dmg", f"{t + 260} text @t2 .kn-strike-dmg",
            f"{t + 270} attr @s2 style .kn-strike-dmg",
            f"{t + 600} nodes @n3 .kn-strike-dmg", f"{t + 610} attr @s3 style .kn-strike-dmg",
            f"{t + 820} nodes @n35 .kn-strike-dmg",
            f"{t + 970} nodes @n4 .kn-strike-dmg", f"{t + 980} attr @s4 style .kn-strike-dmg",
            f"{t + 1300} nodes @n5 .kn-strike-dmg"]


def legacy_hits(t):
    # Legacy rises 22 units a second: 20 units ahead the first number leaves the view after about half a second
    # (FRVR's frustum check drops it too), so the checks are while both are on screen
    return [f"{t} damage 40 0", f"{t + 50} nodes @n1 .kn-strike-dmg", f"{t + 60} text @t1 .kn-strike-dmg",
            f"{t + 70} attr @s1 style .kn-strike-dmg",
            f"{t + 200} damage 15.5 1", f"{t + 250} nodes @n2 .kn-strike-dmg", f"{t + 260} text @t2 .kn-strike-dmg",
            f"{t + 262} grep @g2 15.5 .kn-strike-dmg", f"{t + 270} attr @s2 style .kn-strike-dmg",
            f"{t + 330} nodes @n3 .kn-strike-dmg", f"{t + 340} attr @s3 style .kn-strike-dmg",
            f"{t + 1300} nodes @n5 .kn-strike-dmg"]


def case_legacy_default():
    s = ["0 menu", "3000 nodes @menu .mobMenuAcc--main-menu", "3100 play", "4000 nodes @ingame .frvr-in-game-ui__health-holder",
         "4050 vis @cross0 knStrikeCross", "4100 con bot 2", "4600 vis @cross knStrikeCross", "4650 attr @crossstyle style .kn-strike-cross",
         "4700 nodes @nobar .kn-cross-bar", "4750 nodes @nodmg .kn-strike-dmg"] + legacy_hits(5000) + ["6500 quit"]
    text, _ = run("legacy-default", "\n".join(s) + "\n")
    check("1: in a game: the health holder is up", nodes(text, "ingame") == 1, str(nodes(text, "ingame")))
    check("1: the Dynamic crosshair is shown in play, with its four lines", vis_of(text, "cross", "knStrikeCross") == "1" and nodes(text, "nobar") == 4,
          f"vis {vis_of(text, 'cross', 'knStrikeCross')} bars {nodes(text, 'nobar')}")
    cs = style_of(text, "crossstyle") or {}
    check("1: the crosshair is opaque at the hip", num(cs.get("opacity"), 0) == 1.0, str(cs.get("opacity")))
    check("1: no damage number before a hit", nodes(text, "nodmg") == 0, str(nodes(text, "nodmg")))
    check("1: a hit of 40 shows one number reading 40", nodes(text, "n1") == 1 and text_of(text, "t1") == (1, "40"), f"{nodes(text, 'n1')} {text_of(text, 't1')}")
    s1 = style_of(text, "s1") or {}
    check("1: a fresh number is at dmgScale 30 px, opaque, in GameFont, dmgColor #DE292F with the #2e2e2e stroke",
          num(s1.get("font-size"), 0) == 30.0 and num(s1.get("opacity"), 0) == 1.0 and s1.get("font-family") == "GameFont"
          and s1.get("color", "").lower() == "#de292f" and "#2e2e2e" in s1.get("text-shadow", ""),
          f"{s1.get('font-size')} {s1.get('opacity')} {s1.get('font-family')} {s1.get('color')}")
    check("1: Legacy does not stack: the crit of 15.5 is a second number", nodes(text, "n2") == 2 and text_of(text, "t2") == (2, "40")
          and grep_of(text, "g2") == 1, f"{nodes(text, 'n2')} {text_of(text, 't2')} {grep_of(text, 'g2')}")
    s2 = style_of(text, "s2") or {}
    check("1: the first number is opaque 270 ms on (no fade in its first half)", num(s2.get("opacity"), 0) == 1.0, str(s2.get("opacity")))
    check("1: both are still on screen 330 ms after the first hit", nodes(text, "n3") == 2, str(nodes(text, "n3")))
    s3 = style_of(text, "s3") or {}
    check("1: opaque at 340 ms (Legacy has no fade)", num(s3.get("opacity"), 0) == 1.0, str(s3.get("opacity")))
    check("1: both gone after 1 s", nodes(text, "n5") == 0, str(nodes(text, "n5")))


DMG_TOGGLE = "[onclick=\"strikeAction('dmgnum-toggle')\"]"


def case_dropdown_stacking():
    # Settings (the gear) > Display tab (index 1): the Damage Number Display row's dropdown
    s = ["0 menu", "3000 nodes @menu .mobMenuAcc--main-menu", "3100 clickid mobMenuSett",
         "3800 clicksel [onclick=\"strikeAction('settings-tab:1')\"]",
         "4300 nodes @rows .frvr-settings-screen .setting", "4310 grep @dmgrow Damage .frvr-settings-screen .settName",
         f"4320 text @hdr0 {DMG_TOGGLE} span", "4330 nodes @opts0 .dropdown-option",
         f"4400 call strikeAction \"dmgnum-toggle\"",
         "4700 nodes @opts1 .dropdown-option", "4710 text @opt1 .dropdown-option",
         "4720 grep @optleg Legacy .dropdown-option", "4730 grep @optind Individual .dropdown-option",
         "4800 call strikeAction \"dmgnum:0\"",
         f"5100 text @hdr1 {DMG_TOGGLE} span", "5110 nodes @opts2 .dropdown-option",
         "5200 call strikeAction \"home\"", "5900 nodes @home .mobMenuAcc--main-menu",
         "6000 play", "6900 nodes @ingame .frvr-in-game-ui__health-holder", "7000 con bot 2"] + hits(7500) + ["9000 quit"]
    text, store = run("stacking", "\n".join(s) + "\n", played=True)
    check("2: Settings > Display has the Damage Number Display row", (nodes(text, "rows") or 0) > 0 and grep_of(text, "dmgrow") == 1,
          f"rows {nodes(text, 'rows')} row {grep_of(text, 'dmgrow')}")
    hdr0 = text_of(text, "hdr0")
    check("2: its dropdown reads Legacy (the default) and is closed", hdr0 == (1, "Legacy") and nodes(text, "opts0") == 0,
          f"{hdr0} options {nodes(text, 'opts0')}")
    check("2: opening it lists the three modes: Stacking, Individual, Legacy",
          nodes(text, "opts1") == 3 and text_of(text, "opt1") == (3, "Stacking") and grep_of(text, "optind") == 1 and grep_of(text, "optleg") == 1,
          f"{nodes(text, 'opts1')} {text_of(text, 'opt1')}")
    hdr1 = text_of(text, "hdr1")
    check("2: picking Stacking sets the header and closes the list", hdr1 == (1, "Stacking") and nodes(text, "opts2") == 0,
          f"{hdr1} options {nodes(text, 'opts2')}")
    stored = None
    try:
        with open(store) as f:
            stored = json.load(f).get("damageNumberDisplayMode")
    except (OSError, ValueError):
        pass
    check("2: the choice is stored with the Strike settings (knSettings.json damageNumberDisplayMode 0)", stored in (0, "0"), f"{store}: {stored!r}")
    check("2: home, then in a game", nodes(text, "home") == 1 and nodes(text, "ingame") == 1, f"{nodes(text, 'home')} {nodes(text, 'ingame')}")
    check("2: Stacking: the crit of 15.5 on the same player stacks into one number 55.5", nodes(text, "n2") == 1 and text_of(text, "t2") == (1, "55.5"),
          f"{nodes(text, 'n2')} {text_of(text, 't2')}")
    s2 = style_of(text, "s2") or {}
    check("2: the stacked crit is drawn 1.1 times the size (33 px), gold", num(s2.get("font-size"), 0) == 33.0 and s2.get("color", "").lower().startswith("#ff"),
          f"{s2.get('font-size')} {s2.get('color')}")
    s3 = style_of(text, "s3") or {}
    check("2: the first half of its life is opaque (400 ms after the stack)", nodes(text, "n3") == 1 and num(s3.get("opacity"), 0) == 1.0,
          f"{nodes(text, 'n3')} {s3.get('opacity')}")
    s4 = style_of(text, "s4") or {}
    op4 = num(s4.get("opacity"), -1)
    check("2: it fades over the second half (770 ms after the stack: opacity about 0.46)", nodes(text, "n4") == 1 and 0.3 < op4 < 0.6, f"{nodes(text, 'n4')} {op4}")
    check("2: gone after its 1 s of life", nodes(text, "n5") == 0, str(nodes(text, "n5")))


def case_individual():
    s = ["0 menu", "3000 nodes @menu .mobMenuAcc--main-menu", "3100 play", "4000 nodes @ingame .frvr-in-game-ui__health-holder",
         "4100 con bot 2", "4500 touch 320 180", "4800 vis @cross knStrikeCross"] + hits(5000) + ["6500 quit"]
    text, _ = run("individual", "\n".join(s) + "\n", seed={"damageNumberDisplayMode": 1})
    check("3: the mobile layout (a touch) keeps the crosshair", vis_of(text, "cross", "knStrikeCross") == "1", str(vis_of(text, "cross", "knStrikeCross")))
    check("3: Individual: a hit of 40 shows one number", nodes(text, "n1") == 1 and text_of(text, "t1") == (1, "40"), f"{nodes(text, 'n1')} {text_of(text, 't1')}")
    check("3: Individual: the second hit on the same player is its own number (two on screen)", nodes(text, "n2") == 2, str(nodes(text, "n2")))
    check("3: the first (750 ms) is gone while the second lives (820 ms after the first hit)", nodes(text, "n35") == 1, str(nodes(text, "n35")))
    check("3: both are gone after 1 s", nodes(text, "n5") == 0, str(nodes(text, "n5")))


def grep_of(text, label):
    """how many of the selector's matches contain the needle (the grep probe's hits)"""
    m = re.search(r"\[input_script\] grep t=\d+ @%s needle=\S+ n=\d+ hits=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else None


def case_individual():
    s = ["0 menu", "3000 nodes @menu .mobMenuAcc--main-menu", "3100 play", "4000 nodes @ingame .frvr-in-game-ui__health-holder",
         "4100 con bot 2", "4500 touch 320 180", "4800 vis @cross knStrikeCross"] + hits(5000) + ["6500 quit"]
    text, _ = run("individual", "\n".join(s) + "\n", seed={"damageNumberDisplayMode": 1})
    check("3: the mobile layout (a touch) keeps the crosshair", vis_of(text, "cross", "knStrikeCross") == "1", str(vis_of(text, "cross", "knStrikeCross")))
    check("3: Individual: a hit of 40 shows one number", nodes(text, "n1") == 1 and text_of(text, "t1") == (1, "40"), f"{nodes(text, 'n1')} {text_of(text, 't1')}")
    check("3: Individual: the second hit on the same player is its own number (two on screen)", nodes(text, "n2") == 2, str(nodes(text, "n2")))
    check("3: the first (750 ms) is gone while the second lives (820 ms after the first hit)", nodes(text, "n35") == 1, str(nodes(text, "n35")))
    check("3: both are gone after 1 s", nodes(text, "n5") == 0, str(nodes(text, "n5")))


def main():
    case_legacy_default()
    case_dropdown_stacking()
    case_individual()
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
