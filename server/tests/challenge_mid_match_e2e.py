#!/usr/bin/env python3
"""A real challenge completed mid-match, end to end: kn_brain + a brain-linked server_bin + a headless client_bin.

  python3 -I server/tests/challenge_mid_match_e2e.py BUILD_DIR [SOURCE_DIR]

v10.0.7's game server runs challenges.update on every event (deob-10.0.7.js:180700-180735): the event that brings a
challenge to its target sends CHALLENGE_COMPLETED [index, true, REW] (players.challengeCompleted, :176746-176748) and the
event's UPDATE_CHALLENGES_PROGRESS [CI, progVal, ...] (:180733, :180925-180934); the client shows "Finish the game to
complete: [N] challenge" in the chat and adds REW's skins to the match summary (io_challengeCompleted :225216-225229), and
uchp sets PV and PT = 1 once PV >= DAT.val (:225206-225215). The match end's finalize (:180807-180908) completes and pays
it: UPDATE_CHALLENGES, CHALLENGE_COMPLETED [index] ("Completed!" in #endChals), proc 215 "chall" and
UPDATE_CHALLENGE_REWARDS. KrunkNative (doc/v10/challenges.md 4d): server_bin follows the account's activeChallenges
(kn_brain op "challenges") and asks kn_brain ("challenge_reached") to push chgC / uchp to the account's social sockets;
kn_brain credits at the commit (P_PROGRESS).

kn_brain on a scratch SQLite database (its own config, this tree's migrations), one server_bin on Burg FFA (cheats all,
linked to kn_brain), and one headless client_bin signed in from a saved session that plays through the matchmaker:
  account   registered over the social socket; one whose daily rotation (services/brain/db_progress.c rotate) holds
            the seeded daily 16 "Get 3000 score" (REW kr 5, bpp 50, migrations 0107 / 0397). The seeded map challenge 1
            "Get 25 kills on Burg" (kr 10) is active for every account; one map challenge with a skin is added for the
            items ("k" 20 on Burg, kr 7, sk [6423]).
  round 1   the client (god mode, the bots stopped) kills auto-respawning bots with `kill @bots` until all three are
            reached: kn_brain pushes ["chgC", index, true, REW] mid-match with the index of the challenge in the
            account's list (checked against the list a second social socket of the account reads) and ["uchp", ...];
            the client's chat shows "Finish the game to complete: [<name>] challenge", the client marks each one PT
            mid-match, nothing is credited before the round ends. The round ends (server console): the commit credits
            KR 5 + 10 + 7 (kr_transactions "challenge"), the skin once (skin_transactions "challenge"), the daily's
            50 BPP (bpEnd's cbp; battlepass_progress = before + gn + cbp), player_challenges PT 1; the end screen's
            #endChals shows each one "Completed!".
  rejoin    the client quits and a new client_bin (same saved session) joins again and kills more bots: no chgC for
            the completed challenges, and the next round end pays none of them again (KR, skin, BPP: only a challenge
            this round completed for the first time, e.g. the single-match score one, is paid).
Exit 0 when every check passed, 77 when it cannot run.
"""
import asyncio
import json
import os
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

try:
    import websockets
except ImportError:  # pragma: no cover
    print("challenge_mid_match_e2e: python websockets missing: skipped")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.getcwd()
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
SERVER = os.path.join(BUILD, "server_bin")
CLIENT = os.path.join(BUILD, "client_bin")
for exe in (BRAIN, SERVER, CLIENT):
    if not os.access(exe, os.X_OK):
        print("challenge_mid_match_e2e: no %s: skipped" % exe)
        sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-chal-mid-e2e.", dir=CACHE if os.path.isdir(CACHE) else None)
