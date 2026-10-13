#!/usr/bin/env python3
"""Developer console geometry and F5 full reload end-to-end test (client/src/ui/ui_console.c
con_fit / restore_geom / console_reset, client/include/win_geom.h, client/src/game_reload.c).

  python3 -I server/tests/console_reload_e2e.py BUILD_DIR      (run from the repository root)

Console (headless client_bin --sandbox in the menu, input scripts, private XDG_CONFIG_HOME):
  1. off-screen save: krk_dev_console_geom {left 5000, top 5000, 3000 x 2000} -> the console opens
     inside the 1280x720 window, shrunk to it; a typed `echo` runs; a grip resize past the edges
     stops at them; a titlebar drag past the bottom-right corner keeps all of it inside; the save
     after the drag is that box. Restarted on that save: the same box (no 14 px growth);
     `console_reset`: the default place and size, the save deleted
  2. resize: a box saved at 400,300 in 1280x720; the window resized to 800x450 while the console
     is hidden, then to 640x300 while it is shown: inside each time, the save untouched
  3. fullscreen: --fullscreen with a box saved at 1100,700; F11 back to the 800x450 window: inside
Reload (headless client_bin --offline --map kanji on a private assets tree: textures/ holds
symlinks to the real files, shaders/ is a copy):
  4. in place: while the game runs, knSettings gets another scaleUI and every texture file turns
     into a magenta PNG; F5: no restart, the setting applies (#uiBase scale), the world is magenta,
     the offline game restarts on Kanji and the player moves
  5. restart fallback: `offline Burg`, then a shader changes; F5 re-executes the client, which
     comes back offline on Burg and plays
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device
(KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_png import magenta_share, write_solid  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-reload-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
GEOM = "krk_dev_console_geom.json"
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def cfg_dir(name):
    return os.path.join(WORK, f"{name}-cfg")


def store(name):
    path = os.path.join(cfg_dir(name), "krunknative")
    os.makedirs(path, exist_ok=True)
    return path


def run_client(name, args, script, size="1280x720", env_extra=None, actions=(), timeout=150, cfg=None):
    """client_bin headless with an input script; `actions` = [(regex, fn)]: fn() runs once, the
    first time a log line matches. `cfg`: another run's config dir. Returns the log lines."""
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg_dir(cfg or name),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": size})
    env.update(env_extra or {})
    pending = list(actions)
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        deadline = time.time() + timeout
        while p.poll() is None:
            if time.time() > deadline:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
                break
            if pending:
                text = read(log)
                for item in list(pending):
                    if re.search(item[0], text, re.M):
                        pending.remove(item)
                        item[1]()
            time.sleep(0.05)
    return read(log).splitlines()


def boxes(lines, label):
    """input_script `rects @label <selector>` -> [(x, y, w, h)] of the first match per line"""
    out = []
    for line in lines:
        m = re.search(r"\[input_script\] rects t=\d+ @" + re.escape(label) + r" n=(\d+)(.*)$", line)
        if not m:
            continue
        b = re.search(r"@(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)", m.group(2))
        out.append(tuple(float(v) for v in b.groups()) if b else None)
    return out


def box(lines, label):
    found = boxes(lines, label)
    return found[-1] if found else None


def inside(b, vw, vh):
    return b is not None and b[0] >= -0.5 and b[1] >= -0.5 and b[0] + b[2] <= vw + 0.5 and b[1] + b[3] <= vh + 0.5


def near(b, want, tol=1.5):
    return b is not None and all(abs(a - w) <= tol for a, w in zip(b, want))


def saved_geom(name):
    text = read(os.path.join(store(name), GEOM))
    try:
        return json.loads(text) if text else None
    except ValueError:
        return None


CONSOLE = ".modal[data-kn-pointer]"  # the console's box (#devConsoleOverlay > .modal)


# ---------------------------------------------------------------- console geometry

