#!/usr/bin/env python3
"""The Account Manager (client/src/ui/alt_manager_ui.c, ALT_MANAGER_WINDOW = 104) end to end through the
menu UI, against a local kn_brain (sqlite, temp dir) and services/common/tests/fake_smtp.py:

  register with the login popup (a plain sign-in stores nothing: no alt vault, a restart signs nobody in and
  the login card asks for the password) -> Add with a typed login and password lists the account and stores
  the password sealed: nothing is signed in, nothing is switched, no request reaches the account server (the
  Add runs against a --brain nothing listens on) -> Use fills the login card's Username and Password with the
  saved ones and signs in with them (the card's Login, run for the player): signed in, the session is stored ->
  Use while ANOTHER account is signed in on the same backend switches: the signed-in account is logged out first (the
  Logout button's path: its session revoked by the brain, so its socket presence leaves) and the row's account is signed
  in; back again -> Use on a row of the signed-in account itself (listed under its address) logs nobody out ->
  a restart resumes that account from its stored session -> Logout, then Use signs in again -> the brain then
  refuses the stored session: it is forgotten and nobody is signed in at start (no silent password sign-in); Use
  fills the card and signs in -> Remove ->
  an official krunker.io game (a guest joined through the stub's matchmaker): Add a krunk.io login, Use leaves the
  official game (the krunker.io game gives way to Box Office, as Find New Game does) and signs the Box Office account
  in, Remove -> krunker.io: Add stores its login with no request to krunker.io, Use fills the card with it, its Login
  signs in to krunker.io (the Security Challenge passes by itself, the stub's 2FA code is typed) and the card closes;
  signed in as that account, Use on another krunker.io row signs it out first and posts the other login once (the stub
  refuses it: nobody is signed in), Remove.

  The account split (client/src/ui/account_split.c, doc/v10/account-split.md): on a Box Office server a krunker.io row's
  Use opens no card and sends nothing (no official game to switch to), and a krunker.io login typed into the Box Office
  card (a fresh client home) is refused with no sign-in request. In an official krunker.io game a krunker.io row's Use
  fills the card, its Login signs in to krunker.io, and a Box Office login typed into the krunker.io card is refused with
  no sign-in request to krunker.io. A krunker.io sign-in kept from an official game supplies no Bearer to krunker.io
  while Box Office is the server (the menu's Live Streams read).

  The web build's Accounts window has no krunker.io chip or row (client/src/ui/alt_manager_ui.c,
  __EMSCRIPTEN__). That web absence is checked on the web build's own page, not here.

  python3 -I client/tests/account_manager_e2e.py BUILD_DIR      (repository root)
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, private XDG dirs) under xvfb-run's private Xvfb (the
krunker.io challenge webview needs a display; the game stays headless): its Security Challenge passes by itself on
the krunker.io stub's /captcha page (KRUNKNATIVE_KRUNKER_CAPTCHA_URL), a stub Turnstile widget answering the
test's token. A local server_bin for the menu's map, ports from kn_proto.free_ports(). 77 = skip (no kn_brain / openssl).
"""
import json
import os
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
sys.path.insert(0, os.path.join(ROOT, "services", "common", "tests"))
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not os.path.exists(BRAIN) or not shutil.which("openssl"):
    print("needs kn_brain (KRUNKNATIVE_SERVICES) and openssl: skip")
    sys.exit(77)
