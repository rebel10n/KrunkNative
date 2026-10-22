#!/usr/bin/env python3
"""The Daily Spin and the Battle Pass on an official krunker.io game (client/src/ui/v10_daily_spin.c,
battlepass_ui.c), end to end against account_unify_e2e's stubs. Nothing leaves the machine.

  python3 -I server/tests/kio_spin_battlepass_e2e.py BUILD_DIR      (run from the repository root)

A real kn_brain (scratch database) and the krunker.io stubs of krunker_account_e2e.py. The client signs in
to krunker.io and joins an official game as that account (account_unify_e2e's steps 1-3), then, in the menu:
  spin    the pass is krunker.io's (the game socket's "bpdl"); the Daily Spin's progress comes from
          api.krunker.io's GET /daily-spin/progress with the account's token, the score at its maximum: the
          item says FREE SPIN (before: nothing was asked, the item stayed 0/5000)
  claim   a click on FREE SPIN claims it the way v10.0.3 does (deob-10.0.3.js 164131-164145): the rpc
          ["_0", rpcId, "dsl"] on the game socket (rpc 111529-111540; RPC "_0" 111096, DAILY_SPIN_CLAIM "dsl"
          111109). The stub answers RPC_SUCCESS ["_0", rpcId] (111117, parsed 111438-111448: the promise
          resolves, store isClaimed = true, 164137-164140) and DAILY_SPIN_CLAIM_RESPONSE ["dslr", {ticketId}]
          ("dslr" 111361; handler 261748-261756: account.addSkin(ticketId), window.prizeWheel(0, ticketId)):
          the item says CLAIMED, the ticket's wheel (#spinUI, Starter) is open, no popup (before: a popup said
          the claim can't be made on krunker.io servers yet, and no "dsl" left the client)
  claimerr the stub refuses the rpc with RPC_ERROR ["_1", rpcId, text] (111118, 111449): v10's catch {} shows
          nothing (164141), claiming ends (164142): no popup, the item is FREE SPIN again, no wheel
  nopass  the game server's "bpdl" has no pass (null): the window says No Battle Pass Found (before: Loading
          Battle Pass for ever); the progress is refused (404): the item stays 0/5000 (before: Unavailable)
  silent  the game server never answers "bpdl": the window keeps saying Loading Battle Pass (v10's text) and asks
          again after its 15 s; it never says it could not load (before: "Could not load the Battle Pass")
  menu    in the official game the window shows krunker.io's pass; after leaving it (con disconnect) the active
          backend is krunk.io's DEV account again and the window shows the Box Office pass: kn_brain's Season 10
          (migration 0355_season_10, SEASON_CURRENT 10: "Season 10 Battle Pass", 80 levels), never "No Battle
          Pass Found" or "Loading Battle Pass" (before: the brain seeded Season 7 only, so a brain on v10.0.3's
          season 10 answered null and the window said No Battle Pass Found)
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

MODES = os.environ.get("KIO_MODES", "spin,claim,claimerr,nopass,silent,locked,menu").split(",")  # KIO_MODES=locked runs one mode
AU_SCRIPT = au.script  # account_unify's steps 1-3 (run_client takes its script from au.script, replaced per mode)
SPIN_DATA = {"score": 5000, "maxScore": 5000, "timeRemaining": 3600000, "claimed": False, "stage": 0}  # FREE SPIN
ZERO_DATA = {"score": 0, "maxScore": 5000, "timeRemaining": 3600000, "claimed": False, "stage": 0}     # 0/5000
PLAY_MORE = "Play more"  # the Box Office brain's refusal text; v10 has no such text, so the official item never shows it
NOT_YET = "can't be claimed"  # Box Office's old krunker.io text ("... on krunker.io servers yet"): gone with the claim
STARTER_TICKET = 8067  # stage 0's ticket: (stage + 1) % 7 / % 3 else Starter 8067 (deob-10.0.3.js 164104-164125)
JOIN_END = 38600  # account_unify's script from here on opens its own windows: this test writes its own
CLICK_MODES = ("claim", "claimerr", "locked")  # the modes that click the item (and read the popup / wheel after it)


async def answer_null_pass(send, msg):  # "bpdl" with no pass (the season has none)
    await send("bpdl", None)


async def never_answers(send, msg):  # the game server does not answer "bpdl"
    return


def rpc_answer(stub, ok):
    """The game server's answer to the claim rpc ["_0", rpcId, "dsl"] (deob-10.0.3.js 111529-111540, the client's
    send of RPC "_0" (111096) + rpcCounter + DAILY_SPIN_CLAIM "dsl" (111109)). The socket's parser (111438-111452)
    takes ["_0"|"_1", rpcId, results...]: RPC_SUCCESS "_0" (111117) resolves the rpc's promise with the results,
    RPC_ERROR "_1" (111118) rejects it with them. A success is followed by the push ["dslr", {ticketId}]
    (DAILY_SPIN_CLAIM_RESPONSE 111361), whose handler (261748-261756) reads ticketId, account.addSkin(ticketId)
    and window.prizeWheel(0, ticketId). The handler ignores the results, so the stub sends none; the error's
    text is the stub's own (v10's catch {} at 164141 never shows it)."""
    async def answer(send, msg):
        args = au.kae.frame_args(msg)  # ["_0", rpcId, name, ...]
        if len(args) < 3 or args[2] != "dsl":
            return
        stub.claims.append(args[1])
        if ok:
            await send("_0", args[1])
            await send("dslr", {"ticketId": STARTER_TICKET})
        else:
            await send("_1", args[1], "stub: the daily spin was refused")
    return answer


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
    if mode == "menu":  # leave the official game: the Battle Pass window on the krunk.io backend (kn_brain's pass)
        lines += ["40300 call closWind",
                  "40400 con disconnect",  # the active backend is krunk.io's DEV account again (account_backend)
                  "40700 down ESC", "40750 up ESC",
                  "41000 call showWindow 6",
                  "42000 text @kbpmenu [id=menuWindow]",
                  "42050 shot " + shots["menu"],
                  "42500 text @kbpmenu2 [id=menuWindow]",
                  "42600 shot " + shots["menu"],
                  "42700 text @kbpbot .bpBotH",  # the window's button bar: Purchase / Gift / Skip (battlepass_ui.c)
                  "43000 quit"]
        return "\n".join(lines) + "\n"
    if mode in CLICK_MODES:  # the click: the popup, the item and the wheel around it (locked: v10 does nothing)
        lines += ["40300 call closWind",
                  "40350 vis @prepop popupHolder",
                  "40400 clickid dailySpinDiv",
                  "40900 vis @postpop popupHolder spinUI",
                  "40910 text @popup [id=popupContent]",
                  "40920 text @spinhead [id=spinHeader]",
                  "40930 text @spininfo [id=spinInfo]",
                  "40950 text @dsafter [id=dailySpinDiv]",
                  "41000 shot " + shots["click"],
                  "41200 quit"]
        return "\n".join(lines) + "\n"
    lines += ["40300 quit"]
    return "\n".join(lines) + "\n"


def run_mode(port, mode):
    http_port, ws_port = au.kn_proto.free_ports(2)
    stub = au.kae.Stub(http_port, ws_port)
    stub.players = au.OTHERS
    stub.account_data = [dict(au.KIO_DATA)]
    stub.claims = []  # the rpc ids of the "dsl" claims the game socket received
    stub.daily_spin = {"spin": SPIN_DATA, "claim": SPIN_DATA, "claimerr": SPIN_DATA, "nopass": None, "silent": ZERO_DATA,
                       "locked": ZERO_DATA, "menu": ZERO_DATA}[mode]
    stub.frame_handlers = {"bpdl": {"spin": au.answer_bpdl, "claim": au.answer_bpdl, "claimerr": au.answer_bpdl,
                                    "nopass": answer_null_pass, "silent": never_answers, "locked": au.answer_bpdl, "menu": au.answer_bpdl}[mode]}
    if mode in ("claim", "claimerr"):  # the rpc frame is "_0" (RPC), the claim its third item
        stub.frame_handlers["_0"] = rpc_answer(stub, mode == "claim")
    au.WORK = tempfile.mkdtemp(prefix="kn-kiospin-%s-" % mode, dir=au.CACHE if os.path.isdir(au.CACHE) else None)
    shots = {k: os.path.join(au.WORK, "%s-%s.png" % (k, mode)) for k in ("bp", "item", "click", "menu")}
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
    elif mode in ("claim", "claimerr"):
        pop = au.vis(text, "prepop"), au.vis(text, "postpop")
        popup = au.text_of(text, "popup")
        after = au.text_of(text, "dsafter")
        head, info = au.text_of(text, "spinhead"), au.text_of(text, "spininfo")
        print("  [%s] claims: %r | popup: %r | item after: %r | wheel: %r / %r" % (mode, stub.claims, popup[:80], after[:80], head, info[:60]))
        au.check("%s: the click sends v10's claim rpc [\"_0\", rpcId, \"dsl\"] on the game socket, once, and no bare \"dsl\"" % mode,
                 len(stub.claims) == 1 and "dsl" not in stub.ws_frames, (stub.claims, stub.ws_frames[-20:]))
        au.check("%s: no popup, and no \"can't be claimed on krunker.io servers\" text" % mode,
                 pop[0].get("popupHolder") == "0" and pop[1].get("popupHolder") == "0" and NOT_YET not in popup and NOT_YET not in after,
                 (pop, popup[:120]))
        if mode == "claim":
            au.check("claim: RPC_SUCCESS marks the item CLAIMED (store isClaimed = true), not CLAIMING...",
                     "CLAIMED" in after and "CLAIMING" not in after and "FREE SPIN" not in after, after)
            au.check("claim: \"dslr\" {ticketId} opens the ticket's wheel: #spinUI shows, prizeWheel(0, 8067) = the Starter wheel",
                     pop[1].get("spinUI") == "1" and head.strip() == "SPIN" and "STARTER" in info.upper(), (pop[1], head, info[:120]))
        else:
            au.check("claimerr: RPC_ERROR shows nothing (v10's catch {}) and claiming ends: the item is FREE SPIN again",
                     "FREE SPIN" in after and "CLAIMING" not in after and "CLAIMED" not in after, after)
            au.check("claimerr: no wheel opens", pop[1].get("spinUI") in ("0", None), pop[1])
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
    elif mode == "menu":
        menu, menu2 = au.text_of(text, "kbpmenu"), au.text_of(text, "kbpmenu2")
        print("  [menu] after leaving: %r" % menu[:120])
        au.check("menu: in the official game the window shows krunker.io's pass", "KioPass" in bp and "Loading" not in bp, bp[:200])
        au.check("menu: outside the game the window shows the Box Office Season 10 pass (kn_brain, 0355_season_10)",
                 "Season 10 Battle Pass" in menu and "Level 1/80" in menu and "KioPass" not in menu, menu[:200])
        au.check("menu: never No Battle Pass Found or Loading Battle Pass outside the game",
                 all("No Battle Pass Found" not in t and "Loading" not in t for t in (menu, menu2)), (menu[:120], menu2[:120]))
        bot = au.text_of(text, "kbpbot")
        au.check("menu: the Box Office pass has v10's buttons (the VIP tier, Gift, Skip levels) and countdown",
                 "Season ends in" in menu and "Gift VIP Tier" in bot and "Skip levels" in bot
                 and ("Purchase VIP Tier" in bot or "VIP Tier Purchased" in bot), (menu[:120], bot))
    else:
        au.check("silent: the Battle Pass keeps saying Loading Battle Pass after its timeout, never that it could not load",
                 "Loading Battle Pass" in bp and "Could not load" not in bp, bp[:200])
    taken = {"silent": ("bp", "item"), "menu": ("bp", "item", "menu")}.get(mode, ("bp", "item", "click") if mode in CLICK_MODES else ("bp", "item"))
    shot_ok = [os.path.exists(p) for k, p in shots.items() if k in taken]
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
