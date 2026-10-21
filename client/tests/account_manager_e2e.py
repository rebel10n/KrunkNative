#!/usr/bin/env python3
"""The Account Manager (client/src/ui/alt_manager_ui.c, ALT_MANAGER_WINDOW = 104) end to end through the
menu UI, against a local kn_brain (sqlite, temp dir) and services/common/tests/fake_smtp.py:

  register with the login popup (a plain sign-in stores nothing: no alt vault, a restart signs nobody in
  and the login card asks for the password) -> the Account Manager's Add with a typed login and password
  (an empty Add says so; the account is listed, Added, and nobody is signed in; the password is on no
  disk) -> Use signs in from the stored token with no password; an Add of a listed account while another
  is signed in saves it without switching, and the Use after it signs in from the token that Add saved ->
  a restart resumes the Used account with no prompt -> the brain drops the account's sessions: the stored
  password signs it in at start, silently -> the brain also forgets the account: the row stays listed and
  says the saved login is no longer valid, its Sign in fills the form -> Remove -> krunker.io: Add stores
  without signing in, Use resumes, a refused token signs in with the stored password (2FA code typed),
  Remove signs out; a restart lists nothing.

  The krunker.io half (the krunker.io chip, its rows, Use / Remove of a krunker.io row) is native only: the web
  build plays krunk.io only and its Accounts window has no krunker.io chip or row (client/src/ui/alt_manager_ui.c,
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
WINDOW = 104         # ALT_MANAGER_WINDOW = KN_WINDOW_BASE + 4
VAULT = os.path.join(WORK, "cfg", "krunknative", "alt", "vault.bin")
results = []
procs = []
KIO_ENV = {}  # the krunker.io stub's endpoints for every client run (set in main)


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run_client(name, script, brain_url, game_port, timeout=150):
    """One headless client_bin run of `script`; returns its log text."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
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
    for base in (os.path.join(WORK, "cfg"), os.path.join(WORK, "state")):
        for dirpath, _, files in os.walk(base):
            for fn in files:
                p = os.path.join(dirpath, fn)
                with open(p, "rb") as f:
                    if needle.encode() in f.read():
                        hits.append(p)
    return hits


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


def add_accounts(brain_url, game_port):
    """Add stores an account and signs nobody in: both rows listed, none active, the vault written."""
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
""", brain_url, game_port)
    check("an empty Add asks for the login and the password", "Enter the login and the password." in text_of(log, "empty"),
          text_of(log, "empty")[-120:])
    check("Add says the account is added", f"Added {NAME}." in text_of(log, "added1"), text_of(log, "added1")[-120:])
    check("a second Add lists a second account", f"Added {NAME2}." in text_of(log, "added2"), text_of(log, "added2")[-120:])
    check("Add signs nobody in (the current account is unchanged: none)", logins_as(log, NAME) + logins_as(log, NAME2) == 0
          and "[account] logged in as" not in log and "Signed in as" not in text_of(log, "added1") + text_of(log, "added2"),
          f"logins {logins_as(log, NAME)} {logins_as(log, NAME2)}")
    check("both accounts are listed", match_of(log, "rows") == 2, str(match_of(log, "rows")))
    check("neither listed account is active after Add", match_of(log, "active") == 0, str(match_of(log, "active")))
    check("the Add stored an alt vault", os.path.exists(VAULT))
    check("the password is on no client file after Add", not secret_on_disk(PASS), repr(secret_on_disk(PASS)))


def use_accounts(brain_url, game_port):
    """Use signs in from the stored token, with no password. An Add of a listed account while another is signed in
    lists it (its token and password) and leaves the signed-in account as it is; the Use of that row then signs in
    from the token the Add saved."""
    log = run_client("use", f"""0 menu