from fake_smtp import FakeSMTP, make_cert  # noqa: E402
from kn_proto import free_ports  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (the krunker.io stub: its own WORK dir is removed below)
shutil.rmtree(kae.WORK, ignore_errors=True)

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-account-manager-", dir=CACHE if os.path.isdir(CACHE) else None)
SMTP_USER, SMTP_PASS = "mailer@caan.test", "fake-am-pass-%d" % os.getpid()
STAMP = os.getpid() % 100000
NAME, ADDR = "am%d" % STAMP, "am%d@example.org" % STAMP
NAME2, ADDR2 = "am%db" % STAMP, "am%db@example.org" % STAMP
PASS = "pass_word1"  # a fixture password, typed by the scripts only: it must never reach the disk
# clicksel clicks the LAST row a selector matches (input_script.c): the Use and Remove clicks below reach the second
# account, listed last. Both accounts share PASS, so the Username field is what tells which row was used.
WINDOW = 104         # ALT_MANAGER_WINDOW = KN_WINDOW_BASE + 4
DEAD_BRAIN = "http://127.0.0.1:9"  # nothing listens here: an Add must not need the account server
VAULT = os.path.join(WORK, "cfg", "krunknative", "alt", "vault.bin")
# a fresh client home for the split refusals: no saved login name, so the login card opens in its Email field
SPLIT_HOME = os.path.join(WORK, "split")
results = []
procs = []
KIO_ENV = {}  # the krunker.io stub's endpoints for every client run (set in main)


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run_client(name, script, brain_url, game_port, timeout=150, home=WORK):
    """One headless client_bin run of `script` (its config and state under `home`); returns its log text."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(home, "cfg"),
                "XDG_STATE_HOME": os.path.join(home, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS})  # the full asset tree (a worktree has no models / fonts)
    env.update(KIO_ENV)
    with open(log, "w") as out:
        # a private Xvfb for the challenge webview (xvfb-run -a supplies DISPLAY; the game itself stays headless)
        p = subprocess.Popen(["xvfb-run", "-a", os.path.join(BUILD, "client_bin"), "--mode", "online", "--brain", brain_url,
                              "--finder", "off", "--connect", "127.0.0.1:%d" % game_port], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read()


def text_of(log, label):
    """The text the `text @label` line logged (the first line with that label)."""
    for line in log.splitlines():
        if f"[input_script] text " in line and f" @{label} " in line:
            return line
    return ""


def match_of(log, label):
    """The match count of the `nodes @label` line, -1 when it did not log."""
    for line in log.splitlines():
        if "[input_script] nodes " in line and f" @{label} " in line and "match=" in line:
            return int(line.rsplit("match=", 1)[1].split()[0])
    return -1


def secret_on_disk(needle):
    """Files under the client's config and state dirs (never the input scripts or logs) holding needle."""
    hits = []
    for base in (os.path.join(WORK, "cfg"), os.path.join(WORK, "state"), SPLIT_HOME):
        for dirpath, _, files in os.walk(base):
            for fn in files:
                p = os.path.join(dirpath, fn)
                with open(p, "rb") as f:
                    if needle.encode() in f.read():
                        hits.append(p)
    return hits


def secret_in_logs(needle):
    """The client's run logs (not the brain's) holding needle."""
    return [f for f in os.listdir(WORK) if f.endswith(".log") and f != "brain.log"
            and needle in open(os.path.join(WORK, f), errors="replace").read()]


def sessions(db):
    """The brain's session rows: a sign-in adds one, a logout or a refused token removes it."""
    con = sqlite3.connect(db)
    try:
        return con.execute("SELECT COUNT(*) FROM sessions").fetchone()[0]
    finally:
        con.close()


def sessions_of(db, name):
    """The brain's session rows of one account: a logout revokes them (the socket's presence left with them)."""
    con = sqlite3.connect(db)
    try:
        return con.execute("SELECT COUNT(*) FROM sessions WHERE account_id = (SELECT id FROM accounts WHERE username = ?)",
                           (name,)).fetchone()[0]
    finally:
        con.close()


def forget_brain_sessions(db, name):
    con = sqlite3.connect(db)
    with con:
        con.execute("DELETE FROM sessions WHERE account_id = (SELECT id FROM accounts WHERE username = ?)", (name,))
    con.close()


def auth_requests(stub):
    """The krunker.io stub's auth requests so far (a sign-in or a token refresh)."""
    return len([r for r in stub.requests if r[1].startswith("/frvr/v1/auth")])


# the popup's login card by username (a guest keeps no saved name: the popup opens on Email)
OPEN = "0 menu\n500 clickid knV10LoginBtn\n"
BY_NAME = "800 clickid knV10LoginMode\n"


def logins_as(log, name):
    return log.count(f"[account] logged in as {name} ")


def register(brain_url, game_port, name, addr, run):
    log = run_client(run, OPEN + f"""800 clickid knV10AuthSwitch
1200 clickid accEmail
1300 type {addr}
1400 down TAB
1410 up TAB
1500 type {name}
1600 down TAB
1610 up TAB
1700 type {PASS}
1800 clickid knV10LoginSubmit
5000 quit
""", brain_url, game_port)
    check(f"register {name} through the login popup", logins_as(log, name) >= 1)
    return log


def plain_restart(brain_url, game_port):
    """Remember off: a plain sign-in (the login popup, not listed) stores nothing, so a restart signs nobody in
    and the login card asks for the password."""
    check("a plain sign-in stores no vault", not os.path.exists(VAULT))
    log = run_client("plain_restart", """0 menu
500 clickid knV10LoginBtn
1000 nodes @pass [id="accPass"]
1500 quit
""", brain_url, game_port)
    check("a restart after a plain sign-in signs nobody in", logins_as(log, NAME) + logins_as(log, NAME2) == 0,
          f"logins {logins_as(log, NAME)} {logins_as(log, NAME2)}")
    check("and the login card asks for the password", match_of(log, "pass") == 1, str(match_of(log, "pass")))


