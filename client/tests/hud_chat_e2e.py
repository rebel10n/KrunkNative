#!/usr/bin/env python3
"""KrunkNative HUD chat / feed / reload end-to-end (client/src/ui/hud_ui.c, server/src/config.c).

  python3 -I client/tests/hud_chat_e2e.py BUILD_DIR          (run from the repository root)

Offline (the in-process sandbox server), headless, fixed 16.667 ms frames:
  1. welcome: no chat line for the game's welcome (the server's motd defaults to none); the map's
     welMsg is the menu's line (#mapWelcome: Krunk Plaza, then Soul Sanctum after a map switch, nothing
     on a map without welMsg, Kanji) and not a chat line
  2. deaths on a desktop: the kill-feed row goes to #killFeed (v10 leaderType 1), no chat row and no
     toast (#knNotify)
  3. deaths on a phone (KRUNKNATIVE_MOBILE=1, oldScoreboard's v10 default !!isMobile): the row is a
     chat line in #chatList, #killFeed stays empty, no toast
  4. reload: #ammoHolder's background-size follows reloadUIAnim every frame, round(reloadTimer /
     reload time * 100) + "% 100%", from ~100 down to "0%" when the reload ends (no frame skipped)
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
WORK = tempfile.mkdtemp(prefix="kn-hudchat-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
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


def run_client(tag, args, script, extra_env=None, timeout=240):
    """client_bin headless with the input script; returns its log lines"""
    events = sorted((l for l in script if l.strip()), key=lambda l: int(l.split()[0]))
    script_path = os.path.join(WORK, tag + ".script")
    with open(script_path, "w") as f:
        f.write("\n".join(events) + "\n")
    log = os.path.join(WORK, tag + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, tag + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_FIXED_DELTA": "16.667", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    env.update(extra_env or {})
    # a worktree lacks the untracked font / asset dirs: the main checkout's assets (RELAUNCH notes)
    env.setdefault("KRUNKNATIVE_ASSETS_DIR", "/home/user/src/KrunkNative/assets")
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{tag}: client_bin finished its script", False, f"timeout, log {log}")
    return read(log).splitlines()


def probe(lines, cmd, label):
    """the fields of the first `[input_script] <cmd> ... @label ...` line"""
    for l in lines:
        if l.startswith(f"[input_script] {cmd} ") and f" {label} " in l + " ":
            return dict(kv.split("=", 1) for kv in l.split() if "=" in kv)
    return None


def hits(lines, label):
    g = probe(lines, "grep", label)
    return int(g["hits"]) if g else -1


def match(lines, label):
    g = probe(lines, "nodes", label)
    return int(g["match"]) if g else -1


def menu_text(lines, label):
    """the text of the first `[input_script] text ... @label` line (None when not logged)"""
    for l in lines:
        if l.startswith("[input_script] text ") and f" {label} " in l + " ":
            return l.split(" text=", 1)[1] if " text=" in l else ""
    return None


def welcome():
    # the map's welMsg is the menu's line (the Now Playing block's map), never a chat line; the game's
    # own "Welcome to Krunker Box Office" line is gone (server motd defaults to none)
    script = ["0 play",
              "2000 grep @w0 Welcome [id^=chatMsg_]", "2000 grep @kbo0 Box [id^=chatMsg_]",
              "2000 grep @p0 Plaza [id^=chatMsg_]",
              "2100 down ESC", "2110 up ESC",
              "2600 text @m0 [id=mapWelcome]",
              "3000 con map Soul Sanctum",
              "6000 down ESC", "6010 up ESC",                 # a map switch leaves the menu closed: reopen it
              "8000 text @m1 [id=mapWelcome]", "8000 grep @s1 Sanctum [id^=chatMsg_]",
              "9000 con map Kanji",
              "12000 down ESC", "12010 up ESC",
              "14000 text @m2 [id=mapWelcome]", "14000 text @k2 [id=mapInfo]",
              "14000 grep @w2 Welcome [id^=chatMsg_]",
              "14100 quit"]
    lines = run_client("welcome", ["--offline", "assets/maps/krunk_plaza_v2.json"], script)
    rows = [l for l in lines if "[input_script] grep" in l]
    m0, m1, m2 = menu_text(lines, "@m0"), menu_text(lines, "@m1"), menu_text(lines, "@m2")
    check("welcome: the map's welMsg shows in the menu (Krunk Plaza)",
          m0 is not None and "Welcome to the Krunker Plaza" in m0, f"menu text {m0!r}")
    check("welcome: no Box Office welcome line in chat", hits(lines, "@w0") == 0 and hits(lines, "@kbo0") == 0,
          f"{rows[:3]}")
    check("welcome: the map's welMsg is not a chat line", hits(lines, "@p0") == 0, f"rows {rows[:3]}")
    check("welcome: the next map's welMsg replaces it in the menu (Soul Sanctum)",
          m1 is not None and "You have entered the Soul Sanctum" in m1 and hits(lines, "@s1") == 0,
          f"menu text {m1!r}, Sanctum chat rows {hits(lines, '@s1')}")
    k2 = menu_text(lines, "@k2")
    check("welcome: a map without welMsg shows nothing in the menu (Kanji)",
          m2 == "" and hits(lines, "@w2") == 0 and k2 is not None and "Kanji" in k2,
          f"menu text {m2!r}, map info {k2!r}, chat rows {hits(lines, '@w2')}")


def deaths(tag, mobile):
    script = ["0 play",
              "2000 nodes @f0 [id^=feed_]", "2000 nodes @c0 [id^=chatMsg_]",
              "2200 con kill",
              "3000 nodes @f1 [id^=feed_]", "3000 nodes @c1 [id^=chatMsg_]",
              "3000 grep @k1 committed [id^=chatMsg_]",
              "3000 vis @n1 knNotify",
              "3100 quit"]
    lines = run_client(tag, ["--offline", "assets/maps/kanji.json"], script, {"KRUNKNATIVE_MOBILE": "1"} if mobile else None)
    f0, f1, c0, c1 = match(lines, "@f0"), match(lines, "@f1"), match(lines, "@c0"), match(lines, "@c1")
    vis = probe(lines, "vis", "@n1") or {}
    toast = vis.get("knNotify", "-")
    if mobile:
        check(f"{tag}: the death is a chat line (Classic io Scoreboard, v10's phone default)",
              c0 >= 0 and c1 == c0 + 1 and hits(lines, "@k1") == 1, f"chat rows {c0} -> {c1}, committed {hits(lines, '@k1')}")
        check(f"{tag}: no kill-feed row", f0 == 0 and f1 == 0, f"feed rows {f0} -> {f1}")
    else:
        check(f"{tag}: the death is a #killFeed row (v10 desktop leaderType 1)", f0 == 0 and f1 == 1, f"feed rows {f0} -> {f1}")
        check(f"{tag}: no chat row for it", c0 >= 0 and c1 == c0, f"chat rows {c0} -> {c1}")
    check(f"{tag}: no toast", toast in ("-", "0"), f"knNotify={toast}")


def reload():
    script = ["0 play", "800 mdown left", "900 mup left", "1500 down R", "1600 up R"]
    for t in range(1500, 3600, 17):
        script += [f"{t} state", f"{t} style @r ammoHolder:background-size"]
    script.append("3700 quit")
    lines = run_client("reload", ["--offline", "assets/maps/kanji.json"], script)
    reload_at, size_at = {}, {}
    for l in lines:
        m = re.match(r"\[input_script\] state t=(\d+) .* reload=([\d.]+)", l)
        if m:
            reload_at.setdefault(int(m.group(1)), float(m.group(2)))
        m = re.match(r"\[input_script\] style t=(\d+) @r ammoHolder:background-size=(\S+)", l)
        if m:
            size_at.setdefault(int(m.group(1)), m.group(2))
    frames = sorted(t for t in reload_at if t in size_at)
    active = [t for t in frames if reload_at[t] > 0.0]
    pct = {}
    for t in frames:
        m = re.match(r"(\d+)%_100%$", size_at[t])
        pct[t] = int(m.group(1)) if m else (0 if size_at[t] in ("0%", "-") else None)
    check("reload: sampled every frame of the reload", len(active) >= 40, f"{len(active)} frames with a reload timer")
    if len(active) < 2:
        return
    # reloadUIAnim: pct = round(reloadTimer / total * 100); total from the samples (the timer is shown to 0.01 s)
    ratios = sorted(pct[t] / reload_at[t] for t in active if pct[t] and reload_at[t] > 0.2)
    k = ratios[len(ratios) // 2] if ratios else 0
    worst = max((abs(pct[t] - reload_at[t] * k) for t in active if pct[t] is not None and reload_at[t] > 0.05), default=99)
    check("reload: the bar is reloadTimer / reload time every frame", ratios and worst <= 2.0,
          f"total {100 / k if k else 0:.3f} s, worst {worst:.2f} points")
    seq = [pct[t] for t in active]
    check("reload: the bar starts near full", seq[0] is not None and seq[0] >= 90, f"first {seq[0]}")
    steps = [b - a for a, b in zip(seq, seq[1:]) if a is not None and b is not None]
    check("reload: it drains smoothly (never grows, never skips a frame, < 3 points a frame)",
          steps and all(-3 <= s < 0 for s in steps[:-1]), f"steps {sorted(set(steps))}")
    done = [t for t in frames if t > active[-1]]
    check("reload: '0%' once the reload ended", done and all(size_at[t] == "0%" for t in done),
          f"after: {[size_at[t] for t in done[:3]]}")


def main():
    if not os.path.exists(os.path.join(BUILD, "client_bin")):
        print(f"no client_bin in {BUILD}")
        return 2
    welcome()
    deaths("desktop", False)
    deaths("phone", True)
    reload()
    print(f"{sum(results)} of {len(results)} checks passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    try:
        code = main()
    finally:
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
        if os.environ.get("KN_KEEP_E2E") is None and results and all(results):
            shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(code)
