#!/usr/bin/env python3
"""The Account Manager (client/src/ui/alt_manager_ui.c, ALT_MANAGER_WINDOW = 104) end to end through the
menu UI, against a local kn_brain (sqlite, temp dir) and services/common/tests/fake_smtp.py:

  register with the login popup (a plain sign-in stores nothing: no alt vault, a restart signs nobody in
  and the login card asks for the password) -> the Account Manager's Add with a typed login and password
  (an empty Add says so; the account is listed and signed in; the password is on no disk) -> a restart
  resumes the listed account with its stored session -> Use on the other row switches to it with no
  password, and Use back on the row left signs that account in with no password too (switching keeps the
  session of the account left) -> the brain drops the account's sessions, a restart asks for the
  password (Sign in fills the form) -> Add signs in again -> Remove forgets the account (and signs it
  out), a restart lists nothing.

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
9000 quit
""", brain_url, game_port)
    check("an empty Add asks for the login and the password", "Enter the login and the password." in text_of(log, "empty"),
          text_of(log, "empty")[-120:])
    check("Add signs the typed account in", logins_as(log, NAME) >= 1)
    check("Add says it is signed in", f"Signed in as {NAME}." in text_of(log, "added1"), text_of(log, "added1")[-120:])
    check("a second Add lists a second account", f"Signed in as {NAME2}." in text_of(log, "added2"), text_of(log, "added2")[-120:])
    check("both accounts are listed", match_of(log, "rows") == 2, str(match_of(log, "rows")))
    check("the Add stored an alt vault", os.path.exists(VAULT))
    check("the password is on no client file after Add", not secret_on_disk(PASS), repr(secret_on_disk(PASS)))


def switch_account(brain_url, game_port):
    # switching away from the signed-in account keeps its stored session: Use on it again (the row that
    # was left) signs it in with no password, the same run
    log = run_client("switch", f"""0 menu
3000 call showWindow {WINDOW}
3400 nodes @use [onclick*="knAltUse"]
3500 clicksel [onclick*="knAltUse"]
6500 text @switched [id="knAltMsg"]
6600 clicksel [onclick*="knAltUse"]
9600 text @back [id="knAltMsg"]
9700 clicksel [onclick*="knAltUse"]
12600 text @returned [id="knAltMsg"]
13000 quit
""", brain_url, game_port)
    check("a restart resumes the active listed account", logins_as(log, NAME2) >= 1)
    check("Use on the other row switches with its stored session (no password)", logins_as(log, NAME) >= 1,
          f"logins as {NAME}: {logins_as(log, NAME)}")
    check("Use says it is signed in", f"Signed in as {NAME}." in text_of(log, "switched"), text_of(log, "switched")[-120:])
    check("switching back to the account left signs it in with no password (its session was kept)",
          f"Signed in as {NAME2}." in text_of(log, "back") and f"Enter the password for {NAME2}." not in text_of(log, "back"),
          text_of(log, "back")[-120:])
    check("and back again to the first account, with no password", f"Signed in as {NAME}." in text_of(log, "returned"),
          text_of(log, "returned")[-120:])


def expired_session(brain_url, game_port, db):
    con = sqlite3.connect(db)
    with con:
        con.execute("DELETE FROM sessions WHERE account_id = (SELECT id FROM accounts WHERE username = ?)", (NAME,))
    con.close()
    # the second row is the last Remove on the window; it is dropped so the first row's Use is unambiguous
    log = run_client("expired", f"""0 menu
3000 call showWindow {WINDOW}
3400 clicksel [onclick*="knAltRemove"]
3800 text @dropped [id="knAltMsg"]
4200 clicksel [onclick*="knAltUse"]
4600 text @ask [id="knAltMsg"]
4700 text @form [id="knAltUser"]
4800 clickid knAltPass
4900 type {PASS}
5100 clicksel [onclick*="knAltAdd"]
8000 text @again [id="knAltMsg"]
8500 quit
""", brain_url, game_port)
    check("Remove drops the second account", f"Removed {NAME2}." in text_of(log, "dropped"), text_of(log, "dropped")[-120:])
    check("a refused session asks for the password (Sign in fills the form)",
          f"Enter the password for {NAME}." in text_of(log, "ask") and f"value={NAME}" in text_of(log, "form"),
          text_of(log, "ask")[-120:] + " | " + text_of(log, "form")[-80:])
    check("Add with the password signs in again", logins_as(log, NAME) >= 1)
    check("the account is signed in again", f"Signed in as {NAME}." in text_of(log, "again"), text_of(log, "again")[-120:])


