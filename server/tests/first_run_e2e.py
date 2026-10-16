#!/usr/bin/env python3
"""The first-run choice: asked once per device, each option does what it says, remembered (first-run).

  python3 -I server/tests/first_run_e2e.py BUILD_DIR      (run from the repository root)

client/src/ui/first_run_ui.c (the popup: knFirstRun(0|1|2), knFirstRunBlock), client/include/first_run_ui.h
(the storage key knFirstRun in $XDG_CONFIG_HOME/krunknative, per device), seek_prefs.c (the Server Type
knSeekServers in knSettings), servers_ui.c (the Connect window's note). One headless client_bin run at a time
against a fresh profile (--offline: no network), driven by KRUNKNATIVE_INPUT_SCRIPT:

  1. desktop, fresh profile, Krunker servers: the popup shows once (the recommended option is the highlighted
     one), the choice closes it, the Servers window (windowHolder) opens, knFirstRun = krunker, the Server Type
     = official, the popup does not come back in the session;
  2. desktop, fresh profile, Box Office: the popup closes, no window opens, knFirstRun = boxoffice, Server Type
     = kn;
  3. desktop, fresh profile, Private server: the popup closes and the Connect window opens with the IP and port
     note, its address field is there, knFirstRun = private;
  4. desktop, profile 1 again (the next launch): no popup, no window, knFirstRun still krunker;
  5. touch on a phone (844x390, KRUNKNATIVE_MOBILE=1, the touch layout), fresh profile: the popup fits the
     viewport with 16px gutters, every option is a finger target (44 CSS px or more), tapping Private server
     opens the Connect window.

KRUNKNATIVE_FIRST_RUN=1 asks in a headless run (headless runs do not ask by default). Prints PASS/FAIL lines;
exit 0 only when everything passed.
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

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-first-run-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


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
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_FIRST_RUN": "1", "XDG_CONFIG_HOME": profile,
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


def rect_of(text, t_lo, t_hi, key):
    """the `rect @label key` box (x, y, w, h, offW, offH) at script time in the window, or None"""
    for m in re.finditer(r"\[input_script\] rect t=(\d+) @\S+ " + re.escape(key) + r"=(\S+)", text):
        if t_lo <= int(m.group(1)) < t_hi and m.group(2) != "-":
            return [float(v) for v in m.group(2).split(",")[:4]]
    return None


def rects_of(text, t_lo, t_hi, label):
    """every box [x, y, w, h] of the `rects @label selector` line in the window"""
    for m in re.finditer(r"\[input_script\] rects t=(\d+) @" + re.escape(label) + r" n=\d+ (.*)", text):
        if t_lo <= int(m.group(1)) < t_hi:
            boxes = []
            for item in m.group(2).split():
                if "@" in item:
                    box = item.split("@", 1)[1]
                    try:
                        boxes.append([float(v) for v in box.split(",")])
                    except ValueError:
                        pass
            return boxes
    return []


def text_of(text, t_lo, t_hi, label):
    for m in re.finditer(r"\[input_script\] text t=(\d+) @" + re.escape(label) + r" n=\d+ .*? text=(.*)", text):
        if t_lo <= int(m.group(1)) < t_hi:
            return m.group(2)
    return ""


def settings(profile):
    try:
        with open(os.path.join(profile, "krunknative", "knSettings.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def stored(profile):
    return read(os.path.join(profile, "krunknative", "knFirstRun.json")).strip()


def seek_servers(profile):
    v = settings(profile).get("knSeekServers")
    return v if isinstance(v, str) else None


POPUP = ".kn-firstrun"
OPTIONS = ".kn-firstrun-option"


def desktop_script(choice_id, after):
    """the popup over the menu at 1280x720: samples before the click, the click, samples after it"""
    lines = ["0 menu",
             f"600 nodes @pop {POPUP}",
             "600 vis @shown knFirstRun",
             f"600 nodes @opts {OPTIONS}",
             f"600 nodes @rec {OPTIONS}--recommended",
             f"1000 clickid {choice_id}",
             f"1600 nodes @pop {POPUP}",
             "1600 vis @win windowHolder",
             "1600 vis @shown knFirstRun",
             "1600 text @hdr #windowHeader",
             "1600 nodes @note .kn-connect-note",
             "1600 vis @input connectAddr",
             f"2600 nodes @pop {POPUP}"]
    lines += after
    lines.append("2800 quit")
    return "\n".join(lines) + "\n"


def main():
    if not os.path.isdir(BUILD):
        print(f"no build dir {BUILD}")
        return 2
    # 1. Krunker servers, fresh profile
    prof_a = os.path.join(WORK, "profile-a")
    text = run_client("krunker", prof_a, desktop_script("knFirstRunOpt0", []), "1280x720")
    check("desktop: the popup shows on a fresh profile", nodes(text, 0, 1000, "pop") == [1], nodes(text, 0, 1000, "pop"))
    check("desktop: the popup is the v10 page's (vis knFirstRun shown)", vis(text, 0, 1000, "shown", "knFirstRun") == ["1"])
    check("desktop: three options", nodes(text, 0, 1000, "opts") == [3], nodes(text, 0, 1000, "opts"))
    check("desktop: the recommended option is Krunker servers (one highlighted)", nodes(text, 0, 1000, "rec") == [1])
    check("desktop: Krunker servers closes the popup", nodes(text, 1600, 2600, "pop") == [0], nodes(text, 1600, 2600, "pop"))
    check("desktop: and it stays closed (never shown again in the session)", nodes(text, 2600, 3000, "pop") == [0])
    check("desktop: Krunker servers opens the server list (the Servers window)", vis(text, 1600, 2600, "win", "windowHolder") == ["1"],
          vis(text, 1600, 2600, "win", "windowHolder"))
    check("desktop: it is the server list, not the Connect window", vis(text, 1600, 2600, "input", "connectAddr") in (["0"], ["-"]),
          vis(text, 1600, 2600, "input", "connectAddr"))
    check("desktop: the choice is stored on this device (knFirstRun = krunker)", stored(prof_a) == "krunker", stored(prof_a))
    check("desktop: the Server Type is krunker.io servers (knSeekServers = official)", seek_servers(prof_a) == "official", seek_servers(prof_a))

    # 4. the next launch on the same profile: no popup, no window
    text = run_client("remember", prof_a, "0 menu\n600 nodes @pop .kn-firstrun\n600 vis @shown knFirstRun\n"
                                          "1500 nodes @pop .kn-firstrun\n1500 vis @win windowHolder\n1600 quit\n", "1280x720")
    pops = nodes(text, 0, 2000, "pop")
    check("remembered: the next launch on the same profile shows no popup", pops and all(n == 0 for n in pops), pops)
    check("remembered: and opens no window", vis(text, 0, 2000, "win", "windowHolder") == ["0"], vis(text, 0, 2000, "win", "windowHolder"))
    check("remembered: the stored choice is unchanged", stored(prof_a) == "krunker", stored(prof_a))

    # 2. Box Office, fresh profile
    prof_b = os.path.join(WORK, "profile-b")
    text = run_client("boxoffice", prof_b, desktop_script("knFirstRunOpt1", []), "1280x720")
    check("box office: the popup closes", nodes(text, 1600, 2600, "pop") == [0], nodes(text, 1600, 2600, "pop"))
    check("box office: no window opens (the start-up seek goes on as before)", vis(text, 1600, 2600, "win", "windowHolder") == ["0"],
          vis(text, 1600, 2600, "win", "windowHolder"))
    check("box office: stored (knFirstRun = boxoffice)", stored(prof_b) == "boxoffice", stored(prof_b))
    check("box office: the Server Type stays Box Office (knSeekServers = kn)", seek_servers(prof_b) == "kn", seek_servers(prof_b))

    # 3. Private server, fresh profile
    prof_c = os.path.join(WORK, "profile-c")
    text = run_client("private", prof_c, desktop_script("knFirstRunOpt2", []), "1280x720")
    check("private: the popup closes", nodes(text, 1600, 2600, "pop") == [0], nodes(text, 1600, 2600, "pop"))
    check("private: the Connect window opens", vis(text, 1600, 2600, "win", "windowHolder") == ["1"],
          vis(text, 1600, 2600, "win", "windowHolder"))
    check("private: the window has the IP and port note", nodes(text, 1600, 2600, "note") == [1],
          nodes(text, 1600, 2600, "note"))
    check("private: the address field is there", vis(text, 1600, 2600, "input", "connectAddr") == ["1"],
          vis(text, 1600, 2600, "input", "connectAddr"))
    check("private: stored (knFirstRun = private)", stored(prof_c) == "private", stored(prof_c))

    # 5. touch on a phone, fresh profile: landscape (844x390, the phone's game layout) and portrait (390x844)
    for label, (W, H) in (("phone", (844, 390)), ("phone portrait", (390, 844))):
        prof = os.path.join(WORK, "profile-" + label.replace(" ", "-"))
        phone = [
            "0 menu",
            "600 nodes @pop " + POPUP,
            "600 rects @pop " + POPUP,
            "600 rects @opt " + OPTIONS,
            "600 rect @opt knFirstRunOpt0",
            "600 rect @opt knFirstRunOpt1",
            "600 rect @opt knFirstRunOpt2",
            "1000 clickid knFirstRunOpt2",
            "1600 vis @win windowHolder",
            "1600 nodes @note .kn-connect-note",
            "2000 nodes @pop " + POPUP,
            "2200 quit",
        ]
        text = run_client(label.replace(" ", "-"), prof, "\n".join(phone) + "\n", f"{W}x{H}", mobile=True)
        check(f"{label}: the popup shows on a fresh profile (touch layout)", nodes(text, 0, 1000, "pop") == [1], nodes(text, 0, 1000, "pop"))
        pop = rects_of(text, 0, 1000, "pop")
        check(f"{label}: the popup is inside the viewport with 16px gutters", len(pop) == 1 and pop[0][0] >= 16 and pop[0][0] + pop[0][2] <= W - 16 and
              pop[0][1] >= 0 and pop[0][1] + pop[0][3] <= H, pop)
        opts = [rect_of(text, 0, 1000, f"knFirstRunOpt{i}") for i in range(3)]
        check(f"{label}: three options, listed", all(o is not None for o in opts) and len(rects_of(text, 0, 1000, "opt")) == 3, opts)
        if all(opts):
            check(f"{label}: every option is a finger target (44 px or taller, inside the gutters)",
                  all(o[3] >= 44 and o[0] >= 16 and o[0] + o[2] <= W - 16 for o in opts), opts)
        check(f"{label}: tapping Private server opens the Connect window", vis(text, 1000, 2000, "win", "windowHolder") == ["1"],
              vis(text, 1000, 2000, "win", "windowHolder"))
        check(f"{label}: the Connect window has the IP and port note", nodes(text, 1000, 2000, "note") == [1], nodes(text, 1000, 2000, "note"))
        check(f"{label}: the popup is gone after the tap", nodes(text, 2000, 2200, "pop") == [0], nodes(text, 2000, 2200, "pop"))
        check(f"{label}: stored (knFirstRun = private)", stored(prof) == "private", stored(prof))

    passed = sum(results)
    print(f"first_run_e2e: {passed}/{len(results)} passed (work dir {WORK})")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    rc = main()
    if rc == 0:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(rc)
