#!/usr/bin/env python3
"""One account UI, two backends (client/include/backend_ui.h active_account()), end to end.

  python3 -I server/tests/account_unify_e2e.py BUILD_DIR      (run from the repository root)

The client has two account backends: krunk.io (kn_brain) and krunker.io (krunker_account.h). v10's one
account UI (header, login modal, Account window, player list with its dev actions, alt manager) shows
and signs in to the ACTIVE BACKEND's account: krunker.io's in an official game, krunk.io's elsewhere.

A real kn_brain (scratch sqlite database) with one DEV account, the krunker.io stubs of
krunker_account_e2e.py (API, FRVR auth, matchmaker, game server), one headless client_bin (--offline:
the sandbox is the menu's game; the brain still signs in):
  1. krunk.io: v10's login modal signs in the DEV account; the header, the player list (Alt) with the
     DEV's Kill / TPAll and the alt manager show it
  2. an official game, krunker.io guest: the header is v10's guest bar, the player list shows no DEV
     name and no dev actions, the alt manager marks no account active and offers Login; showWindow(5)
     opens v10's login modal, and a click on its background closes it (v10's view stack)
  3. official, signed in to krunker.io from that modal: the header, player list and Account window are
     the krunker.io account's, still no dev actions
  4. leaving the game switches back at once: the DEV account again (header, player list, alt manager),
     still signed in (the krunker.io session never touched it), and the krunker.io session is kept
     too (the next official join carries its token)
  5. that join's "a" says the krunker.io account is a DEV (player_developer 1): v10's isDev actions
  7. child windows: the editor (`con editor`, its own process: app_window.c) opened in the official game
     is on krunker.io too (the router's session: KRUNKNATIVE_APP_BACKEND at spawn, then "backend" lines),
     and follows the router back to krunk.io when the player leaves (its log says which backend it plays on)
     (Kill, TPAll, TP2 / TPME, the punish popup) show in the player list
The official game server stub spawns players like a live one: "0" ADD_PLAYERS with two other players
after init and ours (io-init's socket id) on the client's "en" (respawn in the script). The player list
of 2. and 3. lists our row and theirs. Its staff buttons follow the active account's krunker.io rights
from "a" only: none for the guest (2.), the moderator's (3.: "a" player_role 2, v10 isMod: the punish
popup, canGlobalKick, but no isDev Kill / TPAll / teleports), the DEV's (5.). ADD_PLAYERS carries no
other player's role (v10 knows only its own account's rights), so the other rows are plain.
Exit 0 when every check passed, 77 when it cannot run.
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
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_acctunify_%d" % os.getpid()
SECRET = "kn-acctunify-%d" % os.getpid()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-account-unify-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
DEV = "unidev%d" % (os.getpid() % 100000)
PW = "pw-unify-%d" % os.getpid()
SIGNED = "signedInHeaderBar signedOutHeaderBar knV10Login"
# the other players in the official game (the stub's "0"): (socket id, sid, name)
OTHERS = [("sockALLY", 5, "StubAlly"), ("sockFOE", 6, "StubFoe")]
GUEST = "Guest_%d" % kae.MY_SID  # our player's name in the guest's "0"
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    detail = str(detail)
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail[:400]) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {}, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def vis(text, label):
    m = re.search(r"\[input_script\] vis t=\d+ @%s (.*)" % re.escape(label), text)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


def window_probe(t, label, window):
    """showWindow(window), then its text, the dev actions, the alt manager's active row and Login"""
    return [f"{t} call showWindow {window}",
            f"{t + 700} text @{label} [id=menuWindow]",
            f"{t + 710} nodes @{label}dev .punishButton.tpall",
            f"{t + 720} nodes @{label}kill .punishButton.kill",
            f"{t + 730} nodes @{label}active .knAltActive",
            f"{t + 740} nodes @{label}signin [id=knAltSignIn]",
            f"{t + 750} text @{label}name .knAltActive .knAltName",
            f"{t + 760} text @{label}rows [id=playerListH]",
            f"{t + 770} nodes @{label}punish .punishButton.kick",
            f"{t + 780} nodes @{label}tp .punishButton.tp2",
            f"{t + 800} call closWind"]


