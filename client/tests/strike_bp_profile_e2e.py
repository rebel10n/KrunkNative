#!/usr/bin/env python3
"""Krunker Strike's Battle Pass Rewards tab, PROFILE-DETAILS and the message popups (client/src/ui/strike_meta.c,
strike_profile.c; ui/strike/screens/REWARDS_TAB_SCREEN, PROFILE-DETAILS, POPUP_MESSAGE). FRVR has no sandbox capture
of these views, so this checks their structure and data, not their pixels.

  python3 -I client/tests/strike_bp_profile_e2e.py BUILD_DIR      (run from the repository root)

  1. --render-scenario of tools/uidiff/scenarios/strike/{battlepass-rewards, profile-details,
     profile-details-settings, profile-details-logout}.json: a 1280x720 PNG each and the screen's own log line:
     the Rewards tab of the signed-in fixture (50 levels, level 4, 3 claimable: levels 2-4, level 1 claimed),
     PROFILE-DETAILS, its Account Settings tab, and POPUP_MESSAGE over it.
  2. a live offline (guest) client, an input script: PROFILE-DETAILS opens (Account Overview: no Logout and no
     name form for a guest), Account Settings shows nothing for a guest, the Logout confirm (POPUP_MESSAGE: Cancel
     and Logout), Logout (guest: nothing to sign out of) returns to the menu, and Submit with an empty name marks
     the field and opens no popup.

Signed-in actions (the Rewards tab's claims, the name change, Logout of an account) need a kn_brain account and are
not run here. Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). KRUNKNATIVE_ASSETS_DIR is passed through (a
worktree without the untracked asset dirs). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-bp-profile-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
SCEN = os.path.join(ROOT, "tools", "uidiff", "scenarios", "strike")
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


def run(args, cfg, log, extra=None, timeout=240):
    os.makedirs(cfg, exist_ok=True)
    try:
        with open(log, "w") as f:
            rc = subprocess.run(args, cwd=ROOT, env=env(cfg, extra), stdout=f, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, timeout=timeout).returncode
    except subprocess.TimeoutExpired:
        rc = -1
    with open(log, errors="replace") as f:
        return rc, f.read()


def png_size(path):
    try:
        with open(path, "rb") as f:
            head = f.read(24)
        return struct.unpack(">II", head[16:24]) if head[:8] == b"\x89PNG\r\n\x1a\n" else None
    except OSError:
        return None


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d value=.*? text=(.*)" % re.escape(label), text)
    return (int(m.group(1)), m.group(2).strip()) if m else (-1, "")


def scenario_case(name, expect_log, expect_screen):
    out = os.path.join(WORK, name + ".png")
    rc, text = run([f"{BUILD}/client_bin", "--flavor", "strike", "--render-scenario", os.path.join(SCEN, name + ".json"), out],
                   os.path.join(WORK, "cfg-" + name), os.path.join(WORK, name + ".log"))
    check(f"1: {name}: renders (rc 0, 1280x720 PNG)", rc == 0 and png_size(out) == (1280, 720), f"rc {rc}")
    check(f"1: {name}: {expect_screen} is shown", f"[strike] screen {expect_screen}" in text, "")
    if expect_log:
        check(f"1: {name}: {expect_log[1]}", expect_log[0] in text, "")
    return text


LIVE_SCRIPT = """menu
1500 nodes @page #frvrMenuMobile
1600 call strikeAction "profile"
2000 nodes @profile .frvr-settings-screen
2100 call strikeAction "profile-details"
2600 nodes @pd .frvr-account-overview
2650 nodes @pdlogout .frvr-account-overview .frvr-button--gray
2700 nodes @pdname #newName
2750 call strikeAction "profile-tab:1"
2900 nodes @pdsettings #newName
2950 call strikeAction "name-submit"
3000 nodes @nopop .mobPopAlert
3100 call strikeAction "profile-tab:0"
3300 call strikeAction "logout"
3600 nodes @msg .mobPopAlert .alert-text
3650 text @msgtext .mobPopAlert .alert-text span
3700 nodes @msgbtn .mobPopAlert .frvr-button
3750 nodes @msgsec .mobPopAlert .frvr-button--gray
3800 call strikeAction "msg-primary"
4100 nodes @msgafter .mobPopAlert
4150 nodes @pdafter .frvr-account-overview
4200 call strikeAction "profile-details"
4700 call strikeAction "logout"
4900 call strikeAction "msg-secondary"
5200 nodes @pdgone .frvr-account-overview
5250 nodes @homeafter .mobMenuAcc--main-menu
5300 quit
"""


def live_case():
    script = os.path.join(WORK, "live.txt")
    with open(script, "w") as f:
        f.write(LIVE_SCRIPT)
    rc, text = run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], os.path.join(WORK, "cfg-live"),
                   os.path.join(WORK, "live.log"), {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    check("2: live client ran its script", rc == 0 and "[strike] Krunker Strike page ready" in text, f"rc {rc}")
    check("2: PROFILE-DETAILS opens with Account Overview (its frame)", nodes(text, "pd") == 1, str(nodes(text, "pd")))
    check("2: a guest has no Logout button on the overview", nodes(text, "pdlogout") == 0, str(nodes(text, "pdlogout")))
    check("2: the overview has no name form", nodes(text, "pdname") == 0, str(nodes(text, "pdname")))
    check("2: Account Settings for a guest: no name form", nodes(text, "pdsettings") == 0, str(nodes(text, "pdsettings")))
    check("2: Submit without a name form (a guest) opens no popup", nodes(text, "nopop") == 0, str(nodes(text, "nopop")))
    check("2: Logout opens the confirm (POPUP_MESSAGE)", nodes(text, "msg") == 1, str(nodes(text, "msg")))
    check("2: the confirm asks \"Logout from this account?\"", text_of(text, "msgtext") == (1, "Logout from this account?"),
          str(text_of(text, "msgtext")))
    check("2: the confirm has Cancel and Logout (two buttons)", nodes(text, "msgbtn") == 2 and nodes(text, "msgsec") == 1,
          f"{nodes(text, 'msgbtn')} {nodes(text, 'msgsec')}")
    check("2: Cancel closes the confirm", nodes(text, "msgafter") == 0 and nodes(text, "pdafter") == 1,
          f"{nodes(text, 'msgafter')} {nodes(text, 'pdafter')}")
    check("2: Logout (a guest: nothing to sign out of) returns to the menu", nodes(text, "pdgone") == 0 and nodes(text, "homeafter") == 1,
          f"{nodes(text, 'pdgone')} {nodes(text, 'homeafter')}")
    check("2: the live client quit cleanly", rc == 0, f"rc {rc}")


def main():
    if not os.path.isdir(os.path.join(ROOT, "ui", "strike")):
        print("run from the repository root")
        return 2
    scenario_case("battlepass-rewards", ("50 levels, level 4, 3 claimable", "the Rewards tab: 50 levels at level 4, 3 claimable"),
                  "BATTLEPASS_SCREEN")
    scenario_case("profile-details", None, "PROFILE-DETAILS")
    scenario_case("profile-details-settings", None, "PROFILE-DETAILS")
    scenario_case("profile-details-logout", None, "POPUP_MESSAGE")
    live_case()
    passed = sum(results)
    print(f"\n{passed}/{len(results)} checks passed (work dir {WORK})")
    return 0 if results and passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
