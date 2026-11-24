#!/usr/bin/env python3
"""The death card's killer weapon [skin] is rainbowText for an Unobtainable skin (deob-10.0.7.js:273175).

game_events.c's events_weapon_label marks the card's weapon rainbow when the killer's skin is Unobtainable (rarity 6,
the only rarity that animates); hud_ui.c's hud_death_html adds rainbowText unless Settings > Interface > Disable
Rarity Animations (disableRarityAnim) is on. The name keeps its rarity colour either way.

Offline sandbox, headless: a bot on class 0 with a primary skin (`con bot_add 1 skin=<i> class=0 damage=0`), then
the scripted KILL packet from that bot with the Assault Rifle (`killed 1`, weapon index 1), and the card's
.death-row-weapon-text read back:
  unob:       Disintegrator (1403, Unobtainable)             -> "[Disintegrator]", rainbowText
  unob-off:   the same with disableRarityAnim saved true     -> "[Disintegrator]", no rainbowText, the same colour
  contraband: Raynb0w (313, Contraband)                      -> "[Raynb0w]", no rainbowText

  python3 -I client/tests/death_rainbow_e2e.py BUILD_DIR [SRC]

77 = skip (no client_bin).
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.getcwd())
CLIENT = os.path.join(BUILD, "client_bin")
if not os.access(CLIENT, os.X_OK):
    print("death_rainbow_e2e: no client_bin: skipped")
    sys.exit(77)
WORK = tempfile.mkdtemp(prefix="kn-death-rainbow.", dir=os.path.expanduser("~/.cache"))
FAILS = []
DISINTEGRATOR, RAYNB0W = 1403, 313  # assets/skins.json: Assault Rifle skins (weapon 2), rarity 6 and 5


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % str(detail)[:400]), flush=True)
    if not ok:
        FAILS.append(what)


def run(tag, skin, settings=None):
    cfg = os.path.join(WORK, tag + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    if settings:
        with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
            json.dump(settings, f)
    script = os.path.join(WORK, tag + ".script")
    with open(script, "w") as f:
        f.write("0 play\n200 con bot_add 1 skin=%d class=0 damage=0\n3000 players\n3100 killed 1\n"
                "3800 vis @dead deathUIHolder\n3800 rows @rows .death-row-weapon-text\n"
                "3800 attr @cls class .death-row-weapon-text\n3800 attr @style style .death-row-weapon-text\n3900 quit\n" % skin)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_FINDER_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_EGL_DEVICE": os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"),
                "LIBGL_ALWAYS_SOFTWARE": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state")})
    log = os.path.join(WORK, tag + ".log")
    with open(log, "w") as out:
        p = subprocess.Popen([CLIENT, "--offline", "--map", "kanji"], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            rc = p.wait(timeout=150)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = "timeout"
    with open(log, errors="replace") as f:
        text = f.read()
    check("%s: client_bin exits 0" % tag, rc == 0, "rc=%s, log %s" % (rc, log))
    killed = re.search(r"\[input_script\] killed t=\d+ by=(-?\d+)", text)
    check("%s: the bot is the killer" % tag, killed is not None and killed.group(1) != "-1", killed.group(0) if killed else "no killed line")
    dead = re.search(r"\[input_script\] vis t=\d+ @dead deathUIHolder=(\d)", text)
    check("%s: the death card is shown" % tag, dead is not None and dead.group(1) == "1", dead.group(0) if dead else "no vis line")
    rows = re.findall(r"\[input_script\] row t=\d+ @rows text=(.*)", text)
    cls = re.search(r"\[input_script\] attr t=\d+ @cls n=\d+ class=(.*)", text)
    style = re.search(r"\[input_script\] attr t=\d+ @style n=\d+ style=(.*)", text)
    # the first .death-row-weapon-text is the killer's (the second is ours, right of the VS)
    return (rows[0].strip() if rows else None, (cls.group(1).split() if cls else None),
            (style.group(1).strip().replace(" ", "") if style else None))


def main():
    name, cls, style = run("unob", DISINTEGRATOR)
    check("unob: the card names the killer's Disintegrator", name == "[Disintegrator]", name)
    check("unob: an Unobtainable skin is rainbowText", cls is not None and "rainbowText" in cls, cls)
    check("unob: in its rarity colour", bool(style) and style.startswith("color:#"), style)

    name2, cls2, style2 = run("unob-off", DISINTEGRATOR, {"disableRarityAnim": True})
    check("unob-off: the card still names the Disintegrator", name2 == "[Disintegrator]", name2)
    check("unob-off: disableRarityAnim turns rainbowText off", cls2 is not None and "rainbowText" not in cls2, cls2)
    check("unob-off: the rarity colour stays", style2 == style, "%r vs %r" % (style2, style))

    name3, cls3, style3 = run("contraband", RAYNB0W)
    check("contraband: the card names the Raynb0w", name3 == "[Raynb0w]", name3)
    check("contraband: a Contraband skin is not rainbowText", cls3 is not None and "rainbowText" not in cls3, cls3)
    check("contraband: a colour other than Unobtainable's", bool(style3) and style3 != style, "%r vs %r" % (style3, style))

    if FAILS:
        print("death_rainbow_e2e: %d failure(s), logs in %s" % (len(FAILS), WORK))
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    print("death_rainbow_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