DB = os.path.join(WORK, "brain.db")
SECRET = "chal-mid-e2e-%d" % os.getpid()
PASS_WORD = "chal_mid_pass_1"
DAILY, MAP_KILLS, E2E_CI = 16, 1, 900  # the seeded "Get 3000 score" daily, "Get 25 kills on Burg", the test's own row
E2E_SKIN = 6423
E2E_JNK = 1.25  # the e2e row's REW.jnk: the Rewards tab's Junk card and chlR's jnk
EXPECT_KR = {DAILY: 5, MAP_KILLS: 10, E2E_CI: 7}
DAILY_BPP = 50
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + detail) if detail and (not ok or len(detail) < 160) else ""), flush=True)
    return bool(ok)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=15.0, step=0.25):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(step)
    try:
        return fn()
    except Exception:  # noqa: BLE001
        return None


def sql(query, args=()):
    con = sqlite3.connect(DB, timeout=10)
    try:
        cur = con.execute(query, args)
        rows = cur.fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def rotation_dailies(account_id, now_s):
    """the daily rows db_progress.c rotate() keeps for the account today (the 3 lowest FNV-1a keys)"""
    period = now_s // 86400

    def key(ci):
        h = 2166136261
        for part in (ci, account_id, period):
            for b in range(8):
                h ^= (part >> (b * 8)) & 0xff
                h = (h * 16777619) & 0xffffffff
        return h
    rows = [r[0] for r in sql("SELECT challenge_id FROM challenges WHERE ctype = 'daily' AND active <> 0")]
    return sorted(rows, key=key)[:3]


class Social:
    """A second social socket of the account (enc=json): records every frame kn_brain pushes to it."""

    def __init__(self, port, name):
        self.frames = []
        self.lock = threading.Lock()
        self.loop = asyncio.new_event_loop()
        self.ws = None
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, args=(port, name), daemon=True)
        self.thread.start()
        self.ready.wait(20)

    def _run(self, port, name):
        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self._main(port, name))

    async def _main(self, port, name):
        try:
            async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=None) as ws:
                self.ws = ws
                await ws.send(json.dumps(["a", 1, [name, PASS_WORD]]))
                while True:
                    f = json.loads(await ws.recv())
                    with self.lock:
                        self.frames.append(f)
                    if f and f[0] == "a":
                        self.ready.set()
        except Exception:  # noqa: BLE001 - closed at the end
            self.ready.set()

    def send(self, frame):
        if self.ws:
            asyncio.run_coroutine_threadsafe(self.ws.send(json.dumps(frame)), self.loop).result(10)

    def get(self, label, since=0):
        with self.lock:
            return [f for f in self.frames[since:] if f and f[0] == label]

    def mark(self):
        with self.lock:
            return len(self.frames)

    def close(self):
        if self.ws:
            try:
                asyncio.run_coroutine_threadsafe(self.ws.close(), self.loop).result(5)
            except Exception:  # noqa: BLE001
                pass


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, PASS_WORD, None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                return (f[2], f[5]) if f[1] is None else (0, "")


class Client:
    """A headless client_bin that plays (KRUNKNATIVE_INPUT_SCRIPT "play") and takes KRUNKNATIVE_UI_CMDS lines."""

    def __init__(self, tag, brain_port, cfg):
        self.tag = tag
        self.log = os.path.join(WORK, tag + ".log")
        self.cmds = os.path.join(WORK, tag + ".cmds")
        open(self.cmds, "w").close()
        script = os.path.join(WORK, tag + ".script")
        with open(script, "w") as f:
            f.write("0 play\n")
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_DEBUG": "2", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                    "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                    "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                    "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port, "KRUNKNATIVE_ALT_KEYSTORE": "file",
                    "KRUNKNATIVE_ASSETS_DIR": ASSETS})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT], cwd=ROOT, stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(self.proc)

    def text(self):
        return read(self.log)

    def cmd(self, line, wait=0.0):
        with open(self.cmds, "a") as f:
            f.write(line + "\n")
        if wait:
            time.sleep(wait)

    def textof(self, elem, timeout=8.0):
        mark = len(self.text())
        self.cmd("textof " + elem)
        m = wait_for(lambda: re.search(r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(elem), self.text()[mark:]), timeout, 0.1)
        return m.group(1) if m else None

    def quit(self):
        self.cmd("quit")
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
        self.out.close()


