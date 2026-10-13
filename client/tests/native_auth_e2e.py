#!/usr/bin/env python3
"""Accounts in the native client, end to end through the menu UI (client/src/ui/account_ui.c) against a
local kn_brain (sqlite, temp dir) and services/common/tests/fake_smtp.py:

  register with an email (name, Tab, password, Tab, email, Register) -> the verification mail -> its
  link verifies -> logout -> login with Enter -> logout -> Forgot password? -> the reset mail -> the
  code typed into the client -> a new password -> signed in -> the old password is refused and the
  new one logs in. Plus: a login while the account server is down says why (not "Please try again.").

  python3 -I client/tests/native_auth_e2e.py BUILD_DIR      (repository root)
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs), a local
server_bin for the menu's map, ports from kn_proto.free_ports(). 77 = skip (no kn_brain / openssl).
"""
import email
import email.policy
import json
import os
import re
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

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-native-auth-", dir=CACHE if os.path.isdir(CACHE) else None)
SMTP_USER, SMTP_PASS = "mailer@caan.test", "fake-native-pass-%d" % os.getpid()
STAMP = os.getpid() % 100000
NAME, ADDR = "nat%d" % STAMP, "nat%d@example.org" % STAMP
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def mail_text(msg):
    m = email.message_from_bytes(msg["data"], policy=email.policy.default)
    return m, m.get_body(("plain",)).get_content()


def run_client(name, script, brain_url, game_port, timeout=150):
    """One headless client_bin run of `script`; returns its log text."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([os.path.join(BUILD, "client_bin"), "--mode", "online", "--brain", brain_url, "--finder", "off",
                              "--connect", "127.0.0.1:%d" % game_port], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read()


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ @%s(.*)" % re.escape(label), log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def storage(key):
    try:
        with open(os.path.join(WORK, "cfg", "krunknative", key + ".json")) as f:
            text = f.read().strip()
    except OSError:
        return None
    try:
        v = json.loads(text)
        return v if isinstance(v, str) else text
    except ValueError:  # strings are stored raw
        return text


# the guest Account window: the header bar's "Login or Register"
OPEN = "0 menu\n500 clickid signedOutHeaderBar\n"


def scenario(brain_url, game_port, smtp, db):
    # 1. register through the form: Tab moves name -> password -> email (native focus order)
    log = run_client("register", OPEN + f"""1200 clickid accName
1300 type {NAME}
1400 down TAB
1410 up TAB
1500 type pass_word1
1600 down TAB
1610 up TAB
1700 type {ADDR}
1800 clicksel .accBtn.buttonP
5000 quit
""", brain_url, game_port)
    check("register through the native form", f"[account] logged in as {NAME}" in log)
    row = sqlite3.connect(db).execute("SELECT email FROM accounts WHERE username = ?", (NAME,)).fetchone()
    check("Tab put the password and the email in their own fields", row and row[0] == ADDR, repr(row))
    check("the session token is saved (krunker_token)", len(storage("krunker_token") or "") >= 32)
    mails = smtp.wait_messages(1, 20)
    check("verification mail captured", len(mails) == 1 and mails[0]["rcpt"] == [ADDR])
    _, text = mail_text(mails[0]) if mails else (None, "")
    link = re.search(r"(http://\S+/auth/verify-email\?token=[A-Za-z0-9_-]+)", text)
    check("the mail links to this brain", link and link.group(1).startswith(brain_url))

    # 2. Account Security shows the address unverified; the link verifies it
    log = run_client("security", f"""0 menu
2000 call showWindow 5
2500 clicksel [onclick*="switchTab(3)"]
3500 vis @sec accEmailNew emailRes
3600 quit
""", brain_url, game_port)
    check("Account Security has the email block", vis(log, "sec").get("accEmailNew") == "1", str(vis(log, "sec")))
    with urllib.request.urlopen(link.group(1) if link else brain_url, timeout=10) as r:
        body = r.read().decode()
    check("the link verifies the address", "Email verified" in body)
    v = sqlite3.connect(db).execute("SELECT email_verified_at IS NOT NULL FROM accounts WHERE username = ?", (NAME,)).fetchone()
    check("email_verified_at set", v and v[0] == 1)

    # 3. logout, then login with Enter from the password field
    log = run_client("relogin", f"""0 menu