def add_accounts(db, game_port):
    """Add stores the login and the password and lists the account. It signs nobody in, switches nothing, and needs no
    account server: the run's --brain is one nothing listens on, and both accounts are still listed."""
    sessions_before = sessions(db)
    log = run_client("add", f"""0 menu
500 call showWindow {WINDOW}
1000 clicksel [onclick*="knAltAdd"]
1500 text @empty [id="knAltMsg"]
2000 clickid knAltUser
2100 type {NAME}
2200 clickid knAltPass
2300 type {PASS}
2400 clicksel [onclick*="knAltAdd"]
5000 text @added1 [id="knAltMsg"]
5500 clickid knAltUser
5600 type {NAME2}
5700 clickid knAltPass
5800 type {PASS}
5900 clicksel [onclick*="knAltAdd"]
8500 text @added2 [id="knAltMsg"]
8600 nodes @rows .knAltRow
8700 nodes @active .knAltActive
9000 quit
""", DEAD_BRAIN, game_port)
    check("an empty Add asks for the login and the password", "Enter the login and the password." in text_of(log, "empty"),
          text_of(log, "empty")[-120:])
    check("Add lists the account (no account server needed)", f"Added {NAME}." in text_of(log, "added1"), text_of(log, "added1")[-120:])
    check("a second Add lists a second account", f"Added {NAME2}." in text_of(log, "added2"), text_of(log, "added2")[-120:])
    check("Add signs nobody in and sends no sign-in request", logins_as(log, NAME) + logins_as(log, NAME2) == 0
          and "[account] logged in as" not in log and "Signed in as" not in text_of(log, "added1") + text_of(log, "added2")
          and sessions(db) == sessions_before, f"sessions {sessions_before} -> {sessions(db)}")
    check("both accounts are listed", match_of(log, "rows") == 2, str(match_of(log, "rows")))
    check("neither listed account is active after Add", match_of(log, "active") == 0, str(match_of(log, "active")))
    check("the Add stored an alt vault", os.path.exists(VAULT))
    check("the password is on no client file after Add", not secret_on_disk(PASS), repr(secret_on_disk(PASS)))


def use_fills(brain_url, game_port, db):
    """Use on a listed account opens the login card with its Username and its saved Password in the fields, and the
    card signs in with them (its Login, run for the player): the account is signed in and its session is stored."""
    sessions_before = sessions(db)
    log = run_client("use", f"""0 menu
3000 call showWindow {WINDOW}
3400 clicksel [onclick*="knAltUse"]
3450 nodes @modal [id="knV10Login"]
6500 quit
""", brain_url, game_port)
    # the card closes when its sign-in completes (a few hundred ms), so it is sampled right after the click; the
    # card's fields are replaced once its sign-in starts, so the saved login is checked by the sign-in it made
    check("Use opens the login card", match_of(log, "modal") == 1, str(match_of(log, "modal")))
    check("Use signs the listed account in with the saved login (no press)", logins_as(log, NAME2) == 1, f"logins {logins_as(log, NAME2)}")
    check("and stores its session under the listed account", sessions(db) > sessions_before, f"sessions {sessions_before} -> {sessions(db)}")


REFUSALS = ("Log out to sign in", "Leave the official game", "already signed in")


def row_text(log, label):
    """The text of the `text @label` line's match (after `text=`), "" when it logged nothing."""
    line = text_of(log, label)
    return line.rsplit("text=", 1)[1] if "text=" in line else ""


def use_switches(brain_url, game_port, db, run, signed, other):
    """Signed in as `signed` (its session restored at start), Use on the other listed row (`other`) switches accounts on
    this backend: the signed-in account is logged out first, the Logout button's way (the brain revokes its session: its
    rows go, its socket presence leaves), and the row's account is signed in with its saved login (no press). The card
    closes on the sign-in; the row's account is the active row. (The active row's Use is dimmed and has no click, so
    clicksel reaches the other row's Use.)"""
    before_old = sessions_of(db, signed)
    log = run_client(run, f"""0 menu
3000 call showWindow {WINDOW}
3400 clicksel [onclick*="knAltUse"]
3450 nodes @modal [id="knV10Login"]
6000 nodes @modalgone [id="knV10Login"]
6100 call showWindow {WINDOW}
6600 text @act .knAltActive
7000 quit
""", brain_url, game_port)
    check(f"{run}: the account is signed in at start (the precondition)", logins_as(log, signed) == 1, f"logins {logins_as(log, signed)}")
    check(f"{run}: Use while another account is signed in opens the login card", match_of(log, "modal") == 1, str(match_of(log, "modal")))
    out, inn = log.find("[account] logged out"), log.find(f"[account] logged in as {other} ")
    check(f"{run}: it logs the signed-in account out first (the Logout path)", 0 <= out < inn, f"logged out at {out}, in at {inn}")
    check(f"{run}: and signs the row's account in with its saved login (no press)", logins_as(log, other) == 1, f"logins {logins_as(log, other)}")
    check(f"{run}: the brain revoked the old account's session (its socket presence left) and holds the new one",
          sessions_of(db, signed) == before_old - 1 and sessions_of(db, other) >= 1,
          f"sessions {signed} {before_old} -> {sessions_of(db, signed)}, {other} {sessions_of(db, other)}")
    check(f"{run}: the card closes on the sign-in", match_of(log, "modalgone") == 0, str(match_of(log, "modalgone")))
    act = row_text(log, "act")
    check(f"{run}: the row's account is the active row", act.startswith(other + " ") or act.startswith(other + "krunk"), repr(act[:80]))