def remove_account(brain_url, game_port):
    log = run_client("remove", f"""0 menu
3000 call showWindow {WINDOW}
3500 clicksel [onclick*="knAltRemove"]
5000 text @removed [id="knAltMsg"]
5500 nodes @rows .knAltRow
6000 quit
""", brain_url, game_port)
    check("Remove forgets the account", f"Removed {NAME}." in text_of(log, "removed"), text_of(log, "removed")[-120:])
    check("Remove signs out the account it removed", "[account] logged out" in log)
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
    """A krunker.io row: added with a typed login (the Security Challenge passes by itself in the webview), used with its stored
    refresh token, refused (the session ended) so the row asks for its password, added again, removed."""
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
    check("krunker.io Add signs the typed account in (the captcha answered)", kio_log(log) >= 1,
          f"sign-ins {kio_log(log)}")
    check("krunker.io Add says it is signed in", f"Signed in as {kae.LOGIN}." in text_of(log, "kadd"), text_of(log, "kadd")[-120:])
    check("the krunker.io row is listed with its krunker.io tag", match_of(log, "krows") == 1 and match_of(log, "ktag") == 1,
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

    # the session is refused (the refresh token no good): the row asks for the password, Add signs in again
    clear_kio_session()
    stub.refresh_ok = ()
    try:
        log = run_client("kio_expired", f"""0 menu
500 call showWindow {WINDOW}
1200 clicksel [onclick*="knAltUse"]
4000 text @kask [id="knAltMsg"]
4100 text @kform [id="knAltUser"]
4300 clickid knAltPass
4400 type {kae.PASSWORD}
4600 clicksel [onclick*="knAltAdd"]
8000 clickid knAltCode
8100 type {kae.CODE}
8200 clicksel [onclick*="knAltCodeSubmit"]
12000 text @kagain [id="knAltMsg"]
12500 quit
""", brain_url, game_port)
    finally:
        stub.refresh_ok = (kae.REFRESH1, kae.REFRESH2)
    check("a refused krunker.io session asks for its password (the form fills in)",
          f"Enter the password for {kae.LOGIN}." in text_of(log, "kask") and f"value={kae.LOGIN}" in text_of(log, "kform"),
          text_of(log, "kask")[-120:] + " | " + text_of(log, "kform")[-80:])
    check("Add with the password signs the krunker.io account in again", kio_log(log) >= 1 and f"Signed in as {kae.LOGIN}." in text_of(log, "kagain"),
          text_of(log, "kagain")[-120:])

    log = run_client("kio_remove", f"""0 menu
500 call showWindow {WINDOW}
1200 clicksel [onclick*="knAltRemove"]
3500 text @kremoved [id="knAltMsg"]
4000 nodes @krows .knAltRow
4500 quit
""", brain_url, game_port)
    check("Remove forgets the krunker.io row and signs its account out",
          f"Removed {kae.LOGIN}." in text_of(log, "kremoved") and "[kio-account] signed out of krunker.io" in log,
          text_of(log, "kremoved")[-120:])
    check("no krunker.io row after Remove", match_of(log, "krows") == 0, str(match_of(log, "krows")))
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
        switch_account(brain_url, game_port)
        expired_session(brain_url, game_port, db)
        remove_account(brain_url, game_port)
        krunker_row(brain_url, game_port, stub)
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