3000 call showWindow {WINDOW}
3400 clicksel [onclick*="knAltUse"]
6500 text @switched [id="knAltMsg"]
6600 clickid knAltUser
6700 type {NAME}
6800 clickid knAltPass
6900 type {PASS}
7000 clicksel [onclick*="knAltAdd"]
9500 text @readded [id="knAltMsg"]
9600 nodes @active .knAltActive
9700 text @act .knAltActive
9800 clicksel [onclick*="knAltUse"]
13000 text @back [id="knAltMsg"]
13500 quit
""", brain_url, game_port)
    check("Use signs in with the stored token (no password)", f"Signed in as {NAME2}." in text_of(log, "switched"),
          text_of(log, "switched")[-120:])
    check("Add of a listed account while another is signed in lists it and does not switch",
          f"Added {NAME}." in text_of(log, "readded") and NAME2 in text_of(log, "act") and match_of(log, "active") == 1,
          text_of(log, "readded")[-120:] + " | " + text_of(log, "act")[-120:])
    check("Use after that Add signs in from the token it saved (no password)", f"Signed in as {NAME}." in text_of(log, "back"),
          text_of(log, "back")[-120:])
    check("each Use signed its account in once; the Add signed nobody in",
          logins_as(log, NAME) == 1 and logins_as(log, NAME2) == 1, f"logins {logins_as(log, NAME)} {logins_as(log, NAME2)}")


def restart_resumes(brain_url, game_port):
    """Add, Use, restart: the account Used resumes at start from its stored token, and the other one Used after the
    restart signs in from its token. No password prompt anywhere; no password on a client file."""
    log = run_client("restart", f"""0 menu
2000 call showWindow {WINDOW}
3000 text @rmsg [id="knAltMsg"]
3400 clicksel [onclick*="knAltUse"]
6500 text @rused [id="knAltMsg"]
7000 quit
""", brain_url, game_port)
    check("a restart resumes the Used account from its stored token", logins_as(log, NAME) >= 1, f"logins {logins_as(log, NAME)}")
    check("and asks for no password", "no longer valid" not in text_of(log, "rmsg") and "Enter the password" not in text_of(log, "rmsg"),
          text_of(log, "rmsg")[-120:])
    check("Use after the restart signs in from the stored token (no password)", f"Signed in as {NAME2}." in text_of(log, "rused"),
          text_of(log, "rused")[-120:])
    check("the password is on no client file after a restart", not secret_on_disk(PASS), repr(secret_on_disk(PASS)))


def silent_password(brain_url, game_port, db):
    """A refused saved token: the stored password signs the active account in again at start, with no prompt. (The
    restart before left NAME2 active: its sessions are the ones the brain drops.)"""
    con = sqlite3.connect(db)
    with con:
        con.execute("DELETE FROM sessions WHERE account_id = (SELECT id FROM accounts WHERE username = ?)", (NAME2,))
    con.close()
    log = run_client("silent", f"""0 menu
2000 call showWindow {WINDOW}
7000 text @smsg [id="knAltMsg"]
7500 quit
""", brain_url, game_port)
    check("a refused token signs the account in with its stored password (no prompt)", logins_as(log, NAME2) >= 1,
          f"logins {logins_as(log, NAME2)}")
    check("... and says nothing is wrong", "no longer valid" not in text_of(log, "smsg") and "Enter the password" not in text_of(log, "smsg"),
          text_of(log, "smsg")[-120:])


def invalid_login(brain_url, game_port, db):
    """A refused token and a refused password (the brain forgets the account): the row stays listed, says the saved
    login is no longer valid, and its Sign in opens the form for that account (no password prompt)."""
    con = sqlite3.connect(db)
    with con:
        con.execute("DELETE FROM sessions WHERE account_id = (SELECT id FROM accounts WHERE username = ?)", (NAME2,))
        con.execute("DELETE FROM accounts WHERE username = ?", (NAME2,))
    con.close()
    log = run_client("invalid", f"""0 menu