def console_offscreen():
    with open(os.path.join(store("geom"), GEOM), "w") as f:
        f.write('{"left":5000,"top":5000,"width":3000,"height":2000}')
    lines = run_client("geom", ["--sandbox"],
                       "0 menu\n200 down GRAVE\n250 up GRAVE\n"
                       f"1000 rects @open {CONSOLE}\n"
                       "1100 type echo console-usable-42\n1200 down ENTER\n1250 up ENTER\n"
                       # the grip (bottom-right 14 px) to 500,300, then the titlebar past the corner
                       "1500 cursor 1273 713\n1550 mdown left\n1650 cursor 900 500\n1750 cursor 500 300\n1850 mup left\n"
                       f"1950 rects @grip {CONSOLE}\n"
                       # the grip again, far past the bottom-right corner, then back to 500,300
                       "2000 cursor 500 300\n2050 mdown left\n2150 cursor 1800 1000\n2250 cursor 3000 3000\n2300 mup left\n"
                       f"2400 rects @gripedge {CONSOLE}\n"
                       "2450 cursor 1273 713\n2500 mdown left\n2550 cursor 500 300\n2600 mup left\n"
                       "2700 cursor 200 15\n2750 mdown left\n2850 cursor 900 600\n2950 cursor 3000 3000\n3050 mup left\n"
                       f"3200 rects @drag {CONSOLE}\n"
                       "3700 quit\n")
    opened = box(lines, "open")
    check("off-screen save: the console opens inside the window, shrunk to it", near(opened, (0, 0, 1280, 720)), f"box {opened}")
    check("off-screen save: the console runs a typed command", any("console-usable-42" in l and "echo" not in l for l in lines))
    grip = box(lines, "grip")
    check("grip resize: the box follows the grip", near(grip, (0, 0, 507, 307)), f"box {grip}")
    edge = box(lines, "gripedge")
    check("grip past the edges: stops at the window's right / bottom edge", inside(edge, 1280, 720) and edge and
          abs(edge[0] + edge[2] - 1280) < 1.5 and abs(edge[1] + edge[3] - 720) < 1.5, f"box {edge}")
    drag = box(lines, "drag")
    check("titlebar drag past the corner: all of it stays inside", inside(drag, 1280, 720) and drag and
          abs(drag[0] + drag[2] - 1280) < 1.5 and abs(drag[1] + drag[3] - 720) < 1.5, f"box {drag}")
    geom = saved_geom("geom")
    check("the drag saves the box it ended at (outer size)", geom and drag and
          near((geom["left"], geom["top"], geom["width"], geom["height"]), drag), f"saved {geom}, box {drag}")

    # restarted on that save: the same box; console_reset: the default
    lines = run_client("geom-again", ["--sandbox"],
                       f"0 menu\n200 down GRAVE\n250 up GRAVE\n1000 rects @again {CONSOLE}\n"
                       f"1100 con console_reset\n1500 rects @reset {CONSOLE}\n2000 quit\n", cfg="geom")
    again = box(lines, "again")
    check("restored from the save: the same box (no growth by the padding)", near(again, drag), f"box {again}, saved {drag}")
    reset = box(lines, "reset")
    check("console_reset: back at the default place and size (758x294 at 514,8)", near(reset, (514, 8, 758, 294)), f"box {reset}")
    check("console_reset: the saved geometry is gone", not os.path.exists(os.path.join(store("geom"), GEOM)))


def console_resize():
    saved = '{"left":400,"top":300,"width":758,"height":294}'
    with open(os.path.join(store("resize"), GEOM), "w") as f:
        f.write(saved)
    lines = run_client("resize", ["--sandbox"],
                       f"0 menu\n200 down GRAVE\n250 up GRAVE\n1000 rects @a {CONSOLE}\n"
                       "1100 down GRAVE\n1150 up GRAVE\n1300 resize 800 450\n"
                       f"2000 down GRAVE\n2050 up GRAVE\n2600 rects @b {CONSOLE}\n"
                       f"2700 resize 640 300\n3500 rects @c {CONSOLE}\n4000 quit\n")
    a = box(lines, "a")
    check("resize: a saved box that fits stays where it was", near(a, (400, 300, 758, 294)), f"box {a}")
    b = box(lines, "b")
    check("resize to 800x450 while hidden: inside when shown", near(b, (42, 156, 758, 294)), f"box {b}")
    c = box(lines, "c")
    check("resize to 640x300 while shown: inside, shrunk to the window width", inside(c, 640, 300) and near(c, (0, 6, 640, 294)), f"box {c}")
    check("resize: the automatic fits leave the save alone", read(os.path.join(store("resize"), GEOM)) == saved,
          read(os.path.join(store("resize"), GEOM)))