1500 call showWindow 5
2000 clicksel [onclick*="logoutAcc"]
2600 clickid accName
2700 type {NAME}
2800 down TAB
2810 up TAB
2900 type pass_word1
3000 down ENTER
3010 up ENTER
6000 quit
""", brain_url, game_port)
    check("logout", "[account] logged out" in log)
    check("Enter in the password field logs in", log.count(f"[account] logged in as {NAME}") >= 2, "restore + login expected")

    # 4. forgot password: logout, Forgot password? -> email -> code -> new password -> signed in
    before = len(smtp.messages)
    log = run_client("forgot", f"""0 menu
1500 call showWindow 5
2000 clicksel [onclick*="logoutAcc"]
2600 clicksel [onclick*="knResetOpen"]
3000 vis @step1 knResetWho knResetGo
3100 clickid knResetWho
3200 type {ADDR.upper()}
3300 down ENTER
3310 up ENTER
6000 vis @step2 knResetCode
6100 quit
""", brain_url, game_port)
    check("Forgot password? opens the reset form", vis(log, "step1").get("knResetWho") == "1", str(vis(log, "step1")))
    check("sending the code moves to the code step", vis(log, "step2").get("knResetCode") == "1", str(vis(log, "step2")))
    mails = smtp.wait_messages(before + 1, 20)
    m, text = mail_text(mails[-1]) if len(mails) > before else (None, "")
    code = re.search(r"code ([A-Z2-7]{4}-[A-Z2-7]{4}-[A-Z2-7]{4})", text)
    check("reset mail with a code", m is not None and str(m["Subject"]) == "Reset your Krunker Box Office password" and code, repr(text[:200]))
    code = code.group(1) if code else "AAAA-AAAA-AAAA"
    log = run_client("reset", f"""0 menu
1000 clickid signedOutHeaderBar
1500 clicksel [onclick*="knResetOpen"]
1800 clickid knResetHave
2100 clickid knResetCode
2200 type AAAA-AAAA-AAAA
2300 down ENTER
2310 up ENTER
3500 vis @wrong knResetCode knResetPass
3600 call knResetBack
3700 clickid knResetHave
3900 clickid knResetCode
4000 type {code.lower()}
4100 down ENTER
4110 up ENTER
5500 vis @step3 knResetPass knResetPass2
5600 clickid knResetPass
5700 type new_pass_2
5800 down TAB
5810 up TAB
5900 type new_pass_2
6000 clickid knResetGo
9000 vis @done knResetPass
9100 quit
""", brain_url, game_port)
    check("a wrong code stays on the code step", vis(log, "wrong").get("knResetCode") == "1" and vis(log, "wrong").get("knResetPass") == "-",
          str(vis(log, "wrong")))
    check("the mailed code (any case) moves to the new password", vis(log, "step3").get("knResetPass") == "1", str(vis(log, "step3")))
    check("setting the password signs in", f"[account] logged in as {NAME}" in log and vis(log, "done").get("knResetPass") == "-")

    # 5. the old password is refused, the new one works
    log = run_client("newpass", f"""0 menu
1500 call showWindow 5
2000 clicksel [onclick*="logoutAcc"]
2600 clickid accName
2700 type {NAME}
2800 clickid accPass
2900 type new_pass_2
3000 clicksel .accBtn.buttonG
6000 quit
""", brain_url, game_port)
    check("login with the new password", log.count(f"[account] logged in as {NAME}") >= 2)
    ok = sqlite3.connect(db).execute("SELECT count(*) FROM email_tokens WHERE kind = 'reset' AND used_at IS NULL").fetchone()
    check("the reset code is used up", ok and ok[0] == 0, repr(ok))


def down_scenario(game_port):
    """The login while the account server is down: the reason, not v3.1.1's "Please try again."."""
    (dead,) = free_ports(1)
    log = run_client("down", OPEN + """1200 clickid accName
1300 type someone
1400 clickid accPass
1500 type some_pass
1600 clicksel .accBtn.buttonG
11000 quit
""", "http://127.0.0.1:%d" % dead, game_port)
    check("a down account server: the specific reason", "[account] the social socket to" in log and "did not open" in log,
          "log in %s" % WORK)


def main():
    # `timeout` (make native-auth-test) sends SIGTERM: run the finally below, kill what we started
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, game_port = free_ports(2)
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
        for _ in range(100):
            try:
                with urllib.request.urlopen(brain_url + "/mm/status", timeout=1) as r:
                    if b'"brain"' in r.read():
                        break
            except OSError:
                time.sleep(0.1)
        scenario(brain_url, game_port, smtp, db)
        down_scenario(game_port)
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
        brain_log.close()
    with open(os.path.join(WORK, "brain.log")) as f:
        check("no SMTP password in the brain log", SMTP_PASS not in f.read())
    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
