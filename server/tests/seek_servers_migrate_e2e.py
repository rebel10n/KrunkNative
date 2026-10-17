#!/usr/bin/env python3
"""The Server Type reset is once per profile (client/src/ui/seek_prefs.c seek_prefs_migrate_servers,
knSeekServersMigrated): a saved "official" (or "both") from the old first-run popup becomes Box Office
("kn") on the first launch, and a later krunker.io pick is never touched again.

  python3 -I server/tests/seek_servers_migrate_e2e.py BUILD_DIR      (run from the repository root)

One headless client_bin run at a time (--offline: no network), each on a profile whose knSettings.json is
written before the launch (XDG_CONFIG_HOME/krunknative), driven by KRUNKNATIVE_INPUT_SCRIPT:

  M1 a profile with knSeekServers "official" and no flag: the first launch stores "kn" and the flag 1, and
     logs the reset once; the next launch keeps "kn" and logs nothing;
  M2 the same for "both";
  M3 a profile where the player picks "official" after the flag is set (setSetting): the next launch keeps it;
  M4 a profile with no Server Type saved: the launch stores the flag only (knSeekServers stays unset), and
     the next launch with "kn" saved keeps "kn".

Prints PASS/FAIL lines; exit 0 only when everything passed.
"""
import json
import os
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-servers-migrate-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
MIGRATED_LOG = "Quickplay knSeekServers="  # the line seek_prefs_migrate_servers prints when it resets a choice


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
    """the game assets: the checkout's own (worktrees lack the untracked asset dirs)"""
    try:
        common = subprocess.check_output(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                         cwd=os.path.dirname(os.path.abspath(__file__)), text=True).strip()
        main = os.path.dirname(common)
        if os.path.isdir(os.path.join(main, "assets")):
            return os.path.join(main, "assets")
    except (OSError, subprocess.CalledProcessError):
        pass
    return os.path.abspath("assets")


def new_profile(name, saved):
    """a profile directory; `saved` is the knSettings.json object to start from (None: no file)"""
    prof = os.path.join(WORK, name)
    os.makedirs(os.path.join(prof, "krunknative"), exist_ok=True)
    if saved is not None:
        with open(os.path.join(prof, "krunknative", "knSettings.json"), "w") as f:
            json.dump(saved, f)
    return prof


def run_client(name, profile, script):
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
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
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


def settings(profile):
    """the stored settings object (knSettings.json), {} when there is none"""
    try:
        with open(os.path.join(profile, "krunknative", "knSettings.json")) as f:
            v = json.load(f)
    except (OSError, ValueError):
        return {}
    return v if isinstance(v, dict) else {}


def quit_script(*lines):
    return "0 menu\n" + "".join(line + "\n" for line in lines) + "1500 quit\n"


def main():
    if not os.path.isdir(BUILD):
        print(f"no build dir {BUILD}")
        return 2

    # M1: "official" from the old popup, no flag: reset once, then kept
    p = new_profile("m1", {"knSeekServers": "official"})
    text = run_client("m1-first", p, quit_script())
    s = settings(p)
    check("M1 first launch: knSeekServers official -> kn", s.get("knSeekServers") == "kn", s.get("knSeekServers"))
    check("M1 first launch: knSeekServersMigrated is 1", s.get("knSeekServersMigrated") == 1, s.get("knSeekServersMigrated"))
    check("M1 first launch: the reset is logged", MIGRATED_LOG in text and "official -> kn" in text)
    text = run_client("m1-next", p, quit_script())
    s = settings(p)
    check("M1 next launch: still kn", s.get("knSeekServers") == "kn", s.get("knSeekServers"))
    check("M1 next launch: no second reset", MIGRATED_LOG not in text)

    # M2: "both" is reset the same way
    p = new_profile("m2", {"knSeekServers": "both"})
    run_client("m2-first", p, quit_script())
    s = settings(p)
    check("M2 both -> kn with the flag set", s.get("knSeekServers") == "kn" and s.get("knSeekServersMigrated") == 1, s)

    # M3: the player picks krunker.io after the flag: the next launch keeps it
    p = new_profile("m3", {"knSeekServers": "official"})
    run_client("m3-reset", p, quit_script())
    text = run_client("m3-pick", p, quit_script('300 call setSetting "knSeekServers" "official"'))
    check("M3 the pick is stored: official", settings(p).get("knSeekServers") == "official", settings(p).get("knSeekServers"))
    text = run_client("m3-next", p, quit_script())
    s = settings(p)
    check("M3 next launch: official is kept", s.get("knSeekServers") == "official", s.get("knSeekServers"))
    check("M3 next launch: no reset", MIGRATED_LOG not in text)

    # M4: nothing saved: the flag only (knSeekServers stays unset); a saved "kn" is kept
    p = new_profile("m4", None)
    run_client("m4-first", p, quit_script())
    s = settings(p)
    check("M4 fresh profile: the flag is set", s.get("knSeekServersMigrated") == 1, s.get("knSeekServersMigrated"))
    check("M4 fresh profile: knSeekServers is not written", "knSeekServers" not in s, s.get("knSeekServers"))
    run_client("m4-pick", p, quit_script('300 call setSetting "knSeekServers" "kn"'))
    run_client("m4-next", p, quit_script())
    check("M4 kn picked and kept on the next launch", settings(p).get("knSeekServers") == "kn", settings(p).get("knSeekServers"))

    passed = sum(results)
    print(f"seek_servers_migrate_e2e: {passed}/{len(results)} passed (work dir {WORK})")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
