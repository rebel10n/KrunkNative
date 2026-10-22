#!/usr/bin/env python3
"""The Daily Spin and the Battle Pass on an official krunker.io game (client/src/ui/v10_daily_spin.c,
battlepass_ui.c), end to end against account_unify_e2e's stubs. Nothing leaves the machine.

  python3 -I server/tests/kio_spin_battlepass_e2e.py BUILD_DIR      (run from the repository root)

A real kn_brain (scratch database) and the krunker.io stubs of krunker_account_e2e.py. The client signs in
to krunker.io and joins an official game as that account (account_unify_e2e's steps 1-3), then, in the menu:
  spin    the pass is krunker.io's (the game socket's "bpdl"); the Daily Spin's progress comes from
          api.krunker.io's GET /daily-spin/progress with the account's token, the score at its maximum: the
          item says FREE SPIN; a click says the claim is not sent from krunker.io servers, and sends no "dsl"
          (before: nothing was asked, the item stayed 0/5000 and a click did nothing)
  nopass  the game server's "bpdl" has no pass (null): the window says No Battle Pass Found (before: Loading
          Battle Pass for ever); the progress is refused (404): the item stays 0/5000 (before: Unavailable)
  silent  the game server never answers "bpdl": the window keeps saying Loading Battle Pass (v10's text) and asks
          again after its 15 s; it never says it could not load (before: "Could not load the Battle Pass")
  locked  krunker.io's progress is 0/5000 (not unlocked): a click does what v10.0.3 does, nothing. No popup,
          no "Play more" text, no "dsl", and the item is still 0/5000 (before: the popup said Play more to
          unlock the daily spin)
A load failure shows no text either: nopass's refused progress leaves the item at its default 0/5000 (v10's
getProgress catch {}), never Unavailable.
Exit 0 when every check passed, 77 when it cannot run.
"""
import json
import os
import re
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import account_unify_e2e as au  # noqa: E402  (its stubs, sign-in steps and client runner)

MODES = os.environ.get("KIO_MODES", "spin,nopass,silent,locked").split(",")  # KIO_MODES=locked runs one mode
AU_SCRIPT = au.script  # account_unify's steps 1-3 (run_client takes its script from au.script, replaced per mode)
SPIN_DATA = {"score": 5000, "maxScore": 5000, "timeRemaining": 3600000, "claimed": False, "stage": 0}  # FREE SPIN
ZERO_DATA = {"score": 0, "maxScore": 5000, "timeRemaining": 3600000, "claimed": False, "stage": 0}     # 0/5000
PLAY_MORE = "Play more"  # the Box Office brain's refusal text; v10 has no such text, so the official item never shows it
JOIN_END = 38600  # account_unify's script from here on opens its own windows: this test writes its own


async def answer_null_pass(send, msg):  # "bpdl" with no pass (the season has none)
    await send("bpdl", None)


async def never_answers(send, msg):  # the game server does not answer "bpdl"
    return


def script(mode, shots):
    """account_unify_e2e's sign-in and official join (steps 1-3), then this mode's menu steps"""
    lines = [ln for ln in AU_SCRIPT().splitlines() if ln.strip() and int(ln.split()[0]) < JOIN_END]
    lines += ["38900 down ESC", "38950 up ESC",  # the join leaves the menu closed: ESC opens it (the screenshots show it)
              "39000 call showWindow 6",  # the Battle Pass (v10's windows[5] = showWindow 6)
              "39700 text @%s [id=menuWindow]" % ("kbpfirst" if mode == "silent" else "kbp"),
              "39720 shot " + shots["bp"],
              "40000 text @dsitem [id=dailySpinDiv]",
              "40050 shot " + shots["item"]]
    if mode == "silent":  # the window's timeout is 15 s of wall time: read it again after 16 s
        lines += ["55500 text @kbp [id=menuWindow]", "55550 shot " + shots["bp"], "55600 quit"]
        return "\n".join(lines) + "\n"
    if mode == "locked":  # v10's click on a locked item does nothing: read the popup and the item around it
        lines += ["40300 call closWind",
                  "40350 vis @prepop popupHolder",
                  "40400 clickid dailySpinDiv",
                  "40900 vis @postpop popupHolder",
                  "40910 text @popup [id=popupContent]",
                  "40950 text @dsafter [id=dailySpinDiv]",
                  "41000 shot " + shots["click"],
                  "41200 quit"]
        return "\n".join(lines) + "\n"
    lines += ["40300 call closWind",
              "40400 clickid dailySpinDiv",  # the item's click: claim it (krunker.io: a message, no "dsl")
              "41000 shot " + shots["click"],
              "41200 quit"]
    return "\n".join(lines) + "\n"


def run_mode(port, mode):
    http_port, ws_port = au.kn_proto.free_ports(2)
    stub = au.kae.Stub(http_port, ws_port)
    stub.players = au.OTHERS
    stub.account_data = [dict(au.KIO_DATA)]
    stub.daily_spin = {"spin": SPIN_DATA, "nopass": None, "silent": ZERO_DATA, "locked": ZERO_DATA}[mode]
    stub.frame_handlers = {"bpdl": {"spin": au.answer_bpdl, "nopass": answer_null_pass, "silent": never_answers,
                                    "locked": au.answer_bpdl}[mode]}
    au.WORK = tempfile.mkdtemp(prefix="kn-kiospin-%s-" % mode, dir=au.CACHE if os.path.isdir(au.CACHE) else None)
    shots = {k: os.path.join(au.WORK, "%s-%s.png" % (k, mode)) for k in ("bp", "item", "click")}
    au.script = lambda: script(mode, shots)
    stub.start()
    try:
        text = au.run_client(port, stub)
    finally:
        stub.stop()
    checks(mode, text, stub, shots)