def challenge_kr(account_id):
    return sql("SELECT ref, amount FROM kr_transactions WHERE account_id = ? AND source = 'challenge' ORDER BY rowid", (account_id,))


def challenge_skins(account_id):
    return sql("SELECT skin_id, delta FROM skin_transactions WHERE account_id = ? AND source = 'challenge' ORDER BY rowid", (account_id,))


def bpp_total(account_id):
    rows = sql("SELECT bpp FROM battlepass_progress WHERE account_id = ? ORDER BY bp_id DESC", (account_id,))
    return rows[0][0] if rows else 0


def kill_until(client, social, since, want, rounds=24):
    """`kill @bots` every 2.5 s until the mid-match chgC of every CI in `want` arrived (or `rounds` tries)"""
    for _ in range(rounds):
        got = {social_list_ci(social, f[1]) for f in social.get("chgC", since) if isinstance(f[1], int)}
        if want <= got:
            return got
        client.cmd("con kill @bots", 2.5)
    return {social_list_ci(social, f[1]) for f in social.get("chgC", since) if isinstance(f[1], int)}


LIST = []


def social_list_ci(social, index):
    return LIST[index]["CI"] if 0 <= index < len(LIST) else None


def round_end(server):
    server.stdin.write("round end\n")
    server.stdin.flush()


