#!/usr/bin/env python3
"""Krunker Strike's Settings > Display tab: V-Sync and Frame Cap are there, change, and reach the client
(client/src/ui/strike_menu.c settings_insert_vsync and h_strike_setting, client/src/ui/ui_settings.c
push_strike_frame_cap, client/src/ui/ui_render_settings.c client_apply_render_setting, client_render_frame_end).

  python3 -I client/tests/strike_vsync_e2e.py BUILD_DIR      (run from the repository root)

Headless --flavor strike --offline clients on the Kanji map, each with a private XDG_CONFIG_HOME (the Strike
store is krunknative-strike, Box Office's is krunknative). The script clock is per frame
(KRUNKNATIVE_FIXED_DELTA); the fps lines come one per wall-clock second, so the scripts run long enough for
several of them before and after each change.
  1. a fresh store: the Display tab has a V-Sync switch, on, and Frame Cap at 0 (uncapped, the default when nothing
     is stored: the fps lines say cap 0 before anything changes). A click on V-Sync turns it
     off: the Strike store keeps "vsync": false, Box Office's store is not written, the preset is not changed
     (no strikePreIndex), and the fps lines after the click say vsync 0. Frame Cap 30 (setSetting "updateRate")
     reaches the client: the fps lines say cap 30 and the store keeps it.
  2. a stored Strike store {"vsync": false, "updateRate": 0}: the client starts uncapped and unsynced, and the
     Display tab shows V-Sync off and Frame Cap 0 (saved values win over the default).
  3. the Fullscreen Mode button calls strikeAction('fullscreen').
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it). Prints PASS/FAIL
lines; exit code 0 only when everything passed.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-vsync-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []

FPS = re.compile(r"\[render\] fps \d+ \(cap (\d+)(?: headless)?, vsync (\d), swap \d(?:, display \d+ Hz)?\)")
VSYNC_CLICK = "[ui] handler strikeSetting('vsync') click"
SETTING_30 = "setSetting argc=2"


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def run_client(name, script, stores=None, timeout=300):
    """One headless Strike client; returns (exit code, log text, log path)."""
    base = os.path.join(WORK, name)
    cfg = os.path.join(base, "cfg")
    # a returning player: a fresh config starts the first run's controls tutorial when Settings opens (strike_ftue.c
    # strike_ftue_first_run), which replaces the menu screens and their Display rows (strike_input_e2e.py does the same)
    os.makedirs(os.path.join(cfg, "krunknative-strike"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative-strike", "knFtue.json"), "w") as f:
        f.write('{"guest":{"played":true}}')
    for sub, text in (stores or {}).items():
        path = os.path.join(cfg, sub, "knSettings.json")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    os.makedirs(base, exist_ok=True)
    script_path = os.path.join(base, "input.script")
    with open(script_path, "w") as f:
        f.write(script)
    drop = ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_VSYNC", "KRUNKNATIVE_FRAME_CAP", "KRUNKNATIVE_BRAIN_URL", "KN_FLAVOR")
    env = {k: v for k, v in os.environ.items() if k not in drop}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(base, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_FPS_LOG": "1", "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_ASSETS_DIR": os.path.abspath("assets") + os.sep})
    log = os.path.join(base, "client.log")
    with open(log, "w") as out:
        try:
            code = subprocess.run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline", "--map", "kanji"],
                                  stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            code = None
    with open(log, errors="replace") as f:
        return code, f.read(), log


def read_store(name, sub):
    try:
        with open(os.path.join(WORK, name, "cfg", sub, "knSettings.json")) as f:
            return json.loads(f.read() or "{}")
    except (OSError, ValueError):
        return {}


def fps_lines(text, after=None, before=None):
    """(cap, vsync) of each fps line, limited to the log between two marker lines."""
    if after is not None:
        i = text.find(after)
        text = text[i:] if i >= 0 else ""
    if before is not None:
        j = text.find(before)
        text = text[:j] if j >= 0 else text
    return FPS.findall(text)


def find(text, pattern):
    return re.search(pattern, text)


def main():
    # 1. fresh store: the rows, the default cap, a V-Sync click that reaches the client, a Frame Cap of 30
    script = """0 menu