def checks(mode, text, stub, shots):
    bp = au.text_of(text, "kbp")  # (silent: the late read; its first read is kbpfirst, Loading)
    item = au.text_of(text, "dsitem")
    gets = [r for r in stub.requests if r[0] == "GET" and r[1] == "/api/daily-spin/progress"]
    print("  [%s] Battle Pass: %r | item: %r" % (mode, bp[:80], item[:80]))
    if mode == "spin":
        au.check("spin: the Daily Spin's progress is asked of api.krunker.io with the account's token",
                 bool(gets) and gets[0][3].get("authorization", "").startswith("Bearer "), [r[:2] for r in stub.requests][-12:])
        au.check("spin: the item says FREE SPIN (the score is at its maximum)", "FREE SPIN" in item, item)
        au.check("spin: the Battle Pass shows krunker.io's pass, not Loading", "KioPass" in bp and "Loading" not in bp, bp[:200])
        au.check("spin: a click sends no claim (\"dsl\") to the game socket", "dsl" not in stub.ws_frames, stub.ws_frames[-20:])
    elif mode == "nopass":
        au.check("nopass: the Battle Pass says No Battle Pass Found (a pass-less answer finishes)",
                 "No Battle Pass Found" in bp and "Loading" not in bp, bp[:200])
        au.check("nopass: the item stays 0/5000 when krunker.io refuses the progress (404): no load failure text",
                 "0/5000" in item and "Unavailable" not in item, item)
    elif mode == "locked":
        pop = au.vis(text, "prepop"), au.vis(text, "postpop")
        popup = au.text_of(text, "popup")
        after = au.text_of(text, "dsafter")
        claim_sent = "dsl" in stub.ws_frames
        posts = [r for r in stub.requests if r[0] != "GET" and "daily-spin" in r[1]]
        print("  [locked] popup: %r | item after: %r" % (popup[:80], after[:80]))
        au.check("locked: a click on the locked item shows no popup (v10 says nothing)",
                 pop[0].get("popupHolder") == "0" and pop[1].get("popupHolder") == "0", pop)
        au.check("locked: no \"Play more\" text anywhere (v10 has none; Box Office's brain rule is its own)",
                 PLAY_MORE not in popup and PLAY_MORE not in after, popup[:200])
        au.check("locked: a click changes no state: no claim (\"dsl\") and no daily-spin POST",
                 not claim_sent and not posts, (stub.ws_frames[-20:], posts))
        au.check("locked: the item is still 0/5000, not claimed", "0/5000" in after and "CLAIMED" not in after
                 and "FREE SPIN" not in after, after)
    else:
        au.check("silent: the Battle Pass keeps saying Loading Battle Pass after its timeout, never that it could not load",
                 "Loading Battle Pass" in bp and "Could not load" not in bp, bp[:200])
    shot_ok = [os.path.exists(p) for k, p in shots.items() if (mode != "silent" or k != "click")]
    au.check("%s: the window screenshots were taken" % mode, all(shot_ok), shots)


def main():
    if au.kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{au.BUILD}/client_bin", os.X_OK) or not os.access(f"{au.BUILD}/services/brain/kn_brain", os.X_OK):
        print("kio_spin_battlepass_e2e: no client_bin / kn_brain in %s: skipped" % au.BUILD)
        return 77
    name = "kn_test_kiospin_%d" % os.getpid()
    scratch = [sys.executable, "-I", os.path.join(au.ROOT, "services", "brain", "tests", "db_scratch.py"), au.ROOT]
    r = subprocess.run(scratch + ["create", au.DRIVER, name], capture_output=True)
    if r.returncode:
        print("kio_spin_battlepass_e2e: %s unavailable: skipped" % au.DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=au.SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain_log = os.path.join(au.WORK, "brain.log")
    try:
        subprocess.run([f"{au.BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=au.ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{au.BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local", "--brain.id=kiospin-%d" % os.getpid(),
                                  "--accounts.email.required=false", "--smtp.host=", "--accounts.developers=" + au.DEV],
                                 cwd=au.ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        au.procs.append(brain)
        m = au.wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", au.read_log(brain_log)), 15)
        au.check("kn_brain starts", bool(m), au.read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        st, reg = au.http(port, "/auth/register", {"username": au.DEV, "password": au.PW})
        au.check("the DEV account is registered", st == 200, "%s %s" % (st, str(reg)[:160]))
        for mode in MODES:
            run_mode(port, mode)
    finally:
        for p in au.procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        subprocess.run(scratch + ["drop", au.DRIVER, name], capture_output=True)
    passed = sum(au.results)
    print(f"{passed}/{len(au.results)} passed")
    return 0 if au.results and passed == len(au.results) else 1


if __name__ == "__main__":
    sys.exit(main())
