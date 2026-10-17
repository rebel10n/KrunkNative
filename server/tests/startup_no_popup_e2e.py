#!/usr/bin/env python3
"""No first-run popup: a fresh install goes straight to the menu, on Box Office by default.

  python3 -I server/tests/startup_no_popup_e2e.py BUILD_DIR      (run from the repository root)

client/src/ui/menu_ui.c and client/src/ui/servers_ui.c (the start-up seek, Box Office as before). One headless
client_bin run at a time against a fresh profile (--offline: no network), driven by KRUNKNATIVE_INPUT_SCRIPT:

  1. desktop, fresh profile: the menu is up (Quick Match), no popup (.kn-firstrun) at any sample, no Servers
     window (windowHolder), no Connect window (connectAddr), nothing stored for the old choice (knFirstRun.json),
     and the Server Type is not krunker.io (knSeekServers is not "official");
  2. touch on a phone (844x390 and 390x844, KRUNKNATIVE_MOBILE=1), fresh profile: the same;
  3. desktop, the same profile again (the next launch): the same.

Prints PASS/FAIL lines; exit 0 only when everything passed.
"""
import json
import os
import re
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-startup-no-popup-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
POPUP = ".kn-firstrun"
MENU = "[id=menuBtnQuickMatch]"  # the v10 menu's Quick Match card (nodes: attribute selectors)


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def assets_dir():
    """the game assets: the checkout's own (worktrees lack the untracked font and asset dirs)"""
    try:
        common = subprocess.check_output(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                         cwd=os.path.dirname(os.path.abspath(__file__)), text=True).strip()
        main = os.path.dirname(common)
        if os.path.isdir(os.path.join(main, "assets")):
            return os.path.join(main, "assets")
    except (OSError, subprocess.CalledProcessError):
        pass
    return os.path.abspath("assets")


def run_client(name, profile, script, window, mobile=False):
    """One headless client_bin run on `profile` (XDG_CONFIG_HOME, kept across runs); returns its log text."""
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL",
                                                             "KRUNKNATIVE_FIRST_RUN", "KRUNKNATIVE_MOBILE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": profile,
                "XDG_STATE_HOME": profile + "-state", "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_WINDOW_SIZE": window})
    if mobile:
        env["KRUNKNATIVE_MOBILE"] = "1"
    env.setdefault("KRUNKNATIVE_ASSETS_DIR", assets_dir())
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=90)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
    text = read(log)
    if "[input_script] " not in text:
        print(text[-2000:])
    return text


def nodes(text, t_lo, t_hi, label):
    """match counts of the `nodes @label selector` lines with that label in the script time window [lo, hi)"""
    out = []
    for m in re.finditer(r"\[input_script\] nodes t=(\d+) @" + re.escape(label) + r" elements=\d+ match=(\d+)", text):
        if t_lo <= int(m.group(1)) < t_hi:
            out.append(int(m.group(2)))
    return out


def vis(text, t_lo, t_hi, label, key):
    """the `vis @label key` value (1 shown, 0 not, - none) in the window"""
    out = []
    for m in re.finditer(r"\[input_script\] vis t=(\d+) @" + re.escape(label) + r" " + re.escape(key) + r"=(\S+)", text):
        if t_lo <= int(m.group(1)) < t_hi:
            out.append(m.group(2))
    return out


def stored(profile):
    return read(os.path.join(profile, "krunknative", "knFirstRun.json")).strip()


def seek_servers(profile):
    try:
        with open(os.path.join(profile, "krunknative", "knSettings.json")) as f:
            v = json.load(f).get("knSeekServers")
    except (OSError, ValueError, AttributeError):
        return None
    return v if isinstance(v, str) else None


def menu_script():
    """samples of the popup, the Servers window and the Connect window over the run, the menu up at 1.5 s"""
    return "\n".join([
        "0 menu",
        f"600 nodes @pop {POPUP}",
        "600 vis @win windowHolder",
        "600 vis @input connectAddr",
        f"1500 nodes @menu {MENU}",
        f"1500 nodes @pop {POPUP}",
        "1500 vis @win windowHolder",
        "1500 vis @input connectAddr",
        "1600 quit",
    ]) + "\n"


def check_menu(label, text):
    check(f"{label}: the menu is up", nodes(text, 1400, 1600, "menu") == [1], nodes(text, 1400, 1600, "menu"))
    pops = nodes(text, 0, 1600, "pop")
    check(f"{label}: no first-run popup at any sample", pops and all(n == 0 for n in pops), pops)
    wins = vis(text, 0, 1600, "win", "windowHolder")
    check(f"{label}: no Servers window opens", wins and all(v == "0" for v in wins), wins)
    inputs = vis(text, 0, 1600, "input", "connectAddr")
    check(f"{label}: no Connect window opens", inputs and all(v in ("0", "-") for v in inputs), inputs)


def main():
    if not os.path.isdir(BUILD):
        print(f"no build dir {BUILD}")
        return 2
    prof = os.path.join(WORK, "profile-desktop")
    text = run_client("desktop", prof, menu_script(), "1280x720")
    check_menu("desktop, fresh profile", text)
    check("desktop, fresh profile: no old choice is stored (knFirstRun.json absent)", stored(prof) == "", stored(prof))
    check("desktop, fresh profile: the Server Type is not krunker.io (knSeekServers not official)",
          seek_servers(prof) != "official", seek_servers(prof))

    for label, (W, H) in (("phone", (844, 390)), ("phone portrait", (390, 844))):
        pprof = os.path.join(WORK, "profile-" + label.replace(" ", "-"))
        text = run_client(label.replace(" ", "-"), pprof, menu_script(), f"{W}x{H}", mobile=True)
        check_menu(f"{label}, fresh profile (touch layout)", text)

    prof_again = prof
    text = run_client("again", prof_again, menu_script(), "1280x720")
    check_menu("desktop, the next launch on the same profile", text)
    check("desktop, the next launch: still nothing stored", stored(prof_again) == "", stored(prof_again))

    passed = sum(results)
    print(f"startup_no_popup_e2e: {passed}/{len(results)} passed (work dir {WORK})")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