def script():
    s = ["0 play",  # the sandbox: our player is in the player list
         "1500 down ESC", "1520 up ESC",
         "2000 vis @guest " + SIGNED,
         # 1. krunk.io: v10's login modal signs in the DEV account
         "2100 clickid knV10LoginBtn",
         "2500 vis @knmodal knV10Login accEmail",
         "2600 clickid knV10LoginMode",
         "2900 clickid accName", "2950 type " + DEV,
         "3100 clickid accPass", "3150 type " + PW,
         "3300 clickid knV10LoginSubmit",
         "7000 vis @kn " + SIGNED,
         "7050 text @knname [id=knV10Name]"]
    s += window_probe(7100, "knplist", 23)
    s += window_probe(8100, "knalt", 104)
    # 2. an official game as a krunker.io guest
    s += ["8900 call showWindow 2",  # (the official lobbies load with the Servers window)
          "12000 call closWind",
          '12100 call krunkerJoin "%s"' % kae.GAME_PICK,
          # Play: the client's "en", the server's "0" spawns us; ESC brings the menu back
          "15000 respawn", "16300 players", "16400 down ESC", "16450 up ESC",
          "16900 con editor",  # 7. a child window opened by the official game
          "17000 vis @kio0 " + SIGNED]
    s += window_probe(17100, "gplist", 23)
    s += window_probe(18100, "galt", 104)
    s += ["19000 call showWindow 5",
          "19600 vis @gmodal knV10Login accEmail",
          "19700 click 20 20",  # the view stack's background (v10: the overlay closes the view)
          "20200 nodes @gmodalgone [id=knV10Login]",
          "20300 vis @gafter " + SIGNED,
          # 3. sign in to krunker.io from v10's login modal
          "20500 call loginOrRegister",
          "21000 clickid knV10LoginMode",
          "21200 clickid accName", "21250 type " + kae.LOGIN,
          "21400 clickid accPass", "21450 type " + kae.PASSWORD,
          "21600 clickid knV10LoginSubmit",
          "23000 clickid knCaptchaToken", "23100 type " + kae.TURNSTILE, "23200 clickid knCaptchaUse"]
    t = 25500
    for i, d in enumerate(kae.CODE):
        s += ["%d clickid knV10Code%d" % (t, i), "%d type %s" % (t + 60, d)]
        t += 150
    s += ["28500 vis @kio " + SIGNED,
          "28550 text @kioname [id=knV10Name]",
          # the krunker.io account's real data (the game server's "a"): KR, junk, ranked points
          "28560 text @kiokr [id=menuKRCount]", "28570 text @kiojnk [id=menuJNKCount]", "28580 text @kiorp [id=menuRPCount]"]
    s += window_probe(28600, "kplist", 23)
    s += window_probe(29600, "kalt", 104)
    kacc = window_probe(30600, "kacc", 5)  # (its own probes before its closWind: the script runs in order)
    s += kacc[:-1] + ["31390 text @kioaccname [id=knKioAccName]", "31395 text @kioaccstat .statTabB0"] + kacc[-1:]
    # 4. leave: krunk.io's DEV again, both sessions kept
    s += ["31600 con disconnect",
          "31800 down ESC", "31850 up ESC",
          '32500 nodes @backout [id=signedOutHeaderBar][style*="none"]',
          '32510 nodes @backin [id=signedInHeaderBar][style*="none"]',
          "32550 text @backname [id=knV10Name]", "32560 text @backkr [id=menuKRCount]"]
    s += window_probe(32600, "balt", 104)
    s += window_probe(33600, "bplist", 23)
    s += ["34300 con editor",  # 7. the child window follows the router back to krunk.io
          '34600 call krunkerJoin "%s"' % kae.GAME_PICK,
          "37200 respawn", "38200 down ESC", "38250 up ESC"]
    # 5. that game's "a": the krunker.io account is a DEV
    s += window_probe(38600, "dplist", 23)
    # 6. the krunker.io account's Battle Pass and Clan windows, fetched over the game socket as v10 does
    s += ["39500 call showWindow 6", "40300 text @kbp [id=menuWindow]", "40350 call closWind",
          "40400 call showWindow 13", "41300 text @kclan [id=menuWindow]", "41350 call closWind",
          "41400 text @kiokr2 [id=menuKRCount]",
          "41600 quit"]
    return "\n".join(s) + "\n"


def run_client(brain_port, stub):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script())
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    log = os.path.join(WORK, "client.log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off"})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(240)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client finished its script", False, "timed out; log " + log)
    return read_log(log)


# What krunker.io's game server says of the account in "a" (v10's new Account(id, name, data), v10:112759)
KIO_DATA = {"player_score": 40000, "player_kills": 321, "player_deaths": 123, "player_wins": 17, "player_games_played": 40,
            "player_funds": 1234, "player_jnk": 5.5, "ranked_points": 77, "player_clan": "KIOC", "player_timeplayed": 3600000,
            "player_stats": json.dumps({"hs": 55, "s": 1000, "h": 400}), "skins": [{"ind": 101, "cnt": 1}, {"ind": 202, "cnt": 2}],
            "player_badge": "1,3"}
