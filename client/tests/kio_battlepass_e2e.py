#!/usr/bin/env python3
"""The Battle Pass on an official krunker.io game (client/src/ui/battlepass_ui.c, doc/v10/battlepass.md), end to end
against account_unify_e2e's krunker.io stub (server/tests/krunker_account_e2e.py). Nothing leaves the machine.

  python3 -I client/tests/kio_battlepass_e2e.py BUILD_DIR      (run from the repository root)

A real kn_brain (scratch database) and the stub. The client signs in to krunker.io and joins an official game as
that account (account_unify_e2e's steps 1-3), then, in the menu, opens the Battle Pass:
  pass    the game server's "bpdl" (the pass) and "bppd" (the progress) are krunker.io's: the window shows the season
          name, "Season ends in", "BP XP: 500 / 1000", "Level 4/10", the four tier cards, and the buttons. The buy
          and skip prices are USD ("$4.99", "$0.99"). Purchase VIP Tier and Skip levels are refused with v10's purchase
          error ("Purchase Error") and no purchase frame (xstbp / xstbpl) reaches the game server. Before: no
          refusal shows, and the prices read KR.
  silent  the game server never answers "bpdl": the window says "Loading Battle Pass" (v10's text), never
          "Could not load", and the request is sent again after the window's timeout (before: "Could not load" after 15 s)
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
import account_unify_e2e as au  # noqa: E402  (its stubs, sign-in steps and client runner; the spin test uses them too)

AU_SCRIPT = au.script  # account_unify's steps 1-3 (run_client takes its script from au.script, replaced per mode)

MODES = os.environ.get("KBP_MODES", "pass,silent").split(",")  # KBP_MODES=silent runs one mode
JOIN_END = 38600  # account_unify's script from here on opens its own windows: this test writes its own

# krunker.io's pass for the account (the game socket's "bpdl"): season 10, 10 levels of 1000 BPP, four rewards
# (itemdata "id,tier,col,sk" groups; krdata / jnkdata "id:value"), USD prices (tier1price is $4.99)
KBP_PASS = {"id": 9, "season": 10, "name": "Season 10 Battle Pass", "bpp_perlevel": 1000, "itemcount": 10,
            "tier1price": 4.99, "tier1name": "VIP", "skipprice": 0, "startdate": "2026-10-01T00:00:00.000Z",
            "enddate": "2026-12-31T00:00:00.000Z", "itemdata": "1,0,1,0,2,1,2,0,3,0,3,0,4,1,4,0",
            "leveldata": "", "krdata": "1:50,3:20,4:80", "jnkdata": "2:5"}
KBP_PROGRESS = {"bpp": 3500, "tier": 0, "items_claimed": ""}  # level 4 (500 of 1000 BP XP), no VIP tier


async def answer_pass(send, msg):  # "bpdl" DATA_LOADED then "bppd" SYNC_BP_PROG (v10.0.3 index.js:111344, 111346)
    await send("bpdl", KBP_PASS)
    await send("bppd", KBP_PROGRESS)


async def never_answers(send, msg):  # the game server does not answer "bpdl"
    return


def script(mode, shots):
    """account_unify_e2e's sign-in and official join (steps 1-3), then this mode's menu steps"""
    lines = [ln for ln in AU_SCRIPT().splitlines() if ln.strip() and int(ln.split()[0]) < JOIN_END]
    lines += ["39000 call showWindow 6",  # the Battle Pass (v10's windows[5] = showWindow 6)
              "39700 text @kbp [id=menuWindow]",
              "39720 shot " + shots["window"],
              "39900 shot " + shots["window"],  # a second capture: a frame can be missed under load
              "39750 nodes @slots .bpCardSlot",
              "39760 text @btns .bpBotH"]
    if mode == "silent":  # the window's timeout is 15 s of wall time: read it again after 16 s
        lines += ["39800 text @kbpfirst [id=menuWindow]", "55500 text @kbp [id=menuWindow]",
                  "55550 shot " + shots["silent"], "55600 quit"]
        return "\n".join(lines) + "\n"
    lines += ["40000 clicksel [onclick*=\"purchasePop\"]",  # Purchase VIP Tier
              "40300 text @pop [id=genericPop]",
              "40320 shot " + shots["purchase"],
              "40370 shot " + shots["purchase"],
              "40400 clicksel [onclick*=\"selectBPTier\"]",  # the popup's price button: selectBPTier -> refused
              "40800 text @resp [id=purchaseRespH]",
              "40950 shot " + shots["refused"],
              "41000 shot " + shots["refused"],
              "41100 call clearPops",
              "41150 call cancelPurchase",  # the first refusal's message goes: a later "Purchase Error" is the skip's
              "41500 clicksel [onclick*=\"skipLevelPop\"]",  # Skip levels
              "41700 text @skip [id=genericPop]",
              "41720 shot " + shots["skip"],
              "42000 shot " + shots["skip"],
              "41800 clicksel [onclick*=\"skipLevels(this)\"]",  # skipLevels -> refused
              "42300 text @skipresp [id=purchaseRespH]",
              "42500 quit"]
    return "\n".join(lines) + "\n"


def run_mode(port, mode):
    http_port, ws_port = au.kn_proto.free_ports(2)
    stub = au.kae.Stub(http_port, ws_port)
    stub.players = au.OTHERS
    stub.account_data = [dict(au.KIO_DATA)]
    stub.daily_spin = {"score": 0, "maxScore": 5000, "timeRemaining": 3600000, "claimed": False, "stage": 0}  # 0/5000 (the item is not under test)
    stub.frame_handlers = {"bpdl": answer_pass if mode == "pass" else never_answers}
    au.WORK = tempfile.mkdtemp(prefix="kn-kiobp-%s-" % mode, dir=au.CACHE if os.path.isdir(au.CACHE) else None)
    shots = {k: os.path.join(au.WORK, "%s-%s.png" % (k, mode)) for k in ("window", "silent", "purchase", "refused", "skip")}
    au.script = lambda: script(mode, shots)
    stub.start()
    try:
        text = au.run_client(port, stub)
    finally:
        stub.stop()
    (checks_pass if mode == "pass" else checks_silent)(text, stub, shots)


def checks_pass(text, stub, shots):
    bp = au.text_of(text, "kbp")
    btns = au.text_of(text, "btns")
    slots = au.nodes(text, "slots")
    print("  [pass] Battle Pass: %r" % bp[:160])
    au.check("pass: the window shows the season name from krunker.io's pass", "Season 10 Battle Pass" in bp, bp[:200])
    au.check("pass: \"Season ends in\" with the countdown", "Season ends in" in bp and any(c.isdigit() for c in bp.split("Season ends in")[1][:40]), bp[:200])
    au.check("pass: BP XP and the level come from the progress (3500 BPP, 1000 a level)",
             "BP XP: 500 / 1000" in bp and "Level 4/10" in bp, bp[:200])
    au.check("pass: the Battle Pass says Available BPP", "Available BPP" in bp, bp[:200])
    au.check("pass: the four tier cards of the pass are shown", slots == 4, slots)
    au.check("pass: the buttons are v10's (Purchase VIP Tier, Gift VIP Tier, Skip levels)",
             "Purchase VIP Tier" in btns and "Gift VIP Tier" in btns and "Skip levels" in btns, btns[:240])
    au.check("pass: no \"Battle Pass not found\" (the pass answered)", "not found" not in bp.lower() and "Loading" not in bp, bp[:200])
    pop = au.text_of(text, "pop")
    au.check("pass: the VIP purchase popup shows the krunker.io price in USD (\"$4.99\")", "$4.99" in pop and "KR" not in pop, pop[:200])
    resp = au.text_of(text, "resp")
    au.check("pass: Purchase VIP Tier is refused with v10's purchase error (\"Purchase Error\")", "Purchase Error" in resp, resp[:200])
    skip = au.text_of(text, "skip")
    au.check("pass: the skip popup prices a level in USD (\"$0.99\")", "$0.99" in skip and "KR" not in skip, skip[:200])
    skipresp = au.text_of(text, "skipresp")
    au.check("pass: Skip levels is refused with v10's purchase error", "Purchase Error" in skipresp, skipresp[:200])
    sent = set(stub.ws_frames)
    au.check("pass: no purchase or claim frame reaches krunker.io (xstbp, xstbpl, bpclm, bpclmAll)",
             not sent & {"xstbp", "xstbpl", "bpclm", "bpclmAll"}, sorted(sent)[-20:])
    au.check("pass: the pass was asked of the game socket (bpdl)", "bpdl" in sent, sorted(sent)[-20:])
    shot_ok = [os.path.exists(p) for k, p in shots.items() if k != "silent"]
    au.check("pass: the window screenshots were taken", all(shot_ok), shots)


def checks_silent(text, stub, shots):
    first = au.text_of(text, "kbpfirst")
    later = au.text_of(text, "kbp")
    print("  [silent] first read: %r | after 16 s: %r" % (first[:80], later[:80]))
    au.check("silent: the window says Loading Battle Pass (v10's text), not a failure",
             "Loading Battle Pass" in first and "Could not load" not in first, first[:200])
    au.check("silent: after the timeout the window still does not say it could not load",
             "Could not load" not in later and "Loading Battle Pass" in later, later[:200])
    asks = [f for f in stub.ws_frames if f == "bpdl"]
    au.check("silent: the pass is asked again after the timeout (two or more \"bpdl\")", len(asks) >= 2, len(asks))
    au.check("silent: the screenshot was taken", os.path.exists(shots["silent"]), shots["silent"])


def main():
    if au.kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{au.BUILD}/client_bin", os.X_OK) or not os.access(f"{au.BUILD}/services/brain/kn_brain", os.X_OK):
        print("kio_battlepass_e2e: no client_bin / kn_brain in %s: skipped" % au.BUILD)
        return 77
    name = "kn_test_kiobp_%d" % os.getpid()
    scratch = [sys.executable, "-I", os.path.join(au.ROOT, "services", "brain", "tests", "db_scratch.py"), au.ROOT]
    r = subprocess.run(scratch + ["create", au.DRIVER, name], capture_output=True)
    if r.returncode:
        print("kio_battlepass_e2e: %s unavailable: skipped" % au.DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=au.SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain_log = os.path.join(au.WORK, "brain.log")
    try:
        subprocess.run([f"{au.BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=au.ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{au.BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local", "--brain.id=kiobp-%d" % os.getpid(),
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
                p.send_signal(signal.SIGTERM)
    failed = au.results.count(False)
    print("kio_battlepass_e2e: %d checks, %d failed" % (len(au.results), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