def console_fullscreen():
    with open(os.path.join(store("fs"), GEOM), "w") as f:
        f.write('{"left":1100,"top":700,"width":758,"height":294}')
    lines = run_client("fs", ["--sandbox", "--fullscreen"],
                       f"0 menu\n200 down GRAVE\n250 up GRAVE\n1000 rects @full {CONSOLE}\n"
                       f"1200 down F11\n1250 up F11\n2500 rects @windowed {CONSOLE}\n3000 quit\n", size="800x450")
    sizes = [tuple(map(int, m.groups())) for m in (re.search(r"pbuffer resized to (\d+)x(\d+)", l) for l in lines) if m]
    full = box(lines, "full")
    vw, vh = sizes[0] if sizes else (0, 0)
    check("fullscreen: the saved box is inside the monitor", vw >= 1858 and vh >= 994 and inside(full, vw, vh),
          f"monitor {vw}x{vh}, box {full}")
    windowed = box(lines, "windowed")
    check("F11 back to the 800x450 window: the console is inside", inside(windowed, 800, 450) and near(windowed, (42, 156, 758, 294)),
          f"box {windowed}, sizes {sizes}")


# ---------------------------------------------------------------- reload

def private_assets(name):
    """assets/ with textures/ as a tree of symlinks (repointable) and shaders/ copied"""
    root = os.path.join(WORK, f"{name}-assets")
    real = os.path.realpath("assets")
    os.makedirs(root)
    links = []
    for entry in os.listdir(real):
        src, dst = os.path.realpath(os.path.join(real, entry)), os.path.join(root, entry)
        if entry == "textures":
            for d, _, files in os.walk(src, followlinks=True):
                out = os.path.join(dst, os.path.relpath(d, src))
                os.makedirs(out, exist_ok=True)
                for f in files:
                    os.symlink(os.path.join(d, f), os.path.join(out, f))
                    links.append(os.path.join(out, f))
        elif entry == "shaders":
            shutil.copytree(src, dst)
        else:
            os.symlink(src, dst)
    return root, links


def states(lines):
    out = []
    for line in lines:
        m = re.search(r"\[input_script\] state t=(\d+) slot=(-?\d+)/(\d+) weapon=(.+?) menu=(\d) locked=(\d) pos=(\S+)", line)
        if m:
            out.append({"t": int(m.group(1)), "loadout": int(m.group(3)), "weapon": m.group(4), "menu": int(m.group(5)),
                        "pos": m.group(7)})
    return out


def in_game(state):
    return state and state["menu"] == 0 and state["loadout"] > 0 and state["weapon"] != "-"


def ui_scale(lines, label):
    for line in lines:
        m = re.search(r"\[input_script\] rect t=\d+ @" + label + r" uiBase=([\d.-]+),([\d.-]+),([\d.-]+),([\d.-]+),([\d.-]+),([\d.-]+)", line)
        if m and float(m.group(5)) > 0:
            return float(m.group(3)) / float(m.group(5))
    return None


