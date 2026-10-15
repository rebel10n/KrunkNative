#!/usr/bin/env python3
"""Every Svelte popup of client/src/ui/v10_misc_popups.c (and the ranked message overlay of ranked_ui.c)
opens and closes on the v10 page, headless, with no brain (the page's own requests fail the way an
offline server does).

  python3 -I client/tests/v10_misc_popups_e2e.py BUILD_DIR      (repository root)

One headless client_bin run (KRUNKNATIVE_HEADLESS=1, null audio, private XDG dirs, a closed brain port).
The input script drives the popups through their page handlers (`call`) and reads what the page shows
back (`nodes` = matching elements, `vis` = shown or not):

  survey          knMiscOpen("survey") -> the notification item; knMiscSurveyDismiss -> gone
  k-ID dialog     knMiscOpen("kid") -> the dialog layer; knMiscKidRegister (its only button) -> gone
  mission briefing knMiscOpen("hook") -> the modal, with its countdown; knMiscHookAbort -> gone
  premium shop    knMiscOpen("premium") -> no shop on this server: the error state; knMiscClose -> gone
  lottery         knMiscOpen("lottery") / openLottery(3) -> the GET /api/lottery failure, shown as v10
                  shows a failed request; knMiscClose -> gone
  usernames       knMiscOpen("usernames") -> the owned list; knViewPop(0) -> gone

The ranked message overlay (ranked_ui.c) is not here: a guest's Ranked button opens loginOrRegister (v10),
and a signed-in player needs an account; its open sequence is the uidiff frames ranked-overlay-open-*.

Exit 77 when the client binary is missing. Ports come from kn_proto.free_ports().
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not os.path.exists(os.path.join(BUILD, "client_bin")):
    print("needs client_bin: skip")
    sys.exit(77)
from kn_proto import free_ports  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-v10-popups-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


SCRIPT = """\
0 boot
1200 call knMiscOpen "survey"
1300 nodes @survey .survey-notification
1400 text @survey .survey-notification
1500 call knMiscSurveyDismiss
1600 nodes @survey_closed .survey-notification
1700 call knMiscOpen "kid"
1800 vis @kid knKidLayer
1900 nodes @kid .kid-display-dialog
2000 call knMiscKidRegister
2100 vis @kid_closed knKidLayer
2200 call knMiscClose
2300 call knMiscOpen "hook"
2400 nodes @hook .mission-briefing
2500 call knMiscHookAbort
2600 nodes @hook_closed .mission-briefing
2700 call knMiscOpen "premium"
2800 nodes @premium .error-container
2900 call knMiscClose
3000 nodes @premium_closed .premium-container
3100 call knMiscOpen "lottery"
3200 nodes @lottery .lottery-window-loading
3300 text @lottery .lottery-window-loading
3400 call knMiscClose
3500 nodes @lottery_closed .lottery-window-loading
3600 call knMiscOpen "usernames"
3700 nodes @usernames .usernames-content
3800 call knViewPop 0
3900 nodes @usernames_closed .usernames-content
4000 quit
"""


def run_client(script):
    path = os.path.join(WORK, "popups.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, "popups.log")
    brain_port = free_ports(1)[0]
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port})
    with open(log, "w") as out:
        p = subprocess.Popen([os.path.join(BUILD, "client_bin"), "--finder", "off"], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, "timeout, log %s" % log)
    with open(log, errors="replace") as f:
        return f.read(), p.returncode


def nodes(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), log)
    return int(m.group(1)) if m else -1


def vis(log, label, element):
    """1 shown, 0 in the page but hidden, 0 when no such element (vis logs '-'), -1 when the line is missing."""
    m = re.search(r"\[input_script\] vis t=\d+ @%s(.*)" % re.escape(label), log)
    if not m:
        return -1
    kv = dict(p.split("=", 1) for p in m.group(1).split() if "=" in p)
    v = kv.get(element, "-")
    return 0 if v == "-" else int(v)


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=\d+ .*? text=(.*)" % re.escape(label), log)
    return m.group(1).strip() if m else ""


def main():
    log, rc = run_client(SCRIPT)
    check("the client ran the whole script and quit cleanly", "usernames_closed" in log and rc == 0,
          "exit %s, log: %s" % (rc, os.path.join(WORK, "popups.log")))
    check("survey: the notification item shows", nodes(log, "survey") == 1, "match=%d" % nodes(log, "survey"))
    check("survey: its question text", "How is your" in text(log, "survey"), text(log, "survey")[:80])
    check("survey: dismiss removes it", nodes(log, "survey_closed") == 0, "match=%d" % nodes(log, "survey_closed"))
    check("k-ID: the dialog layer shows", vis(log, "kid", "knKidLayer") == 1, "shown=%d" % vis(log, "kid", "knKidLayer"))
    check("k-ID: its register button closes it", vis(log, "kid_closed", "knKidLayer") == 0,
          "shown=%d" % vis(log, "kid_closed", "knKidLayer"))
    check("mission briefing: the modal shows", nodes(log, "hook") == 1, "match=%d" % nodes(log, "hook"))
    check("mission briefing: ABORT closes it", nodes(log, "hook_closed") == 0, "match=%d" % nodes(log, "hook_closed"))
    check("premium: the no-shop error state shows", nodes(log, "premium") == 1, "match=%d" % nodes(log, "premium"))
    check("premium: knMiscClose closes it", nodes(log, "premium_closed") == 0, "match=%d" % nodes(log, "premium_closed"))
    check("lottery: the failed request is shown", nodes(log, "lottery") == 1, "match=%d" % nodes(log, "lottery"))
    check("lottery: knMiscClose closes it", nodes(log, "lottery_closed") == 0, "match=%d" % nodes(log, "lottery_closed"))
    check("usernames: the Usernames modal shows", nodes(log, "usernames") == 1, "match=%d" % nodes(log, "usernames"))
    check("usernames: Close (knViewPop 0) closes it", nodes(log, "usernames_closed") == 0,
          "match=%d" % nodes(log, "usernames_closed"))
    failed = [r for r in results if not r]
    print(f"{len(results) - len(failed)}/{len(results)} passed", flush=True)
    if not failed:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