2000 call showWindow {WINDOW}
7000 text @imsg [id="knAltMsg"]
7100 nodes @signin [onclick*="knAltSignIn"]
7200 clicksel [onclick*="knAltSignIn"]
7500 text @iform [id="knAltUser"]
8000 quit
""", brain_url, game_port)
    check("a refused token and password say the saved login is no longer valid",
          "Saved login is no longer valid. Sign in again to restore it." in text_of(log, "imsg"), text_of(log, "imsg")[-120:])
    check("the row offers Sign in (one row)", match_of(log, "signin") == 1, str(match_of(log, "signin")))
    check("Sign in opens the form for that account (no prompt)", f"value={NAME2}" in text_of(log, "iform")
          and "Enter the password" not in log, text_of(log, "iform")[-80:])


def dropped_on_restart(brain_url, game_port):
    """The saved login the run before left with neither a session nor a password is dropped at the next start (the
    vault keeps nothing it cannot sign in with): one row is left, NAME's."""
    log = run_client("dropped", f"""0 menu
2000 call showWindow {WINDOW}
3000 nodes @rows .knAltRow
3500 quit
""", brain_url, game_port)
    check("a restart drops the saved login with neither a session nor a password", match_of(log, "rows") == 1,
          f"rows {match_of(log, 'rows')}")


def remove_account(brain_url, game_port):
    log = run_client("remove", f"""0 menu
2000 call showWindow {WINDOW}
2500 clicksel [onclick*="knAltRemove"]
3000 text @removed [id="knAltMsg"]
3500 nodes @rows .knAltRow
4000 quit
""", brain_url, game_port)
    check("Remove forgets the account", f"Removed {NAME}." in text_of(log, "removed"), text_of(log, "removed")[-120:])
    check("no rows after Remove", match_of(log, "rows") == 0, str(match_of(log, "rows")))
    restart = run_client("after_remove", f"""0 menu
2000 call showWindow {WINDOW}
3000 nodes @rows .knAltRow
3500 quit
""", brain_url, game_port)
    check("a restart lists nothing and signs nobody in", logins_as(restart, NAME) == 0 and match_of(restart, "rows") == 0,
          f"rows {match_of(restart, 'rows')}")
    check("the password is on no client file", not secret_on_disk(PASS), repr(secret_on_disk(PASS)))
    check("the password is in no client log", not any(PASS in open(os.path.join(WORK, f), errors="replace").read()
                                                      for f in os.listdir(WORK) if f.endswith(".log") and f != "brain.log"))


def clear_kio_session():
    """The krunker.io session the client keeps for itself (kio_*): gone, as on a fresh start with none."""
    d = os.path.join(WORK, "cfg", "krunknative")
    for fn in os.listdir(d) if os.path.isdir(d) else []:
        if fn.startswith("kio_"):
            os.remove(os.path.join(d, fn))