def reload_in_place():
    assets, links = private_assets("inplace")
    magenta = os.path.join(WORK, "magenta.png")
    write_solid(magenta, (255, 0, 255, 255))
    settings = os.path.join(store("inplace"), "knSettings.json")
    before_shot, after_shot = os.path.join(WORK, "before.png"), os.path.join(WORK, "after.png")

    def change_files():
        current = {}
        try:
            current = json.loads(read(settings) or "{}")
        except ValueError:
            pass
        current["scaleUI"] = 0.5 if current.get("scaleUI") != 0.5 else 0.9
        with open(settings, "w") as f:
            f.write(json.dumps(current))
        for link in links:
            tmp = link + ".kn-tmp"
            os.symlink(magenta, tmp)
            os.replace(tmp, link)
        print(f"  (changed knSettings scaleUI -> {current['scaleUI']} and {len(links)} texture files)", flush=True)

    lines = run_client("inplace", ["--offline", "--map", "kanji"],
                       f"0 play\n1500 rect @before uiBase\n1500 shot {before_shot}\n2100 rect @mark uiBase\n"
                       "9000 down F5\n9050 up F5\n"
                       f"14000 state\n14100 down W\n15100 up W\n15200 state\n15300 rect @after uiBase\n15300 shot {after_shot}\n"
                       "16000 quit\n",
                       env_extra={"KRUNKNATIVE_ASSETS_DIR": assets}, actions=[(r"\[input_script\] rect t=\d+ @mark ", change_files)])
    st = states(lines)
    check("reload: the offline game runs on Kanji", any("[sandbox] Loaded map Kanji" in l for l in lines) and st and in_game(st[0]),
          f"states {st[:1]}")
    after = [l for l in lines if "[reload]" in l]
    check("reload: F5 reloads in place (no restart)", any("[controls] reload key" in l for l in lines) and
          any("full reload" in l for l in after) and not any("[reload] restart" in l for l in after), "; ".join(after))
    m = next((re.search(r"textures: (\d+), OBJ models: (\d+)", l) for l in after if "textures:" in l), None)
    check("reload: the cached textures and models were read again", m and int(m.group(1)) > 0, "; ".join(after))
    check("reload: the setting change was picked up", any(re.search(r"settings: [1-9]\d* changed", l) for l in after))
    s0, s1 = ui_scale(lines, "before"), ui_scale(lines, "after")
    check("reload: the new scaleUI applies (#uiBase scale)", s0 and s1 and abs(s1 - s0) > 0.05 * s0, f"scale {s0} -> {s1}")
    m0, m1 = magenta_share(before_shot), magenta_share(after_shot)
    check("reload: the changed texture files show (magenta world)", m0 >= 0 and m1 >= 0.15 and m1 > m0 + 0.1,
          f"magenta {m0:.3f} -> {m1:.3f}")
    check("reload: the offline game restarted on Kanji", any("Restarted the offline game" in l for l in lines) and
          sum("[sandbox] Loaded map" in l for l in lines) >= 2)
    late = [s for s in st if s["t"] >= 14000]
    check("reload: the game still works (in game, the player moves)", len(late) >= 2 and in_game(late[0]) and in_game(late[1]) and
          late[0]["pos"] != late[1]["pos"], f"states {late}")
    check("reload: no errors", not any(re.search(r"shader .*error|failed to reload|Segmentation|AddressSanitizer", l) for l in lines))


def reload_restart():
    assets, _ = private_assets("restart")
    shader = os.path.join(assets, "shaders", "basic.frag")

    def change_shader():
        with open(shader, "a") as f:
            f.write("\n// changed by console_reload_e2e\n")
        print("  (changed shaders/basic.frag)", flush=True)

    lines = run_client("restart", ["--offline", "--map", "kanji"],
                       "0 play\n1000 con offline Burg\n4000 state\n4100 rect @mark uiBase\n"
                       "8000 down F5\n8050 up F5\n"
                       "12000 state\n13000 quit\n",
                       env_extra={"KRUNKNATIVE_ASSETS_DIR": assets}, actions=[(r"\[input_script\] rect t=\d+ @mark ", change_shader)],
                       timeout=240)
    restart = [l for l in lines if "[reload]" in l]
    check("restart: a changed shader makes F5 restart the client", any(re.search(r"restart: shader .*basic\.frag changed", l) for l in restart),
          "; ".join(restart))
    check("restart: the new process comes back to the offline game on Burg",
          any(re.search(r"resume after the restart: offline \S+", l) for l in restart) and any("resume: offline" in l for l in restart),
          "; ".join(restart))
    runs = "\n".join(lines).split("resume after the restart")
    first, second = runs[0], runs[1] if len(runs) > 1 else ""
    burg = re.findall(r"\[sandbox\] Loaded map (\d+) mode", first.split("] offline Burg")[-1])
    check("restart: Burg loaded again after the restart", burg and re.search(r"\[sandbox\] Loaded map %s mode" % burg[0],
          second.split("resume: offline")[-1]) is not None, f"Burg is map {burg}")
    st = states(second.splitlines())
    check("restart: in game after the restart (the script's F5 there reloads in place)",
          any(in_game(s) for s in st) and "reload] restart" not in second.split("full reload")[-1], f"states {st}")


def main():
    try:
        console_offscreen()
        console_resize()
        console_fullscreen()
        reload_in_place()
        reload_restart()
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
        if all(results) and results:
            shutil.rmtree(WORK, ignore_errors=True)
        else:
            print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()
