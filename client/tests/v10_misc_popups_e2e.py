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
  usernames       knMiscOpen("usernames") -> the popup; its GET /account/usernames fails (no brain), which v10's
                  request helper throws, so no list and no error line show (deob-10.0.7.js:115497-115500,
                  :26152-26160); knViewPop(0) -> gone

The usernames list itself renders from --render-scenario answers (tools/uidiff/scenarios/popup-usernames.json with
state.community.popups.usernames {names, cooldownSecs, held, holdSecs, swapConfirm, renewConfirm, buy, confirmBuy}),
read back from the captured document (KRUNKNATIVE_SCENARIO_HTML): the expiry line ("Expires Mar 5, 2027 <span
class=meta-left>3d 4h left</span>" + "Renewable from"), the grace row, Renew capped (disabled, renew.cap title),
the cooldown line ("4m 07s") with Use / Switch disabled, the switch confirmation's original-name note, the renew
confirmation, the buy confirmation's original-name note, the held-name card and the escaped input value.

The ranked message overlay (ranked_ui.c) is not here: a guest's Ranked button opens loginOrRegister (v10),
and a signed-in player needs an account; its open sequence is the uidiff frames ranked-overlay-open-*.

Exit 77 when the client binary is missing. Ports come from kn_proto.free_ports().
"""
import json
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
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail else ''}", flush=True)


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
3750 nodes @usernames_list .usernames-columns
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


DAY = 86400
DATE = r"[A-Z][a-z]{2} \d{1,2}, \d{4}"  # toLocaleDateString(undefined, {day, month: short, year}) as en-US
C = r"(?:<!---->)?"  # a Svelte anchor comment, when the serializer keeps it
NAMES = [
    {"name": "Sidney", "state": "active", "original": True, "leave_release_secs": 365 * DAY},
    {"name": "Kestrel", "state": "owned", "expires_secs": 400 * DAY, "release_secs": 3 * DAY + 4 * 3600 + 1800,
     "renew_secs": 40 * DAY},
    {"name": "Wren", "state": "grace", "release_secs": 2 * 3600 + 5 * 60 + 50},
]


def render_usernames(name, usernames):
    with open(os.path.join(ROOT, "tools/uidiff/scenarios/popup-usernames.json")) as fp:
        scen = json.load(fp)
    scen["state"]["community"]["popups"]["usernames"] = usernames
    path = os.path.join(WORK, name + ".json")
    with open(path, "w") as fp:
        json.dump(scen, fp)
    html = os.path.join(WORK, name + ".html")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "KRUNKNATIVE_SCENARIO_HTML": html, "TZ": "UTC"})
    r = subprocess.run([os.path.join(BUILD, "client_bin"), "--render-scenario", path, os.path.join(WORK, name + ".png")], cwd=ROOT,
                       env=env, capture_output=True, text=True, timeout=240)
    check("usernames %s: client_bin exits 0" % name, r.returncode == 0, (r.stdout + r.stderr)[-600:])
    try:
        with open(html, encoding="utf-8") as fp:
            return fp.read()
    except OSError:
        check("usernames %s: the document was written" % name, False)
        return ""


def usernames_scenarios():
    doc = render_usernames("swap", {"names": NAMES, "cooldownSecs": 4 * 60 + 7, "swapConfirm": "Kestrel",
                                    "buy": 'a"b<c'})
    check("usernames: the list renders", doc.count('class="username-row svelte-27udiy') == 3,
          "rows=%d" % doc.count('class="username-row svelte-27udiy'))
    check("usernames: the expiry line (Expires {date}, meta-left {t} left, Renewable from {date})",
          re.search(r'<span class="username-meta svelte-27udiy">Expires %s <span class="meta-left svelte-27udiy">3d 4h left</span> '
                    r'<span class="meta-left svelte-27udiy">Renewable from %s</span></span>' % (DATE, DATE), doc),
          re.findall(r'<span class="username-meta svelte-27udiy">Expires.{0,200}', doc)[:1])
    check("usernames: the grace row's release countdown (Xh Ym)",
          re.search(r'<span class="username-grace svelte-27udiy">Pending release</span> Released in 2h 5m unless renewed</span>', doc),
          re.findall(r'grace-meta.{0,200}', doc)[:1])
    check("usernames: a grace row's Renew is io-button--accept",
          re.search(r'class="io-button io-button--slim svelte-27udiy io-button--accept"[^>]*>Renew<', doc))
    check("usernames: Renew while renewable later is disabled with the renew.cap title",
          re.search(r'<button type="button" class="io-button io-button--slim svelte-27udiy" disabled(?:="")? '
                    r'title="A username can be paid for up to 2 years ahead"', doc))
    check("usernames: the cooldown line",
          re.search(r'<div class="usernames-cooldown svelte-27udiy"><i class="material-icons svelte-27udiy">schedule</i> '
                    r'You can switch again in 4m 07s</div>', doc),
          re.findall(r'usernames-cooldown.{0,160}', doc)[:1])
    check("usernames: Use is disabled while the cooldown runs",
          re.search(r'io-button--accept-secondary svelte-27udiy" disabled(?:="")?[^>]*>Use<', doc))
    check("usernames: the switch confirmation with the original name's leave note",
          re.search(r'<div class="inline-confirm svelte-27udiy"><span>Switch to Kestrel\? You will sign in as Kestrel from now on\.</span> '
                    r'<span class="leave-note svelte-27udiy">%s Sidney is your original username\. It\'s kept for 365 days after you switch '
                    r'away, plus any renewals \(until %s\)' % (C, DATE), doc),
          re.findall(r'Switch to Kestrel.{0,300}', doc)[:1])
    check("usernames: Switch is io-button--accept-secondary, disabled during the cooldown",
          re.search(r'class="io-button io-button--slim io-button--accept-secondary" disabled(?:="")?[^>]*>Switch<', doc))
    check("usernames: the buy input's value is escaped", 'value="a&quot;b&lt;c"' in doc or "value=\"a&quot;b<c\"" in doc,
          re.findall(r'id="knUnBuy".{0,80}', doc)[:1])
    check("usernames: Buy is enabled for a non-empty name",
          re.search(r'<button type="button" class="io-button io-button--accept buy-button svelte-27udiy"(?! disabled)[^>]*>Buy \$9\.99<', doc))

    doc = render_usernames("renew", {"names": NAMES, "renewConfirm": "Kestrel", "buy": "Neo", "confirmBuy": True,
                                     "held": "Trinity", "holdSecs": 14 * 60 + 5})
    check("usernames: the renew confirmation",
          re.search(r'<div class="inline-confirm svelte-27udiy"><span>Renew Kestrel for \$4\.99\? It will be valid until %s\.</span> '
                    r'<div class="confirm-buttons svelte-27udiy"><button type="button" class="io-button io-button--slim"[^>]*>Cancel</button> '
                    r'<button type="button" class="io-button io-button--slim io-button--accept"[^>]*>Renew \$4\.99</button>' % DATE, doc),
          re.findall(r'Renew Kestrel.{0,300}', doc)[:1])
    check("usernames: no cooldown line when none runs", "usernames-cooldown" not in doc)
    check("usernames: the buy confirmation with the original name's note",
          re.search(r'<span>Buy Neo for \$9\.99\? It\'s valid for 365 days; renew it for \$4\.99 to keep it longer\.</span> '
                    r'<span class="leave-note svelte-27udiy">Your original username Sidney is kept for 365 days', doc),
          re.findall(r'Buy Neo.{0,300}', doc)[:1])
    check("usernames: the held name's card",
          re.search(r'<div class="pending-card svelte-27udiy"><span>Trinity is held for you for 14m 05s</span> '
                    r'<button type="button" class="io-button io-button--slim io-button--accept"[^>]*>Buy \$9\.99</button></div>', doc),
          re.findall(r'pending-card.{0,200}', doc)[:1])

    doc = render_usernames("none", {})
    check("usernames: no list before the answer", "usernames-columns" not in doc and "usernames-content" in doc)


def main():
    usernames_scenarios()
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
    check("usernames: the failed list request shows no list", nodes(log, "usernames_list") == 0,
          "match=%d" % nodes(log, "usernames_list"))
    check("usernames: Close (knViewPop 0) closes it", nodes(log, "usernames_closed") == 0,
          "match=%d" % nodes(log, "usernames_closed"))
    failed = [r for r in results if not r]
    print(f"{len(results) - len(failed)}/{len(results)} passed", flush=True)
    if not failed:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