def use_same_account(brain_url, game_port, db):
    """Signed in as NAME2: Use on a row of that same account, listed under its address (the brain pushed the address on
    file), logs nobody out: the card signs in as before and closes, no switch. The address's row is removed again."""
    before = sessions(db)
    log = run_client("same_login", f"""0 menu
4500 call showWindow {WINDOW}
5000 clickid knAltUser
5100 type {ADDR2}
5200 clickid knAltPass
5300 type {PASS}
5400 clicksel [onclick*="knAltAdd"]
5900 text @aadd [id="knAltMsg"]
6200 clicksel [onclick*="knAltUse"]
9400 nodes @aclosed [id="knV10Login"]
9800 call showWindow {WINDOW}
10300 clicksel [onclick*="knAltRemove"]
10800 text @arem [id="knAltMsg"]
11200 text @act .knAltActive
11500 quit
""", brain_url, game_port)
    check("the address's row is listed", f"Added {ADDR2}." in text_of(log, "aadd"), text_of(log, "aadd")[-120:])
    check("Use on the signed-in account's own row (its address) logs nobody out", "[account] logged out" not in log)
    check("and signs it in as before (the card closes, no switch)", logins_as(log, NAME2) == 1 and match_of(log, "aclosed") == 0,
          f"logins {logins_as(log, NAME2)} card {match_of(log, 'aclosed')}")
    check("the Login reached the brain (a session more)", sessions(db) >= before + 1, f"sessions {before} -> {sessions(db)}")
    check("the signed-in account stays the active row", NAME2 in row_text(log, "act"), row_text(log, "act")[:80])
    check("and the address's row is removed again", f"Removed {ADDR2}." in text_of(log, "arem"), text_of(log, "arem")[-120:])


def official_use(brain_url, game_port, stub, db):
    """An official krunker.io game (a guest joined through the stub's matchmaker): the Account Manager lists a krunk.io
    login. Its Use switches the client to that row's backend: the official game gives way to Box Office (left, as Find
    New Game leaves it), the window says why, and the Box Office account is signed in with its saved login (the card
    opens and closes on the sign-in). Remove signs it out again and forgets the row."""
    joins = len(stub.ws_paths)
    before = sessions(db)
    log = run_client("official_use", f"""0 menu
1500 call krunkerJoin "{kae.GAME_PICK}"
9000 call showWindow {WINDOW}
9500 clickid knAltUser
9600 type {NAME}
9700 clickid knAltPass
9800 type {PASS}
9900 clicksel [onclick*="knAltAdd"]
10500 text @oadd [id="knAltMsg"]
11000 clicksel [onclick*="knAltUse"]
11050 nodes @omodal [id="knV10Login"]
13500 nodes @ogone [id="knV10Login"]
13600 call showWindow {WINDOW}
14100 text @omsg [id="knAltMsg"]
14200 text @oact .knAltActive
14500 clicksel [onclick*="knAltRemove"]
15000 text @oremoved [id="knAltMsg"]
15500 quit
""", brain_url, game_port)
    check("the client joined an official krunker.io game (the stub's game socket)", len(stub.ws_paths) > joins, str(len(stub.ws_paths) - joins))
    check("in the official game, Add lists the krunk.io login", f"Added {NAME}." in text_of(log, "oadd"), text_of(log, "oadd")[-120:])
    left, inn = log.find("[krunker] leaving the official game"), log.find(f"[account] logged in as {NAME} ")
    check("in the official game, Use of a Box Office row leaves the official game first (it gives way to Box Office)",
          0 <= left < inn, f"left at {left}, signed in at {inn}")
    check("and signs the Box Office account in with its saved login (the card opens, then closes)",
          logins_as(log, NAME) == 1 and match_of(log, "omodal") == 1 and match_of(log, "ogone") == 0 and sessions_of(db, NAME) >= 1,
          f"logins {logins_as(log, NAME)} card {match_of(log, 'omodal')} -> {match_of(log, 'ogone')} sessions of {NAME} {sessions_of(db, NAME)}")
    check("the window says why the game was left", "Box Office accounts are used on a Box Office server." in text_of(log, "omsg"),
          text_of(log, "omsg")[-120:])
    check("the Box Office account is the active row", NAME in row_text(log, "oact"), row_text(log, "oact")[:80])
    check("and the row is removed again (signed out)", f"Removed {NAME}." in text_of(log, "oremoved") and "[account] logged out" in log,
          text_of(log, "oremoved")[-120:])


