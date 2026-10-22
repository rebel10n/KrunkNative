#!/usr/bin/env python3
"""Krunker Strike's Battle Pass tabs for a signed-out player (client/src/ui/strike_meta.c bp_tab): the Rewards and the
Missions tab say so ("You must be logged in.") and never show "Loading Rewards" / "Loading Missions" for good.

  python3 -I client/tests/strike_signedout_e2e.py BUILD_DIR      (run from the repository root)

A headless Strike guest (--flavor strike --offline: no account, no Strike account). The Battle Pass opens on its
Missions tab (strikeAction "missions"), then its Rewards tab (strikeAction "bp-tab:0"); the tab's text is read after
each. Fails before the fix (both tabs read "Loading Missions" / "Loading Rewards"), passes after it.
Needs a headless GL device (KRUNKNATIVE_EGL_DEVICE picks it). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-signedout-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
SIGNED_OUT = "You must be logged in."


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run_client(script):
    base = os.path.join(WORK, "guest")
    cfg = os.path.join(base, "cfg")
    # a returning player: the first run's controls tutorial would replace the menu (strike_ftue.c), as in the other
    # Strike e2e tests
    os.makedirs(os.path.join(cfg, "krunknative-strike"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative-strike", "knFtue.json"), "w") as f:
        f.write('{"guest":{"played":true}}')
    os.makedirs(os.path.join(base, "state"), exist_ok=True)
    script_path = os.path.join(base, "input.script")
    with open(script_path, "w") as f:
        f.write(script)
    drop = ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_VSYNC", "KRUNKNATIVE_FRAME_CAP", "KRUNKNATIVE_BRAIN_URL", "KN_FLAVOR")
    env = {k: v for k, v in os.environ.items() if k not in drop}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(base, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_ASSETS_DIR": os.path.abspath("assets") + os.sep})
    log = os.path.join(base, "client.log")
    with open(log, "w") as out:
        try:
            code = subprocess.run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline", "--map", "kanji"],
                                  stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                  timeout=240).returncode
        except subprocess.TimeoutExpired:
            code = None
    with open(log, errors="replace") as f:
        return code, f.read()


def tab_text(text, label):
    found = re.findall(rf"\[input_script\] text t=\d+ @{label} n=\d+ .*? text=(.*)", text)
    return found[-1] if found else None


def main():
    # the guest's Battle Pass on its Missions tab, then on its Rewards tab; each tab's text is read after the change
    script = """0 boot
0 waitfor [id=mobMenuPlay]
1000 call strikeAction "missions"
3000 text @missions .tabsContent
3100 call strikeAction "bp-tab:0"
3200 text @rewards .tabsContent
3300 quit
"""
    code, text = run_client(script)
    check("the client ran to its quit", code == 0, f"exit {code}")
    missions = tab_text(text, "missions")
    rewards = tab_text(text, "rewards")
    check("Missions: the signed-out line, not Loading Missions",
          missions is not None and SIGNED_OUT in missions and "Loading" not in missions, f"text={(missions or 'none read')[:120]!r}")
    check("Rewards: the signed-out line, not Loading Rewards",
          rewards is not None and SIGNED_OUT in rewards and "Loading" not in rewards, f"text={(rewards or 'none read')[:120]!r}")
    print(f"log: {WORK}/guest/client.log")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    rc = main()
    if rc == 0:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(rc)