0 waitfor [id=mobMenuSett]
800 call strikeAction "settings"
1000 call strikeAction "settings-tab:1"
1300 text @sw [id=knVsync]
1310 text @frame [id=slid_input_updateRate]
1320 grep @rows Cap .settName
1330 grep @rows Fullscreen .settName
1340 attr @rows onclick [onclick*=fullscreen]
6000 clicksel [id=knVsync]
6400 text @off [id=knVsync]
9000 call setSetting "updateRate" "30"
10500 call strikeAction "settings-tab:0"
11000 call strikeAction "settings-tab:1"
11500 text @reopen [id=knVsync]
11510 text @reopen_frame [id=slid_input_updateRate]
18000 quit
"""
    code, text, log = run_client("fresh", script)
    sw = find(text, r"\[input_script\] text t=\d+ @sw n=(\d+) checked=(\d) ")
    check("1. the Display tab has a V-Sync switch, on at first", sw and sw.group(1) == "1" and sw.group(2) == "1", log)
    frame = find(text, r"\[input_script\] text t=\d+ @frame n=\d+ checked=\d value=(\S*)")
    check("1. Frame Cap is on the Display tab at 0 (nothing stored)", frame and frame.group(1) == "0", log)
    cap = find(text, r"\[input_script\] grep t=\d+ @rows needle=Cap n=\d+ hits=(\d+)")
    check("1. the Frame Cap row is there", cap and int(cap.group(1)) >= 1, log)
    fs = find(text, r"\[input_script\] grep t=\d+ @rows needle=Fullscreen n=\d+ hits=(\d+)")
    check("1. the Fullscreen Mode row is there", fs and int(fs.group(1)) >= 1, log)
    attr = find(text, r"\[input_script\] attr t=\d+ @rows n=\d+ onclick=(.*)")
    check("1. the Fullscreen Mode button calls strikeAction('fullscreen')",
          attr and attr.group(1).strip() == "strikeAction('fullscreen')", log)
    before = fps_lines(text, before=VSYNC_CLICK)
    check("1. before the click the fps lines say cap 0, vsync 1",
          before and all(c == "0" and v == "1" for c, v in before), f"{before[:3]} log {log}")
    check("1. a click on V-Sync reaches the switch's handler", VSYNC_CLICK in text, log)
    off = find(text, r"\[input_script\] text t=\d+ @off n=\d+ checked=(\d)")
    check("1. the switch reads off after the click", off and off.group(1) == "0", log)
    st = read_store("fresh", "krunknative-strike")
    check("1. the Strike store keeps \"vsync\": false", st.get("vsync") is False, str(st))
    check("1. Box Office's store is not written by it", not os.path.exists(os.path.join(WORK, "fresh", "cfg", "krunknative", "knSettings.json")), log)
    check("1. the preset is not changed by V-Sync (no strikePreIndex)",
          not os.path.exists(os.path.join(WORK, "fresh", "cfg", "krunknative-strike", "strikePreIndex.json")), log)
    after_click = fps_lines(text, after=VSYNC_CLICK)
    check("1. the fps lines after the click say vsync 0", after_click and all(v == "0" for _, v in after_click),
          f"{after_click[:3]} log {log}")
    capped = fps_lines(text, after=SETTING_30)
    check("1. Frame Cap 30 (setSetting) reaches the client: the fps lines say cap 30",
          capped and all(c == "30" for c, _ in capped), f"{capped[:3]} log {log}")
    check("1. the Strike store keeps Frame Cap 30", read_store("fresh", "krunknative-strike").get("updateRate") == 30,
          str(read_store("fresh", "krunknative-strike")))
    reopen = find(text, r"\[input_script\] text t=\d+ @reopen n=\d+ checked=(\d)")
    reopen_frame = find(text, r"\[input_script\] text t=\d+ @reopen_frame n=\d+ checked=\d value=(\S*)")
    check("1. reopening the Display tab keeps V-Sync off and shows Frame Cap 30",
          reopen and reopen.group(1) == "0" and reopen_frame and reopen_frame.group(1) == "30", log)
    check("1. the client exits cleanly", code == 0, f"exit {code}, log {log}")

    # 2. a stored Strike store: the saved values win over the default, and the Display tab shows them
    script2 = """0 menu
0 waitfor [id=mobMenuSett]
800 call strikeAction "settings"
1000 call strikeAction "settings-tab:1"
1300 text @sw [id=knVsync]
1310 text @frame [id=slid_input_updateRate]
6000 quit
"""
    code, text, log = run_client("stored", script2, stores={"krunknative-strike": '{"vsync": false, "updateRate": 0}'})
    lines = fps_lines(text)
    check("2. a stored vsync false and updateRate 0: the fps lines say cap 0, vsync 0",
          lines and all(c == "0" and v == "0" for c, v in lines), f"{lines[:2]} log {log}")
    row = find(text, r"\[input_script\] text t=\d+ @sw n=\d+ checked=(\d)")
    check("2. the Display tab shows V-Sync off", row and row.group(1) == "0", log)
    frame = find(text, r"\[input_script\] text t=\d+ @frame n=\d+ checked=\d value=(\S*)")
    check("2. the Display tab shows Frame Cap 0", frame and frame.group(1) == "0", log)
    saved = read_store("stored", "krunknative-strike")
    check("2. the stored Strike store keeps its values", saved.get("vsync") is False and saved.get("updateRate") == 0,
          str(saved))

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