def krunker_row(brain_url, game_port, stub):
    """A krunker.io row: added with a typed login (the Security Challenge passes by itself in the webview; the code
    step), used with its stored refresh token, refused (the session ended) so the stored password signs it in again
    (the code step again), removed."""
    kio_log = lambda log: log.count(f"[kio-account] signed in to krunker.io as {kae.LOGIN}")
    log = run_client("kio_add", f"""0 menu
500 call showWindow {WINDOW}
1000 call knAltBackend 1
1200 clickid knAltUser
1300 type {kae.LOGIN}
1400 clickid knAltPass
1500 type {kae.PASSWORD}
1600 clicksel [onclick*="knAltAdd"]
5000 clickid knAltCode
5100 type {kae.CODE}
5200 clicksel [onclick*="knAltCodeSubmit"]
7000 text @kadd [id="knAltMsg"]
7100 nodes @krows .knAltRow
7150 nodes @ktag .knAltTagKio
7500 quit
""", brain_url, game_port)
    check("krunker.io Add stores the account and signs nobody in (the game's session is unchanged)",
          kio_log(log) == 0 and "signed in to krunker.io" not in log, f"sign-ins {kio_log(log)}")
    check("krunker.io Add says it is added", f"Added {kae.LOGIN}." in text_of(log, "kadd"), text_of(log, "kadd")[-120:])
    check("the krunker.io row is listed with its krunker.io tag", match_of(log, "krows") == 2 and match_of(log, "ktag") == 1,
          f"rows {match_of(log, 'krows')} tags {match_of(log, 'ktag')}")
    check("the password is on no client file after the krunker.io Add", not secret_on_disk(kae.PASSWORD), "")

    # a fresh start with the krunker.io session gone: Use resumes the row with its stored refresh token
    clear_kio_session()
    n_refresh = len(stub.find("POST", "/frvr/v1/auth/refresh"))
    log = run_client("kio_use", f"""0 menu
500 call showWindow {WINDOW}
1000 nodes @krows .knAltRow
1200 clicksel [onclick*="knAltUse"]
4000 text @kuse [id="knAltMsg"]
4500 quit
""", brain_url, game_port)
    check("Use on the krunker.io row resumes it with its stored refresh token",
          len(stub.find("POST", "/frvr/v1/auth/refresh")) > n_refresh and f"Signed in as {kae.LOGIN}." in text_of(log, "kuse"),
          text_of(log, "kuse")[-120:])

    # the refresh token is refused: the stored password signs the krunker.io account in again (its 2FA code is typed
    # in the code step; no password prompt)
    clear_kio_session()
    stub.refresh_ok = ()
    try:
        log = run_client("kio_silent", f"""0 menu
500 call showWindow {WINDOW}
1200 clicksel [onclick*="knAltUse"]
9000 clickid knAltCode
9100 type {kae.CODE}
9200 clicksel [onclick*="knAltCodeSubmit"]
13000 text @ksilent [id="knAltMsg"]
13500 quit
""", brain_url, game_port)
    finally:
        stub.refresh_ok = (kae.REFRESH1, kae.REFRESH2)
    check("a refused krunker.io token signs in with the stored password (no password prompt)", kio_log(log) >= 1,
          f"sign-ins {kio_log(log)}")
    check("... and says it is signed in", f"Signed in as {kae.LOGIN}." in text_of(log, "ksilent"), text_of(log, "ksilent")[-120:])
    check("... with no password prompt", "Enter the password" not in log and "no longer valid" not in text_of(log, "ksilent"), "")

    log = run_client("kio_remove", f"""0 menu
500 call showWindow {WINDOW}
1200 clicksel [onclick*="knAltRemove"]
3500 text @kremoved [id="knAltMsg"]
4000 nodes @krows .knAltRow
4100 nodes @ktag .knAltTagKio
4500 quit
""", brain_url, game_port)
    check("Remove forgets the krunker.io row and signs its account out",
          f"Removed {kae.LOGIN}." in text_of(log, "kremoved") and "[kio-account] signed out of krunker.io" in log,
          text_of(log, "kremoved")[-120:])
    check("no krunker.io row after Remove", match_of(log, "ktag") == 0 and match_of(log, "krows") == 1,
          f"rows {match_of(log, 'krows')} tags {match_of(log, 'ktag')}")
    tokens = [kae.PASSWORD, kae.REFRESH1, kae.REFRESH2]
    leaked = [t for t in tokens if secret_on_disk(t) or any(t in open(os.path.join(WORK, f), errors="replace").read()
                                                             for f in os.listdir(WORK) if f.endswith(".log"))]
    check("no krunker.io password or token on a client file or in a log", not leaked, repr(leaked))


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
        add_accounts(brain_url, game_port)
        use_accounts(brain_url, game_port)
        restart_resumes(brain_url, game_port)
        silent_password(brain_url, game_port, db)
        invalid_login(brain_url, game_port, db)
        dropped_on_restart(brain_url, game_port)
        krunker_row(brain_url, game_port, stub)
        remove_account(brain_url, game_port)
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