def kio_login(brain_url, game_port, stub):
    """The krunker.io row's card in an official krunker.io game (the split: only there does its Login sign in to krunker.io):
    Login signs in to krunker.io (the Security Challenge passes by itself on the stub's captcha page; then the stub's 2FA
    code is typed), and the card closes."""
    posts = len(stub.find("POST", "/api/auth/login/username"))
    script = "\n".join([
        "0 menu",
        f'1500 call krunkerJoin "{kae.GAME_PICK}"',
        f"9000 call showWindow {WINDOW}",
        '9500 clicksel [onclick*="knAltUse"]',
        '10500 nodes @kmodal [id="knV10Login"]',
        f'10550 nodes @kuser [id="accName"][value="{kae.LOGIN}"]',
        f'10560 nodes @kpass [id="accPass"][value="{kae.PASSWORD}"]',
        '14400 vis @code knV10Code knV10Code0 knV10CodeSubmit',
    ] + [line for i, d in enumerate(kae.CODE) for line in (f"{14600 + i * 150} clickid knV10Code{i}",
                                                           f"{14660 + i * 150} type {d}")] + [
        '19500 nodes @modalgone [id="knV10Login"]',
        "20000 quit",
    ]) + "\n"
    log = run_client("kio_login", script, brain_url, game_port)
    check("in an official game, Use fills the krunker.io card with its login", match_of(log, "kmodal") == 1
          and match_of(log, "kuser") == 1 and match_of(log, "kpass") == 1,
          f"card {match_of(log, 'kmodal')} user {match_of(log, 'kuser')} pass {match_of(log, 'kpass')}")
    check("and Use posts the krunker.io login once (no press)", len(stub.find("POST", "/api/auth/login/username")) - posts == 1,
          f"posts {posts} -> {len(stub.find('POST', '/api/auth/login/username'))}")
    check("krunker.io Login signs the row's account in to krunker.io", f"signed in to krunker.io as {kae.LOGIN}" in log)
    check("the card closes after the krunker.io sign-in", match_of(log, "modalgone") == 0, str(match_of(log, "modalgone")))


def restart_and_logout(brain_url, game_port):
    """A restart resumes the account from its stored session (no prompt); Logout ends it, and Use then fills the card
    again with no sign-in."""
    log = run_client("restart", f"""0 menu
2000 call logoutAcc
3000 call knV10LoginClose
3500 call showWindow {WINDOW}
4000 clicksel [onclick*="knAltUse"]
5000 quit
""", brain_url, game_port)
    # the restore's sign-in, then Use's (after the Logout): two sign-ins in this run
    check("a restart resumes the account from its stored session", logins_as(log, NAME2) >= 1, f"logins {logins_as(log, NAME2)}")
    check("and asks for no password", "no longer valid" not in log and "Enter the password" not in log)
    check("Logout signs the account out", "[account] logged out" in log)
    check("after Logout, Use signs the account in again (no press, no password prompt)", logins_as(log, NAME2) == 2,
          f"logins {logins_as(log, NAME2)}")


def refused_token(brain_url, game_port, db):
    """The brain refuses the stored session: it is forgotten, nobody is signed in at start (no silent sign-in with
    the stored password), and the row still fills the card."""
    forget_brain_sessions(db, NAME2)
    log = run_client("refused", f"""0 menu
7000 call showWindow {WINDOW}
7500 clicksel [onclick*="knAltUse"]
8500 quit
""", brain_url, game_port)
    check("a refused stored session signs nobody in at start: the only sign-in is Use's", logins_as(log, NAME2) == 1, f"logins {logins_as(log, NAME2)}")


def remove_accounts(brain_url, game_port):
    log = run_client("remove", f"""0 menu
2000 call showWindow {WINDOW}
2500 clicksel [onclick*="knAltRemove"]
3000 text @removed [id="knAltMsg"]
3500 clicksel [onclick*="knAltRemove"]
4000 text @removed2 [id="knAltMsg"]
4500 nodes @rows .knAltRow
5000 quit
""", brain_url, game_port)
    check("Remove forgets the second account (the last row)", f"Removed {NAME2}." in text_of(log, "removed"), text_of(log, "removed")[-120:])
    check("Remove forgets the first account", f"Removed {NAME}." in text_of(log, "removed2"), text_of(log, "removed2")[-120:])
    check("no rows after Remove", match_of(log, "rows") == 0, str(match_of(log, "rows")))
    restart = run_client("after_remove", f"""0 menu
2000 call showWindow {WINDOW}
3000 nodes @rows .knAltRow
3500 quit
""", brain_url, game_port)
    check("a restart lists nothing and signs nobody in", logins_as(restart, NAME2) == 0 and match_of(restart, "rows") == 0,
          f"rows {match_of(restart, 'rows')}")