def run(brain_port, game_port):
    # the account: one whose daily rotation holds the "Get 3000 score" daily today
    stamp = os.getpid() % 100000
    name = account_id = token = None
    for i in range(24):
        n = "chm%d_%d" % (stamp, i)
        aid, tok = asyncio.run(register(brain_port, n))
        if not aid:
            continue
        if DAILY in rotation_dailies(aid, int(time.time())):
            name, account_id, token = n, aid, tok
            break
    if not check("an account whose dailies hold the seeded \"Get 3000 score\" (CI %d)" % DAILY, account_id):
        return
    sql("INSERT INTO challenges (challenge_id, ctype, dat, rew, icon, name, sort) VALUES (?, 'map', ?, ?, 0, ?, 16)",
        (E2E_CI, '{"ky":"k","val":20,"mp":0}', '{"kr":7,"jnk":%s,"sk":[%d]}' % (E2E_JNK, E2E_SKIN), "Get 20 kills on Burg (e2e)"))
    daily_rew = json.loads(sql("SELECT rew FROM challenges WHERE challenge_id = ?", (DAILY,))[0][0])
    check("the daily carries 0397's REW.bpp", daily_rew.get("bpp") == DAILY_BPP and daily_rew.get("kr") == 5, str(daily_rew))

    social = Social(brain_port, name)
    check("a second social socket of the account signs in", bool(social.get("a")))
    social.send(["chg"])
    lst = wait_for(lambda: social.get("chgL"), 10)
    LIST[:] = lst[-1][1] if lst else []
    cis = [c["CI"] for c in LIST]
    check("the account's activeChallenges hold the daily, the Burg kills and the e2e row",
          all(ci in cis for ci in (DAILY, MAP_KILLS, E2E_CI)), str(cis))
    names = {c["CI"]: c.get("N") or "" for c in LIST}

    cfg = os.path.join(WORK, "client-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for k, v in (("krunker_username", name), ("krunker_token", token), ("knRuleset", "v10")):
        with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
            f.write(v)

    kr0, skins0, bpp0 = challenge_kr(account_id), challenge_skins(account_id), bpp_total(account_id)
    game_log = os.path.join(WORK, "game.log")
    server = procs[1]

    # ---- round 1: reached mid-match
    c = Client("client1", brain_port, cfg)
    joined = wait_for(lambda: re.search(r"\[accounts\] .*%s" % re.escape(name), read(game_log)) and "challenges" in read(game_log)
                      or "connected to server" in c.text(), 120, 0.5)
    check("the client signs in and joins the brain-linked server", joined, c.text()[-400:])
    if not joined:
        return
    time.sleep(6.0)  # the map, the spawn, the account's challenge list on the server
    since = social.mark()
    c.cmd("con god", 0.5)
    c.cmd("con bot_stop on", 0.5)
    c.cmd("con bot 8", 4.0)
    got = kill_until(c, social, since, {DAILY, MAP_KILLS, E2E_CI})
    check("mid-match chgC for the daily, the Burg kills and the e2e row", {DAILY, MAP_KILLS, E2E_CI} <= got, str(got) + " " + read(game_log)[-600:])
    mids = [f for f in social.get("chgC", since) if isinstance(f[1], int)]
    for ci in (DAILY, MAP_KILLS, E2E_CI):
        f = next((f for f in mids if social_list_ci(social, f[1]) == ci), None)
        idx = cis.index(ci)
        check("CI %d: chgC [index %d, true, REW] (its index in activeChallenges)" % (ci, idx),
              f is not None and f[1] == idx and f[2] is True and f[3] == LIST[idx]["REW"], str(f))
    check("one mid-match chgC per challenge", len(mids) == len({f[1] for f in mids}), str(mids))
    uchp = [f for f in social.get("uchp", since)]
    pairs = {}
    for f in uchp:
        d = f[1]
        for i in range(0, len(d) - 1, 2):
            pairs[d[i]] = d[i + 1]
    check("uchp carries the Burg kills and the e2e row at their targets (PV 25, 20)",
          pairs.get(MAP_KILLS) == 25 and pairs.get(E2E_CI) == 20, str(pairs))
    chat = c.textof("chatList") or ""
    for ci in (DAILY, MAP_KILLS, E2E_CI):
        nm = names[ci] or {DAILY: "Get 3000 score"}.get(ci, "")
        check("chat: \"Finish the game to complete: [%s] challenge\"" % nm, "Finish the game to complete: [%s] challenge" % nm in chat, chat[-300:])
    marks = re.findall(r"\[challenges\] CI (\d+) PV ([\d.]+) PT 1", c.text())
    marked = {int(m[0]) for m in marks}
    check("the client marks the Burg kills and the e2e row PT mid-match (uchp)", {MAP_KILLS, E2E_CI} <= marked, str(marks[-6:]))
    check("nothing credited before the round ends", challenge_kr(account_id) == kr0 and challenge_skins(account_id) == skins0
          and not sql("SELECT 1 FROM player_challenges WHERE account_id = ? AND pt = 1", (account_id,)))
    c.cmd("shot " + os.path.join(WORK, "1-mid-match.png"), 1.0)

    since_end = social.mark()
    round_end(server)
    done = wait_for(lambda: social.get("chlR", since_end) and social.get("bpEnd", since_end), 30)
    check("the round end commits: chlR and bpEnd", bool(done), str(social.frames[since_end:])[-400:])
    done_rows = {r[0] for r in sql("SELECT challenge_id FROM player_challenges WHERE account_id = ? AND pt = 1", (account_id,))}
    check("player_challenges PT 1", {DAILY, MAP_KILLS, E2E_CI} <= done_rows, str(done_rows))
    # the bots' kills may complete more of the account's challenges (the kill streak ones): the commit pays them all in
    # one proc-215 credit (deob-10.0.7.js:180855-180878), so the KR is the sum of every completed REW.kr
    rew = {c["CI"]: c["REW"] for c in LIST}
    want_kr = sum(int(rew.get(ci, {}).get("kr", 0)) for ci in done_rows)
    want_bpp = sum(int(rew.get(ci, {}).get("bpp", 0)) for ci in done_rows)
    kr1 = challenge_kr(account_id)
    new_kr = [a for _, a in kr1[len(kr0):]]
    check("KR credited once: %d (5 + 10 + 7 and any other completed challenge)" % want_kr,
          new_kr == [want_kr] and want_kr >= sum(EXPECT_KR.values()), str(kr1))
    sk1 = challenge_skins(account_id)
    check("the skin credited once", sk1[len(skins0):] == [(E2E_SKIN, 1)], str(sk1))
    bp = social.get("bpEnd", since_end)
    t = bp[-1][1] if bp else {}
    bpp1 = bpp_total(account_id)
    check("BPP: bpEnd's cbp is the daily's %d" % DAILY_BPP, t.get("cbp") == DAILY_BPP == want_bpp, str(t)[:200])
    check("BPP: battlepass_progress = before + gn + cbp", bpp1 == bpp0 + t.get("gn", -1) + t.get("cbp", -1), "%s -> %s %s" % (bpp0, bpp1, t))
    chlr = social.get("chlR", since_end)[-1]
    check("chlR [skins, funds, junk, jnk, kr, ...]: the skin and the KR", chlr[1] == [E2E_SKIN] and chlr[5] == want_kr, str(chlr))
    # the mid-match chgC's REW.jnk went to matchRewardSummary.junk (deob-10.0.7.js:225219): the Rewards tab's first card is
    # "<x.xx> Junk" (tabs.rewards.gen :210029-210052), the sum over the challenges completed mid-match
    want_jnk = sum(float(rew.get(ci, {}).get("jnk", 0) or 0) for ci in got)
    card = wait_for(lambda: (lambda t: t if t and t.endswith("Junk") else None)(c.textof(".rewItemHolder .rewItemN", 2.0)), 30, 1.0)
    # (the same card also counts Junk the match's kill pools found, "You found [x] Junk": at least the challenges')
    card_jnk = float(card.split()[0]) if card and re.fullmatch(r"\d+\.\d\d Junk", card) else -1.0
    check("the end screen's Rewards tab: the mid-match Junk card (at least %.2f Junk)" % want_jnk,
          want_jnk >= E2E_JNK and card_jnk >= want_jnk - 0.005, str(card))
    done_jnk = sum(float(rew.get(ci, {}).get("jnk", 0) or 0) for ci in done_rows)
    check("chlR's jnk: the challenges' Junk credited at the round end (%.2f)" % done_jnk, abs(float(chlr[4] or 0) - done_jnk) < 0.005, str(chlr))
    ends = wait_for(lambda: (c.textof("endChals") or "").count("Completed!") >= 3 and c.textof("endChals"), 15)
    check("the end screen shows each one \"Completed!\"", ends and all((names[ci] or "Get 3000 score") in ends for ci in (DAILY, MAP_KILLS, E2E_CI)),
          str(ends))
    c.cmd("shot " + os.path.join(WORK, "2-end.png"), 1.0)
    c.quit()

    # ---- the rejoin: nothing twice (into the next round, once the end screen is over)
    check("the next round starts", wait_for(lambda: "Loaded map 0 mode 0" in read(game_log), 120, 0.5), read(game_log)[-300:])
    kr2, sk2 = challenge_kr(account_id), challenge_skins(account_id)
    c2 = Client("client2", brain_port, cfg)
    joined = wait_for(lambda: c2.text().count("connected to server") >= 1, 120, 0.5)
    check("the client rejoins", joined, c2.text()[-300:])
    time.sleep(6.0)
    since2 = social.mark()
    c2.cmd("con god", 0.5)
    c2.cmd("con bot 8", 3.0)
    for _ in range(6):
        c2.cmd("con kill @bots", 2.5)
    again = [f for f in social.get("chgC", since2) if isinstance(f[1], int) and social_list_ci(social, f[1]) in (DAILY, MAP_KILLS, E2E_CI)]
    check("no mid-match chgC again for a completed challenge", not again, str(again))
    since_end = social.mark()
    round_end(server)
    if not wait_for(lambda: social.get("bpEnd", since_end), 20):
        round_end(server)
        wait_for(lambda: social.get("bpEnd", since_end), 20)
    check("the rejoin's round is committed (final)", len(re.findall(r"\[stats\] final %s" % re.escape(name), read(game_log))) >= 2,
          read(game_log)[-400:])
    time.sleep(2.0)
    # this round may complete other challenges of the account (a single-match score one): only those are paid
    done2 = {r[0] for r in sql("SELECT challenge_id FROM player_challenges WHERE account_id = ? AND pt = 1", (account_id,))}
    fresh = done2 - done_rows
    want_kr2 = sum(int(rew.get(ci, {}).get("kr", 0)) for ci in fresh)
    kr3 = challenge_kr(account_id)
    check("the rejoin's round end credits no challenge KR again (only newly completed ones: %s)" % sorted(fresh),
          [a for _, a in kr3[len(kr2):]] == ([want_kr2] if want_kr2 else []), str(kr3))
    check("... and no skin again", challenge_skins(account_id) == sk2, str(challenge_skins(account_id)))
    bp = social.get("bpEnd", since_end)
    want_bpp2 = sum(int(rew.get(ci, {}).get("bpp", 0)) for ci in fresh)
    check("... and no challenge BPP again (cbp %d)" % want_bpp2, bool(bp) and bp[-1][1].get("cbp") == want_bpp2, str(bp)[:200])
    again = [f for f in social.get("chgC", since_end) if not isinstance(f[1], int)
             and any(c_.get("CI") in (DAILY, MAP_KILLS, E2E_CI) for c_ in f[1])]
    check("... and no \"Completed!\" card again", not again, str(again)[:200])
    c2.quit()
    social.close()


def main():
    brain_port, game_port = free_ports(2)
    blog = os.path.join(WORK, "brain.log")
    bcfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
    with open(bcfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": brain_port, "federation": False, "internal_secret": SECRET, "region": "local"},
                   "finder": {"poll_interval_s": 1},
                   "accounts": {"signup_rewards": {"enabled": False}, "sale_grace_s": 0, "free_premium": False, "starting_kr": 0,
                                "login_throttle_s": 0, "password_iterations": 1000, "skins_file": os.path.join(ASSETS, "skins.json")}}, f)
    ok = False
    try:
        procs.append(subprocess.Popen([BRAIN, "--config", bcfg, "--migrate"], cwd=ROOT, stdout=open(blog, "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        up = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:%d" % brain_port, read(blog)), 30)
        if not check("kn_brain starts", up, read(blog)[-300:]):
            return 1
        gcfg = os.path.join(WORK, "game.json")
        with open(gcfg, "w") as f:
            json.dump({"gameserver": {"node_id": "CHM", "name": "challenge mid-match e2e", "listen_port": game_port, "region": "local",
                                      "public_host": "127.0.0.1", "modes": ["ffa"], "maps": ["Burg"], "max_players": 8,
                                      "shutdown_grace": 0, "idle_kick_seconds": 0, "ruleset": "v10", "cheats": "all",
                                      "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port, "brain_secret": SECRET,
                                      "finder_url": "http://127.0.0.1:%d" % brain_port, "bans_file": os.path.join(WORK, "bans.json")}}, f)
        procs.append(subprocess.Popen([SERVER, "--headless", "--config", gcfg, "--cfg", "none"], cwd=ROOT, stdin=subprocess.PIPE, text=True,
                                      stdout=open(os.path.join(WORK, "game.log"), "w"), stderr=subprocess.STDOUT, start_new_session=True))
        linked = wait_for(lambda: "welcomed" in read(os.path.join(WORK, "game.log")) or "linked" in read(os.path.join(WORK, "game.log")), 30)
        if not check("server_bin links to kn_brain", linked, read(os.path.join(WORK, "game.log"))[-300:]):
            return 1
        run(brain_port, game_port)
        ok = bool(results) and all(results)
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        for p in procs:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except OSError:
                    pass
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    ok = bool(results) and all(results)
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print("%d/%d passed" % (sum(results), len(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