KIO_PASS = {"id": 7, "season": 10, "name": "KioPass", "bpp_perlevel": 1000, "itemcount": 0, "tier1price": 900, "skipprice": 100,
            "startdate": "2026-10-01T00:00:00.000Z", "enddate": "2026-12-31T00:00:00.000Z", "itemdata": "[]", "leveldata": "[]",
            "krdata": "[]", "jnkdata": "[]"}
KIO_CLAN = {"clan_name": "KIOC", "clan_id": 9, "clan_score": 5000, "clan_funds": 250, "clan_playerid": kae.ACCOUNT_ID,
            "members": [{"p": kae.ACCOUNT_NAME, "i": kae.ACCOUNT_ID, "r": 3}], "requests": []}


async def answer_bpdl(send, msg):  # "bpdl" DATA_LOADED + "bppd" SYNC_BP_PROG (v10:218178 / 218181), then a KR change ("uf")
    await send("bpdl", KIO_PASS)
    await send("bppd", {"bpp": 3500, "tier": 1, "items_claimed": ""})
    await send("uf", 1500, 6.25)


async def answer_cln(send, msg):  # sendReq(3): the clan page ("cln" [msg, clanData, ok, action], v10:213437)
    await send("cln", "", KIO_CLAN, 1, 3)


def checks(text, stub):
    g = vis(text, "guest")
    check("start: a krunk.io guest (v10's guest bar)", g.get("signedOutHeaderBar") == "1", g)
    check("the sign-in window is v10's login modal", vis(text, "knmodal").get("knV10Login") == "1", vis(text, "knmodal"))
    k = vis(text, "kn")
    check("1. krunk.io: the DEV account signed in (v10's signed-in bar)", k.get("signedInHeaderBar") == "1" and k.get("knV10Login") == "-", k)
    check("1. the menu account box names the DEV account", DEV in text_of(text, "knname"), text_of(text, "knname"))
    check("1. the player list (Alt) names the DEV account and shows its dev actions (TPAll)",
          DEV in text_of(text, "knplistrows") and nodes(text, "knplistdev") >= 1, (text_of(text, "knplistrows")[:120], nodes(text, "knplistdev")))
    check("1. the alt manager marks the DEV account active", DEV in text_of(text, "knaltname") and nodes(text, "knaltactive") == 1,
          (text_of(text, "knaltname"), nodes(text, "knaltactive")))

    check("2. joining the official game makes krunker.io the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:3])
    seeks = stub.find("GET", "/mm/seek-game")
    check("2. the join is a krunker.io guest's (no accessToken)", seeks and "accessToken" not in seeks[0][2], [("accessToken" in x[2]) for x in seeks])
    g0 = vis(text, "kio0")
    check("2. official guest: the menu account box is v10's guest bar, not the DEV account",
          g0.get("signedOutHeaderBar") == "1" and g0.get("signedInHeaderBar") != "1", g0)
    pl = text_of(text, "gplistrows")
    check("2. official guest: the player list does not show the krunk.io account", DEV not in pl, pl[:200])
    players = re.search(r"\[input_script\] players t=\d+ n=\d+(.*)", text)
    check("2. the stub's \"0\" spawned our player (sid %d, ours) and the two others" % kae.MY_SID,
          players and re.search(r" %d:%s@[^ ]*\*" % (kae.MY_SID, GUEST), players.group(1)) and
          all(" %d:%s@" % (sid, n) in players.group(1) for _, sid, n in OTHERS), players.group(0) if players else None)
    check("2. official guest: the player list has our row and the other players' rows",
          GUEST in pl and all(n in pl for _, _, n in OTHERS), pl[:200])
    check("2. our player is the first row only (krunker.io is config.isProd: the list leaves us out)",
          pl.startswith(GUEST) and pl.count(GUEST) == 1, pl[:200])
    check("2. official guest: no krunk.io dev actions in the player list (no Kill, no TPAll)",
          nodes(text, "gplistdev") == 0 and nodes(text, "gplistkill") == 0, (nodes(text, "gplistdev"), nodes(text, "gplistkill")))
    check("2. official guest: no staff actions at all (no punish popup, no TP2)",
          nodes(text, "gplistpunish") == 0 and nodes(text, "gplisttp") == 0, (nodes(text, "gplistpunish"), nodes(text, "gplisttp")))
    check("2. official guest: the alt manager lists no krunk.io account and marks none active",
          nodes(text, "galtactive") == 0 and not text_of(text, "galtname"), (text_of(text, "galtname"), nodes(text, "galtactive")))
    check("2. official guest: the alt manager offers v10's Login", nodes(text, "galtsignin") == 1, nodes(text, "galtsignin"))
    check("2. showWindow(5) as the krunker.io guest: v10's login modal", vis(text, "gmodal").get("knV10Login") == "1", vis(text, "gmodal"))
    check("2. a click on the modal's background closes it (v10's view stack)", nodes(text, "gmodalgone") == 0, nodes(text, "gmodalgone"))
    check("2. ... and the menu is still up", vis(text, "gafter").get("signedOutHeaderBar") == "1", vis(text, "gafter"))
    check("2. no kn_brain login was sent with the krunker.io credentials", kae.LOGIN not in read_log(os.path.join(WORK, "brain.log")), "")

    k = vis(text, "kio")
    check("3. official, signed in to krunker.io from v10's login modal: v10's signed-in bar",
          k.get("signedInHeaderBar") == "1" and k.get("signedOutHeaderBar") == "0" and k.get("knV10Login") == "-", k)
    name = text_of(text, "kioname")
    check("3. the menu account box names the krunker.io account, not the DEV", kae.ACCOUNT_NAME in name and DEV not in name, name)
    pl = text_of(text, "kplistrows")
    check("3. the player list does not show the krunk.io account", DEV not in pl, pl[:200])
    check("3. the player list has the krunker.io account's row and the other players' rows",
          kae.ACCOUNT_NAME in pl and all(n in pl for _, _, n in OTHERS), pl[:200])
    check("3. the server's \"a\" made the krunker.io account a moderator (player_role 2)",
          stub.a_sent[:1] and stub.a_sent[0].get("player_role") == 2, stub.a_sent)
    check("3. the moderator's staff action shows: v10's punish popup on each other player's row (canGlobalKick)",
          nodes(text, "kplistpunish") == len(OTHERS), nodes(text, "kplistpunish"))
    check("3. still no DEV actions: no Kill, no TPAll, no TP2 (isDev only; neither the krunk.io DEV's nor a moderator's)",
          nodes(text, "kplistdev") == 0 and nodes(text, "kplistkill") == 0 and nodes(text, "kplisttp") == 0,
          (nodes(text, "kplistdev"), nodes(text, "kplistkill"), nodes(text, "kplisttp")))
    check("3. the alt manager lists no krunk.io account", nodes(text, "kaltactive") == 0 and not text_of(text, "kaltname"),
          (text_of(text, "kaltname"), nodes(text, "kaltactive")))
    acc = text_of(text, "kacc")
    check("3. the Account window is the krunker.io account's", kae.ACCOUNT_NAME in acc and DEV not in acc, acc[:200])

    check("4. leaving switches back to krunk.io", "[account] active account backend: krunk.io" in text, re.findall(r"\[account\] active.*", text)[:4])
    check("4. at once: v10's signed-in bar again (the guest bar hidden)", nodes(text, "backout") == 1 and nodes(text, "backin") == 0,
          (nodes(text, "backout"), nodes(text, "backin")))
    check("4. ... naming the DEV account: the krunker.io session never signed it out", DEV in text_of(text, "backname"), text_of(text, "backname"))
    check("4. the krunk.io account stayed signed in throughout", "[account] logged out" not in text, re.findall(r"\[account\] logged.*", text)[:2])
    check("4. the alt manager marks the DEV account active again", DEV in text_of(text, "baltname") and nodes(text, "baltactive") == 1,
          (text_of(text, "baltname"), nodes(text, "baltactive")))
    check("4. the krunker.io session is kept too: the next official join carries its token",
          len(seeks) >= 2 and seeks[-1][2].get("accessToken") == stub.access2, [("accessToken" in x[2]) for x in seeks])

    check("3. the header shows the krunker.io account's KR, junk and ranked points (its \"a\" data)",
          # ("a" says 1,234 KR / 5.50 junk; the battle pass answer's "uf" may have made it 1,500 / 6.25 by then)
          text_of(text, "kiokr") in ("1,234", "1,500") and text_of(text, "kiojnk") in ("5.50", "6.25") and text_of(text, "kiorp") == "77",
          (text_of(text, "kiokr"), text_of(text, "kiojnk"), text_of(text, "kiorp")))
    check("3. the Account window shows its clan and its stats", "[KIOC]" in text_of(text, "kioaccname") and re.search(r"KR1,(234|500)", text_of(text, "kioaccstat"))
          and "Kills321" in text_of(text, "kioaccstat")
          and "40,000" in text_of(text, "kioaccstat"), (text_of(text, "kioaccname"), text_of(text, "kioaccstat")[:200]))
    check("4. back on krunk.io the header's KR is the DEV account's again", text_of(text, "backkr") not in ("1,234", "1,500"), text_of(text, "backkr"))
    check("6. the Battle Pass is fetched over the game socket (\"bpdl\", as v10 sends it)", "bpdl" in stub.ws_frames, stub.ws_frames[-20:])
    check("6. the Battle Pass window shows krunker.io's pass", "KioPass" in text_of(text, "kbp"), text_of(text, "kbp")[:200])
    check("6. the Clans window asks the game socket for the clan page (\"cln\" 3)", "cln" in stub.ws_frames, stub.ws_frames[-20:])
    check("6. the Clans window shows the krunker.io account's clan", "KIOC" in text_of(text, "kclan"), text_of(text, "kclan")[:200])
    check("6. a KR change from the game server (\"uf\") reaches the header", text_of(text, "kiokr2") == "1,500", text_of(text, "kiokr2"))
    writes = sorted({f for f in stub.ws_frames if f in ("bpclm", "bpclmAll", "unbx", "spin", "crft", "nme", "fo", "clnR", "clnRA", "flg", "bdg", "trd", "strd")})
    check("6. nothing that spends KR or changes the krunker.io account went to its game server", not writes, writes)

    pl = text_of(text, "dplistrows")
    check("5. the next game's \"a\" says the krunker.io account is a DEV (player_developer 1)",
          len(stub.a_sent) >= 2 and stub.a_sent[-1].get("player_developer") == 1, stub.a_sent)
    check("5. the player list lists the other players", all(n in pl for _, _, n in OTHERS), pl[:200])
    check("5. the krunker.io DEV's actions show: Kill and TPAll on our row, Kill / TP2 / the punish popup on each other row",
          nodes(text, "dplistdev") == 1 and nodes(text, "dplistkill") == len(OTHERS) + 1 and nodes(text, "dplisttp") == len(OTHERS) and
          nodes(text, "dplistpunish") == len(OTHERS),
          (nodes(text, "dplistdev"), nodes(text, "dplistkill"), nodes(text, "dplisttp"), nodes(text, "dplistpunish")))

    # 7. child windows (app_window.c): each window is its own process whose log lines carry its pid (the
    # "[account] active account backend: X (pid N)" line). The router's pid is the first such line's; the editor
    # the official game opened (a child) plays on krunker.io, and follows the router back to krunk.io.
    marks = [m.start() for m in re.finditer(r"\[app\] (?:new |)editor window", text)]
    check("7. the official game opened the editor as a child window", len(marks) >= 2, len(marks))
    lines = re.findall(r"\[account\] active account backend: (krunker\.io|krunk\.io) \(pid (\d+)\)", text)
    router_pid = lines[0][1] if lines else None
    router = [b for b, pid in lines if pid == router_pid]
    child = [b for b, pid in lines if pid != router_pid]
    check("7. the router's backend: krunk.io at start, krunker.io in the official game, krunk.io after leaving, krunker.io rejoined",
          router == ["krunk.io", "krunker.io", "krunk.io", "krunker.io"], router)
    check("7. the editor window starts on krunker.io (the router's session at spawn) and follows the router to krunk.io on leaving",
          child[:2] == ["krunker.io", "krunk.io"], child)
    check("7. ... and back to krunker.io with the router's rejoin (the same window, reloaded, never its own session)",
          child == ["krunker.io", "krunk.io", "krunker.io"], child)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("account_unify_e2e: no client_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("account_unify_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain_log = os.path.join(WORK, "brain.log")
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.players = OTHERS  # a game with players: the official player list's rows
    stub.account_data = [dict(KIO_DATA, player_role=2), dict(KIO_DATA, player_developer=1)]  # 3. a moderator, 5. a DEV
    stub.frame_handlers = {"bpdl": answer_bpdl, "cln": answer_cln}
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local", "--brain.id=acctunify-%d" % os.getpid(),
                                  "--accounts.email.required=false", "--smtp.host=", "--accounts.developers=" + DEV],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        st, reg = http(port, "/auth/register", {"username": DEV, "password": PW})
        check("the DEV account is registered", st == 200, "%s %s" % (st, str(reg)[:160]))
        stub.start()
        text = run_client(port, stub)
        checks(text, stub)
    finally:
        stub.stop()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