OTHER = "kioother"  # a second krunker.io login (the stub accepts only kae.LOGIN: any other is refused)


def kio_other_login(brain_url, game_port, stub):
    """krunker.io signed in as the row's account, in an official game: Use on another krunker.io row (OTHER, listed here)
    switches: the signed-in account is signed out first (the Logout button's path) and OTHER's login is posted once to
    krunker.io (no press). The stub refuses it, so nobody is signed in and the card says so. OTHER is removed again."""
    posts = len(stub.find("POST", "/api/auth/login/username"))
    log = run_client("kio_other", f"""0 menu
1500 call krunkerJoin "{kae.GAME_PICK}"
9000 call showWindow {WINDOW}
9500 call knAltBackend 1
9700 clickid knAltUser
9800 type {OTHER}
10000 clickid knAltPass
10100 type {kae.PASSWORD}
10200 clicksel [onclick*="knAltAdd"]
11000 text @oadd [id="knAltMsg"]
11500 clicksel [onclick*="knAltUse"]
11550 nodes @omodal [id="knV10Login"]
30000 text @omsg [id="accResp"]
30500 call knV10LoginClose
31000 call showWindow {WINDOW}
31500 clicksel [onclick*="knAltRemove"]
32000 text @oremoved [id="knAltMsg"]
32500 quit
""", brain_url, game_port)
    check("krunker.io: the other row's Use opens the login card", match_of(log, "omodal") == 1, str(match_of(log, "omodal")))
    out, post = log.find("[account] signed out of krunker.io"), log.find("auth/login/username")
    check("krunker.io: Use signs the signed-in account out first (the Logout path)", 0 <= out < post or (out >= 0 and post < 0),
          f"signed out at {out}, post at {post}")
    check("krunker.io: and posts the other login once (no press)", len(stub.find("POST", "/api/auth/login/username")) - posts == 1,
          f"posts {posts} -> {len(stub.find('POST', '/api/auth/login/username'))}")
    check("krunker.io: the refused login signs nobody in, and the card says why", "[kio-account] signed in to krunker.io as" not in log
          and "signed in as" not in text_of(log, "omsg").lower() and "invalid username or password" in text_of(log, "omsg").lower(),
          text_of(log, "omsg")[-120:])
    check("and the other login is removed again", f"Removed {OTHER}." in text_of(log, "oremoved"), text_of(log, "oremoved")[-120:])


def krunker_row(brain_url, game_port, stub):
    """A krunker.io row: Add stores its login with no request to krunker.io, Use fills the card with it (no request),
    Remove forgets it. The card's own sign-in (krunker.io's code step included) is not run here."""
    before = auth_requests(stub)
    log = run_client("kio_add", f"""0 menu
500 call showWindow {WINDOW}
1000 call knAltBackend 1
1200 clickid knAltUser
1300 type {kae.LOGIN}
1400 clickid knAltPass
1500 type {kae.PASSWORD}
1600 clicksel [onclick*="knAltAdd"]
2500 text @kadd [id="knAltMsg"]
2600 nodes @krows .knAltRow
2700 nodes @ktag .knAltTagKio
3000 quit
""", brain_url, game_port)
    check("krunker.io Add says it is added", f"Added {kae.LOGIN}." in text_of(log, "kadd"), text_of(log, "kadd")[-120:])
    check("krunker.io Add signs nobody in and sends no request to krunker.io",
          "signed in to krunker.io" not in log and auth_requests(stub) == before, f"auth requests {before} -> {auth_requests(stub)}")
    check("the krunker.io row is listed with its krunker.io tag", match_of(log, "krows") == 1 and match_of(log, "ktag") == 1,
          f"rows {match_of(log, 'krows')} tags {match_of(log, 'ktag')}")

    before = auth_requests(stub)
    log = run_client("kio_use_box", f"""0 menu
500 call showWindow {WINDOW}
1000 clicksel [onclick*="knAltUse"]
1500 text @kref [id="knAltMsg"]
2000 nodes @kmodal [id="knV10Login"]
2500 quit
""", brain_url, game_port)
    check("Use on a krunker.io row is refused on a Box Office server (no card)", "krunker.io accounts are used in an official krunker.io game."
          in text_of(log, "kref") and match_of(log, "kmodal") == 0, f"{text_of(log, 'kref')[-120:]} card {match_of(log, 'kmodal')}")
    check("the refused Use signs nobody in and sends no request to krunker.io",
          "signed in to krunker.io" not in log and auth_requests(stub) == before, f"auth requests {before} -> {auth_requests(stub)}")

    kio_login(brain_url, game_port, stub)
    kio_idle(brain_url, game_port, stub)
    kio_other_login(brain_url, game_port, stub)  # (signs the krunker.io account out: after kio_idle, which needs it signed in)
    log = run_client("kio_remove", f"""0 menu
500 call showWindow {WINDOW}
1000 clicksel [onclick*="knAltRemove"]
1500 text @kremoved [id="knAltMsg"]
2000 nodes @krows .knAltRow
2500 quit
""", brain_url, game_port)
    check("Remove forgets the krunker.io row", f"Removed {kae.LOGIN}." in text_of(log, "kremoved") and match_of(log, "krows") == 0,
          f"{text_of(log, 'kremoved')[-120:]} rows {match_of(log, 'krows')}")


