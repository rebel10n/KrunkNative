#!/usr/bin/env python3
"""Krunker v10.0.2 rewards end to end on the v10 page (client/src/ui/v10_rewards.c, battlepass_ui.c;
services/brain/rewards.c, db_rewards.c, season.c; migration 0116_v10_rewards).

  python3 -I server/tests/v10_rewards_e2e.py BUILD_DIR      (run from the repository root)

Creates its own PostgreSQL database (sudo -n -u postgres createdb; KN_TEST_DB to reuse one), starts
one kn_brain and one server_bin (ports from kn_proto.free_ports(), cheats "all"), and drives a
headless client_bin on ui/v10 (KRUNKNATIVE_UI_CMDS: clicks, typed text, page calls, screenshots):
  1. as a guest: Get Signup Rewards -> the "Join Krunker today!" popup -> REGISTER -> the login
     popup's Register card (v10_login.c) -> registered: kn_brain grants the 14 cosmetics + 24 h of premium once and the client
     shows "Welcome to the Family!" ("prr")
  2. the next start (the saved session): logged in again, nothing granted twice
  3. a challenge completed in a game: the account plays Burg FFA on server_bin (kn_proto) and kills
     twice; the round ends -> the challenge's KR and skin are credited; the client's Challenges
     window (CHALLENGES in the sidebar) shows it completed
  4. the Battle Pass: a reached card claimed from the window (Claim) -> the skin in the inventory;
     the VIP tier gifted to another player (Gift VIP Tier) -> their tier
Screenshots and logs go to the work dir (printed on failure, kept with KN_TEST_KEEP=1).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import secrets
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
DB = os.environ.get("KN_TEST_DB", f"kn_v10rw_e2e_{os.getpid()}")
BRAIN_PORT, GAME_PORT = kn_proto.free_ports(2)
WORK = tempfile.mkdtemp(prefix="kn-v10-rewards-e2e-", dir=os.path.expanduser("~/.cache"))
CHAT_SEND = 8
SIGNUP_SKINS = [113, 121, 119, 283, 260, 832, 834, 866, 871, 3, 15, 26, 36, 46]
CHAL_ID, CHAL_KR, CHAL_SKIN = 900, 30, 6423
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def cfgdb():
    with open("config/config.json") as f:
        return json.load(f)["database"]


def psql(sql):
    d = cfgdb()
    env = dict(os.environ, PGPASSWORD=d.get("password", ""))
    out = subprocess.run(["psql", "-h", d.get("host", "127.0.0.1"), "-U", d.get("user", "krunknative"), "-d", DB,
                          "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql], env=env, capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(out.stderr)
    return out.stdout.strip()


def http(path, body=None, **q):
    url = f"http://127.0.0.1:{BRAIN_PORT}{path}" + ("?" + urllib.parse.urlencode(q) if q else "")
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def log(name):
    try:
        with open(os.path.join(WORK, name), errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.25)
    return None


class Client:
    """One headless client_bin on ui/v10 with a private config dir, driven through KRUNKNATIVE_UI_CMDS."""

    def __init__(self, name, xdg):
        self.name, self.log_name = name, f"client-{name}.log"
        self.cmds = os.path.join(WORK, f"cmds-{name}.txt")
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": xdg,
                    "KRUNKNATIVE_UI_DIR": "ui/v10", "KRUNKNATIVE_BRAIN_URL": f"http://127.0.0.1:{BRAIN_PORT}",
                    "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_NO_BROWSER": "1",
                    "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
        self.proc = subprocess.Popen([os.path.join(BUILD, "client_bin"), "--offline"], env=env,
                                     stdout=open(os.path.join(WORK, self.log_name), "w"), stderr=subprocess.STDOUT)
        procs.append(self.proc)

    def text(self):
        return log(self.log_name)

    def cmd(self, line, wait=0.8):
        with open(self.cmds, "a") as f:
            f.write(line + "\n")
        time.sleep(wait)

    def shot(self, tag):
        path = os.path.join(WORK, f"{self.name}-{tag}.png")
        self.cmd(f"shot {path}", 1.0)
        return path

    def quit(self):
        self.cmd("quit", 2.0)
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def storage(xdg, key):
    try:
        with open(os.path.join(xdg, "krunknative", key + ".json")) as f:
            return f.read().strip().strip('"')
    except OSError:
        return ""


def play(seek, token):
    c = kn_proto.Conn("127.0.0.1", int(seek["port"]))
    c.send(1, bytes([1, 3, 0]))
    c.join(client_key=seek["clientId"], session=token)
    init = c.read(until=kn_proto.INIT, timeout=10)
    uid = struct.unpack("<BBBiiHi", init[:struct.calcsize("<BBBiiHi")])[6] if init else -1
    c.read(timeout=1.0)
    return c, uid


def inventory(account_id):
    rows = psql(f"SELECT skin_id FROM inventory WHERE account_id = {account_id} AND count > 0 ORDER BY skin_id")
    return [int(x) for x in rows.split()] if rows else []


def run():
    subprocess.run([f"{BUILD}/services/common/kn_migrate", "--set", f"database.name={DB}"], check=True,
                   stdout=open(os.path.join(WORK, "migrate.log"), "w"), stderr=subprocess.STDOUT)
    procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--set", f"database.name={DB}", "--set", f"brain.port={BRAIN_PORT}",
                                   "--set", "brain.region=local", "--set", "accounts.login_throttle_s=0"],
                                  stdout=open(os.path.join(WORK, "brain.log"), "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    up = bool(wait_for(lambda: http("/mm/status")[0] == 200)) and procs[-1].poll() is None
    check("coordinator up", up, "" if up else log("brain.log")[-300:])
    if not up:
        return
    name, password = "rw" + secrets.token_hex(3), "rewards-pass-1"
    xdg = os.path.join(WORK, "cfg")
    os.makedirs(xdg, exist_ok=True)

    # 1. guest: Get Signup Rewards -> REGISTER -> register -> "Welcome to the Family!"
    c = Client("signup", xdg)
    time.sleep(14)  # page, map, social socket
    c.cmd("ui clearPops()")
    c.cmd("clickid signupRewardsButton", 1.5)
    c.shot("1-signup-rewards")
    check("Get Signup Rewards opens the signup rewards popup", "[rewards] signup rewards popup" in c.text(), c.text()[-300:])
    c.cmd("clickid knSignupRegister", 1.5)
    check("REGISTER opens the login popup on its Register card", "[rewards] register from the signup rewards" in c.text()
          and "loginOrRegister" in c.text())
    c.shot("2-register-form")
    c.cmd("focus accEmail")  # v10 registers by email (Email, Username, Password)
    c.cmd(f"text {name}@example.com")
    c.cmd("focus accName")
    c.cmd(f"text {name}")
    c.cmd("focus accPass")
    c.cmd(f"text {password}")
    c.cmd("clickid knV10LoginSubmit", 4.0)  # the login popup's Register (v10_login.c)
    got = wait_for(lambda: "[rewards] register rewards: 14 cosmetics + premium" in c.text(), 15)
    check("the new account gets the signup perks (prr: 14 cosmetics + premium)", got, c.text()[-400:])
    c.shot("3-welcome")
    c.quit()
    acc = psql(f"SELECT id FROM accounts WHERE username = '{name}'")
    acc_id = int(acc) if acc else 0
    check("account registered from the client", acc_id > 0)
    if not acc_id:
        return
    inv = inventory(acc_id)
    check("brain: the 14 cosmetics in the inventory", inv == sorted(SIGNUP_SKINS), str(inv))
    prem = int(psql(f"SELECT player_premium FROM players WHERE player_id = {acc_id}") or 0)
    check("brain: 24 h of premium (premiumT ms)", prem >= 86400000, str(prem))
    check("brain: one signup_rewards row", psql(f"SELECT count(*) FROM signup_rewards WHERE account_id = {acc_id}") == "1")

    # 2. the next start: logged in from the saved session, nothing granted twice
    token = storage(xdg, "krunker_token")
    check("the session token was saved", bool(token))
    c = Client("relogin", xdg)
    time.sleep(16)
    c.shot("4-relogin")
    c.quit()
    check("the next login grants nothing again (no prr)", "[rewards] register rewards" not in c.text() and inventory(acc_id) == sorted(SIGNUP_SKINS)
          and int(psql(f"SELECT player_premium FROM players WHERE player_id = {acc_id}") or 0) == prem)

    # 3. a challenge completed in a game on server_bin
    psql(f"INSERT INTO challenges (challenge_id, ctype, dat, rew, icon, name, sort) VALUES ({CHAL_ID}, 'map', "
         f"'{{\"ky\":\"k\",\"val\":2,\"mp\":0}}', '{{\"kr\":{CHAL_KR},\"sk\":[{CHAL_SKIN}]}}', 0, 'Get 2 kills on Burg (e2e)', 1)")
    cfg = os.path.join(WORK, "game.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Rewards E2E"\nport {GAME_PORT}\nnode_id RWE\nregion local\nmaxplayers 10\ncheats all\n'
                f"coordinator_url ws://127.0.0.1:{BRAIN_PORT}/mm/internal/ws\n"
                f"coordinator_http_url http://127.0.0.1:{BRAIN_PORT}\nmapcycle Burg\nmode ffa\njoin_timeout 4\n")
    with open(os.path.join(WORK, "none.json"), "w") as f:
        json.dump({"database": cfgdb()}, f)
    game = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                            stdout=open(os.path.join(WORK, "game.log"), "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True)
    procs.append(game)
    check("server linked to the coordinator", bool(wait_for(lambda: "welcomed" in log("game.log"))))
    victim, vtoken = "rv" + secrets.token_hex(3), secrets.token_hex(16)
    psql(f"SELECT kn_register('{victim}', 'x', '127.0.0.1', '', 0, 0, '{vtoken}', 3600, 'e2e')")
    victim_id = int(psql(f"SELECT id FROM accounts WHERE username = '{victim}'"))
    lobby = wait_for(lambda: ([g[0] for g in http("/mm/game-list")[1].get("games", []) if g[0].startswith("RWE:")] + [None])[0])
    check("public Burg lobby listed", bool(lobby))
    if not lobby:
        return
    funds0 = int(psql(f"SELECT player_funds FROM players WHERE player_id = {acc_id}"))
    conns = {}
    for who, tok in ((name, token), (victim, vtoken)):
        code, seek = http("/mm/seek-game", token=tok, game=lobby)
        conns[who] = play(seek, tok)
        conns[who][0].ent(class_index=0)
    for cc, _ in conns.values():
        cc.read(timeout=1.5)
    me = conns[name][0]
    for _ in range(2):
        me.send(CHAT_SEND, kn_proto.cstr(f"/kill {victim}", 288))
        me.read(timeout=3.2)
        conns[victim][0].ent(class_index=0)
        for cc, _ in conns.values():
            cc.read(timeout=0.3)
    game.stdin.write("round end\n")
    game.stdin.flush()
    done = wait_for(lambda: psql(f"SELECT pt FROM player_challenges WHERE account_id = {acc_id} AND challenge_id = {CHAL_ID}") == "1", 20)
    check("the challenge completed in the game (player_challenges pt 1)", done, log("game.log")[-300:] if not done else "")
    funds1 = int(psql(f"SELECT player_funds FROM players WHERE player_id = {acc_id}"))
    check(f"its {CHAL_KR} KR credited", funds1 - funds0 >= CHAL_KR, f"{funds0} -> {funds1}")
    check("its skin credited", CHAL_SKIN in inventory(acc_id))
    for cc, _ in conns.values():
        cc.close()

    # the Challenges window (sidebar CHALLENGES) and 4. the Battle Pass
    psql(f"UPDATE battlepass_progress SET bpp = 700 WHERE account_id = {acc_id}")  # level 3
    psql(f"INSERT INTO battlepass_progress (account_id, bp_id, bpp) SELECT {acc_id}, 2, 700 WHERE NOT EXISTS "
         f"(SELECT 1 FROM battlepass_progress WHERE account_id = {acc_id} AND bp_id = 2)")
    psql(f"UPDATE players SET player_funds = player_funds + 1500 WHERE player_id = {acc_id}")
    c = Client("account", xdg)
    time.sleep(16)
    c.shot("5-menu-account")
    c.cmd("clickid menuBtnChall", 2.0)
    c.shot("6-challenges")
    check("CHALLENGES opens the Challenges window with the completed challenge",
          "[challenges] window home" in c.text() and "completed" in c.text().split("[challenges] window home")[-1].split("\n")[0]
          and " 0 completed" not in c.text().split("[challenges] window home")[-1].split("\n")[0], c.text()[-300:])
    c.cmd("ui windows[32].openChallengeCat('map',0)", 1.5)
    c.shot("7-burg-challenges")
    check("the Burg category", "[challenges] window map" in c.text())
    c.cmd("ui closWind()")
    c.cmd("clickid menuBtnBattlepass", 2.5)
    c.shot("8-battlepass")
    check("the sidebar's Battle Pass item opens the pass", "Season 7 Battle Pass" in psql("SELECT name FROM battlepasses WHERE bp_id = 2")
          and "[ui_cmds] #menuBtnBattlepass rect" in c.text(), c.text()[-300:])
    c.cmd("ui windows[5].claimItem(20003)", 2.5)
    c.shot("9-claimed")
    claimed = psql(f"SELECT items_claimed FROM battlepass_progress WHERE account_id = {acc_id} AND bp_id = 2")
    check("a reached card claimed from the window", "20003" in claimed.split(",") and "[battlepass] claimed 1 item(s)" in c.text(), claimed)
    check("its skin in the inventory", 6424 in inventory(acc_id))
    c.cmd("ui clearPops()")
    c.cmd("ui windows[5].giftPop()", 1.5)
    c.shot("10-gift-form")
    c.cmd("focus giftPlayerName")
    c.cmd(f"text {victim}")
    c.cmd("ui knBpGiftSend()", 3.0)
    c.shot("11-gifted")
    vt = psql(f"SELECT tier FROM battlepass_progress WHERE account_id = {victim_id} AND bp_id = 2")
    check("Gift VIP Tier: the other player has the VIP tier", vt == "1" and f"Battlepass Gifted to {victim}!" in c.text(), vt)
    c.quit()


def main():
    created = False
    if not os.environ.get("KN_TEST_DB"):
        subprocess.run(["sudo", "-n", "-u", "postgres", "createdb", "-O", cfgdb().get("user", "krunknative"), DB], check=True)
        created = True
    try:
        run()
    finally:
        for p in procs:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in procs:
            try:
                p.wait(15)
            except subprocess.TimeoutExpired:
                p.kill()
        if created and not os.environ.get("KN_TEST_KEEP"):
            subprocess.run(["sudo", "-n", "-u", "postgres", "dropdb", "--if-exists", DB])
    ok = all(results) and results
    if ok and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs: {WORK}")
    print(f"{'ALL PASSED' if ok else 'FAILED'} ({sum(results)}/{len(results)})")
    sys.exit(0 if ok else 1)


main()
