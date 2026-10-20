#!/usr/bin/env python3
"""The Physics Gun and the Tool Gun put away and back, through the real client.

  python3 -I client/tests/gun_unequip_e2e.py BUILD_DIR      (run from the repository root)

One offline Kanji game (headless client_bin, input script, fixed 60 fps step). Both guns are extra
loadout slots (the Object Manager's Tools pane); a gun in hand is put away by:
  1. the Physics Gun's tile in the Tools pane a second time (the Weapons tab's "again: take it out"),
  2. Z (the drop key on an extra slot: OM_ACTION_DROP_HELD),
  3. nothing else: a number key, the wheel or the swap key takes the hand elsewhere and the gun
     stays in its slot (checked as "not in hand, still carried").
The Tool Gun's tile does the same, and the Tools pane's Physics Gun row puts the Physics Gun back in
hand while the Tool Gun is in hand. After each put-away no gun is in hand, the hand shows the next
weapon, the other weapons still fire, and equipping again works.
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
WORK = tempfile.mkdtemp(prefix="kn-gun-unequip-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
results = []

SCRIPT = """
0 menu
300 play
500 con god
600 om physgun 0
800 state
900 om physgun 0
1100 state
1200 om physgun 0
1400 state
1500 down 1
1550 up 1
1700 state
1800 mdown left
1850 mup left
1950 state
2050 om physgun 0
2250 state
2350 down Z
2400 up Z
2600 state
2700 om tool 2
2900 state
3000 down F1
3050 up F1
3100 clickid knOmTab3
3200 clickid knOmToolGunTile
3350 state
3450 clickid knOmToolGunTile
3600 state
3700 clickid knOmPhysRow
3850 state
3950 down F1
4000 up F1
4200 down Z
4250 up Z
4400 state
4450 wsel @hand
4500 quit
"""

STATE_RE = re.compile(r"\[input_script\] state t=\d+ slot=(\d+)/(\d+) weapon=(.+?) menu=\d locked=\d .*? ammo=(\d+) ")
WSEL_RE = re.compile(r"\[input_script\] wsel t=\d+ @\S+ who=me (.*)")
# the state probes in script order
LABELS = ["phys-held", "phys-away", "phys-again", "slot-1", "fired", "phys-reequip", "z-away", "tool-held", "tool-away",
          "tool-again", "pane-phys", "end"]
PHYS, TOOL, RIFLE = "Physics Gun", "Tool Gun", "Assault Rifle"


def phys_reequip_ok(state):
    return bool(state and state["weapon"] == PHYS)


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run():
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(cfg)
    script = os.path.join(WORK, "gun-unequip.script")
    with open(script, "w") as f:
        f.write(SCRIPT.strip() + "\n")
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS),
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("the client finishes the script", False, "timeout")
    with open(log, errors="replace") as f:
        return f.read()


def main():
    text = run()
    states = {}
    for label, m in zip(LABELS, STATE_RE.finditer(text)):
        states[label] = {"slot": int(m.group(1)), "size": int(m.group(2)), "weapon": m.group(3), "ammo": int(m.group(4))}
    wsel = [m.group(1) for m in WSEL_RE.finditer(text)]
    s = states.get
    ok_states = len(states) == len(LABELS)
    check("every state probe logged", ok_states, f"{len(states)}/{len(LABELS)}")
    if not ok_states:
        print(f"logs kept in {WORK}")
        return 1

    # 1. Physics Gun: in hand, the tile's second click puts it away, the first click equips it again
    held, away, again = s("phys-held"), s("phys-away"), s("phys-again")
    check("physics gun: the tile puts it in hand", held and held["weapon"] == PHYS, str(held))
    check("physics gun: a second tile click puts it away (no gun in hand)", away and away["weapon"] != PHYS and away["weapon"] != TOOL,
          str(away))
    check("physics gun: put away leaves its extra slot", held and away and away["size"] == held["size"] - 1, f"{held} -> {away}")
    check("physics gun: equipping again works", again and again["weapon"] == PHYS and again["size"] == held["size"], str(again))

    # 2. Other weapons still work: a number key takes the hand off the gun, the rifle fires
    slot1, fired = s("slot-1"), s("fired")
    check("slot key: the gun leaves the hand, the next slot is in hand", slot1 and slot1["weapon"] == RIFLE, str(slot1))
    check("other weapon: it still fires after the gun is put away", slot1 and fired and fired["ammo"] < slot1["ammo"],
          f"{slot1 and slot1['ammo']} -> {fired and fired['ammo']}")

    # 3. Tool Gun: the tile's second click puts it away; the Physics Gun row takes the hand while it is held
    tool_held, phys_tools = s("tool-held"), s("pane-phys")
    check("tool gun: in hand from the Tools pane", tool_held and tool_held["weapon"] == TOOL, str(tool_held))
    tool_away, tool_again = s("tool-away"), s("tool-again")
    check("tool gun: a second tile click puts it away", tool_away and tool_away["weapon"] != TOOL, str(tool_away))
    check("tool gun: put away leaves its extra slot", tool_held and tool_away and tool_away["size"] == tool_held["size"] - 1,
          f"{tool_held} -> {tool_away}")
    check("tool gun: the tile equips it again", tool_again and tool_again["weapon"] == TOOL, str(tool_again))
    check("physics gun row: takes the hand from the Tool Gun", phys_tools and phys_tools["weapon"] == PHYS, str(phys_tools))

    # 4. Z puts the held gun away (the Physics Gun, then the Physics Gun again from the pane); the hand shows
    # the next weapon: the wsel probe logs the model built on the held slot (w is the weapon id)
    z_away, end = s("z-away"), s("end")
    check("drop key: the Physics Gun leaves the hand", z_away and phys_reequip_ok(s("phys-reequip")) and z_away["weapon"] != PHYS, str(z_away))
    check("drop key: the Physics Gun from the pane leaves the hand too", end and end["weapon"] != PHYS and phys_tools and end["size"] == phys_tools["size"] - 1,
          f"{phys_tools} -> {end}")
    hand = wsel[-1] if wsel else ""
    check("hand: no gun model in hand after the drop", hand and " w=66 " not in hand and " w=64 " not in hand and "weapon_25" not in hand and "weapon_12" not in hand,
          hand or "no wsel line")

    ok = all(results)
    print(f"{sum(results)}/{len(results)} passed")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