def kio_idle(brain_url, game_port, stub):
    """A Box Office server with the krunker.io account still signed in (its session kept from the official game): the
    menu's Live Streams read sends no krunker.io Bearer, and the krunker.io row is not the active account here (the
    split: a krunker.io identity is used only in an official krunker.io game)."""
    start = len(stub.requests)
    log = run_client("kio_idle", f"""0 menu
500 call showWindow {WINDOW}
1000 nodes @kidrows .knAltRow
1500 nodes @kact .knAltActive
9000 quit
""", brain_url, game_port)
    bearers = [r[1] for r in stub.requests[start:] if r[3].get("authorization")]
    check("on a Box Office server the signed-in krunker.io account sends no Bearer to krunker.io", not bearers, repr(bearers[:3]))
    check("the krunker.io row is listed (the window was up)", match_of(log, "kidrows") == 1, str(match_of(log, "kidrows")))
    check("and the krunker.io row is not the active account on a Box Office server", match_of(log, "kact") == 0, str(match_of(log, "kact")))


def split_box_office(brain_url, game_port, stub):
    """A krunker.io row listed on a Box Office server, in a fresh client home (no saved login name: the card's Email field
    comes first). The krunker.io login and its password typed into the Box Office card are refused (account_split.h): no
    sign-in goes to kn_brain or to krunker.io, nobody is signed in, and the row is removed again."""
    before = auth_requests(stub)
    log = run_client("split_box", f"""0 menu
500 call showWindow {WINDOW}
1000 call knAltBackend 1
1200 clickid knAltUser
1300 type {kae.LOGIN}
1400 clickid knAltPass
1500 type {kae.PASSWORD}
1600 clicksel [onclick*="knAltAdd"]
2500 text @sadd [id="knAltMsg"]
3000 call loginOrRegister
3500 clickid accEmail
3600 type {kae.LOGIN}
4000 clickid knV10LoginSubmit
4500 clickid accPass
4600 type {kae.PASSWORD}
5000 clickid knV10LoginSubmit
6000 text @sref [id="accResp"]
6500 call knV10LoginClose
7000 call showWindow {WINDOW}
7500 clicksel [onclick*="knAltRemove"]
8000 text @sremoved [id="knAltMsg"]
8500 quit
""", brain_url, game_port, home=SPLIT_HOME)
    check("the Box Office card refuses a krunker.io login", "krunker.io account" in text_of(log, "sref"), text_of(log, "sref")[-120:])
    check("and sends it to no kn_brain login", "[account] logged in as" not in log and "Invalid login" not in text_of(log, "sref"))
    check("and no sign-in request reaches krunker.io", auth_requests(stub) == before, f"auth requests {before} -> {auth_requests(stub)}")
    check("the krunker.io row is removed again", f"Removed {kae.LOGIN}." in text_of(log, "sremoved"), text_of(log, "sremoved")[-120:])


def split_official(brain_url, game_port, stub):
    """An official krunker.io game: a Box Office login (a listed krunk.io login) typed into the krunker.io card's Email
    field with its password is refused (account_split.h): no sign-in request goes to krunker.io, and the row is removed."""
    before = auth_requests(stub)
    log = run_client("split_kio", f"""0 menu
1500 call krunkerJoin "{kae.GAME_PICK}"
9000 call showWindow {WINDOW}
9500 clickid knAltUser
9600 type {NAME}
9700 clickid knAltPass
9800 type {PASS}
9900 clicksel [onclick*="knAltAdd"]
10500 text @sadd [id="knAltMsg"]
11000 call loginOrRegister
11500 clickid accEmail
11600 type {NAME}
12000 clickid knV10LoginSubmit
12500 clickid knV10UsePass
13000 clickid accPass
13100 type {PASS}
13500 clickid knV10LoginSubmit
14500 text @sref [id="accResp"]
15000 call knV10LoginClose
15500 call showWindow {WINDOW}
16000 clicksel [onclick*="knAltRemove"]
16500 text @sremoved [id="knAltMsg"]
17000 quit
""", brain_url, game_port)
    check("the krunker.io card refuses a Box Office login", "Box Office account" in text_of(log, "sref"), text_of(log, "sref")[-120:])
    check("and no sign-in request reaches krunker.io", auth_requests(stub) == before, f"auth requests {before} -> {auth_requests(stub)}")
    check("and nothing is signed in to krunker.io", "signed in to krunker.io" not in log)
    check("the Box Office row is removed again", f"Removed {NAME}." in text_of(log, "sremoved"), text_of(log, "sremoved")[-120:])


def main():
    # `timeout` sends SIGTERM: run the finally below, kill what we started
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, game_port, http_port, ws_port = free_ports(4)
    stub = kae.Stub(http_port, ws_port)
    cert, key = make_cert(WORK)
    smtp = FakeSMTP("starttls", cert, key, users={SMTP_USER: SMTP_PASS}).start()
    db = os.path.join(WORK, "coordinator.db")
    brain_log = open(os.path.join(WORK, "brain.log"), "w")
    brain_url = "http://127.0.0.1:%d" % brain_port
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--database.driver=sqlite",
                                       "--database.path=" + db, "--migrate", "--smtp.host=127.0.0.1", "--smtp.port=%d" % smtp.port,
                                       "--smtp.tls=starttls", "--smtp.user=" + SMTP_USER, "--smtp.ca_file=" + cert, "--smtp.retries=-1",
                                       "--accounts.email.resend_cooldown_s=1", "--brain.public_url=" + brain_url],
                                      cwd=ROOT, env=dict(os.environ, KN_SMTP_PASSWORD=SMTP_PASS), stdout=brain_log,
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True))
        procs.append(subprocess.Popen([os.path.join(BUILD, "server_bin"), "--headless", "--cfg", "none", "--port", str(game_port)],
                                      cwd=ROOT, stdout=open(os.path.join(WORK, "server.log"), "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        stub.start()
        KIO_ENV.update({"KRUNKNATIVE_KRUNKER_API": "http://127.0.0.1:%d/api" % http_port,
                        "KRUNKNATIVE_KRUNKER_AUTH": "http://127.0.0.1:%d/frvr/v1/auth" % http_port,
                        "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d/mm" % http_port,
                        # the footer counts (social / editor /player-count) go to a closed port: a test never reaches krunker.io
                        "KRUNKNATIVE_KRUNKER_SOCIAL": "http://127.0.0.1:9", "KRUNKNATIVE_KRUNKER_EDITOR": "http://127.0.0.1:9",
                        "KRUNKNATIVE_ASSET_FETCH": "off",  # (the official game's map is in the asset tree)
                        "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": "http://127.0.0.1:%d%s" % (http_port, kae.CAPTCHA_PATH)})
        for _ in range(100):
            try:
                with urllib.request.urlopen(brain_url + "/mm/status", timeout=1) as r:
                    if b'"brain"' in r.read():
                        break
            except OSError:
                time.sleep(0.1)
        register(brain_url, game_port, NAME, ADDR, "register")
        register(brain_url, game_port, NAME2, ADDR2, "register2")
        plain_restart(brain_url, game_port)
        add_accounts(db, game_port)
        use_fills(brain_url, game_port, db)
        use_switches(brain_url, game_port, db, "switch", NAME2, NAME)
        use_switches(brain_url, game_port, db, "switch_back", NAME, NAME2)
        use_same_account(brain_url, game_port, db)
        restart_and_logout(brain_url, game_port)
        refused_token(brain_url, game_port, db)
        remove_accounts(brain_url, game_port)
        official_use(brain_url, game_port, stub, db)
        krunker_row(brain_url, game_port, stub)
        split_box_office(brain_url, game_port, stub)
        split_official(brain_url, game_port, stub)
        check("the password is on no client file", not secret_on_disk(PASS) and not secret_on_disk(kae.PASSWORD),
              repr(secret_on_disk(PASS) + secret_on_disk(kae.PASSWORD)))
        leaked = [t for t in (PASS, kae.PASSWORD, kae.REFRESH1, kae.REFRESH2) if secret_in_logs(t)]
        check("no password or token in a client log", not leaked, repr(leaked))
    except Exception as e:  # noqa: BLE001
        check("exception", False, repr(e))
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
        smtp.stop()
        stub.stop()
        brain_log.close()
    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
