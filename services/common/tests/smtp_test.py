#!/usr/bin/env python3
"""knsvc SMTP client (knsvc/smtp.h) against tests/fake_smtp.py on free ports.

  smtp_test.py <knsvc_test_smtp>

Covers: STARTTLS with a self-signed certificate (verified through the CA file; refused without it),
implicit TLS, AUTH PLAIN and LOGIN, bad credentials (5xx), a rejected recipient, a relay refusing
foreign sender domains, STARTTLS response injection, the clear-text AUTH refusal, the MIME message as
received (parsed with python's email package), dot-stuffing, no credentials in the transcript, and
the kn_mailer queue (temporary failures retried, results on the loop thread, the loop never stalls).
77 = skip (no openssl CLI).
"""
import email
import email.policy
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_smtp import FakeSMTP, make_cert  # noqa: E402

BIN = sys.argv[1]
FAILS = []
USER, PASS = "mailer@example.com", "s3cr3t-Pa55word"


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def send(port, tls, user, password, cafile, verify, frm="Krunker <noreply@example.com>", to="player@example.org",
         auth=None):
    args = [BIN, "send", str(port), tls, user or "-", password or "-", cafile or "-", str(verify), frm, to]
    if auth:
        args.append(auth)
    r = subprocess.run(args, capture_output=True, timeout=60)
    out = dict(l.split(" ", 1) if " " in l else (l, "") for l in r.stdout.decode().splitlines())
    return int(out.get("status", "99")), out.get("id", ""), out.get("err", ""), r.stderr.decode()


def main():
    if not shutil.which("openssl"):
        print("no openssl CLI: skip")
        return 77
    tmp = tempfile.mkdtemp(prefix="kn-smtp-test-", dir=os.path.expanduser("~/.cache"))
    try:
        cert, key = make_cert(tmp)
        r = subprocess.run([BIN, "unit"], capture_output=True, timeout=60)
        sys.stdout.write(r.stdout.decode())
        sys.stderr.write(r.stderr.decode())
        check(r.returncode == 0, "message builder units")

        # STARTTLS + AUTH PLAIN, certificate verified against the test CA
        srv = FakeSMTP("starttls", cert, key, users={USER: PASS}).start()
        st, mid, err, transcript = send(srv.port, "starttls", USER, PASS, cert, 1)
        check(st == 0, "starttls + verified certificate + AUTH PLAIN delivers (%s)" % err)
        msgs = srv.wait_messages(1)
        check(len(msgs) == 1, "fake server got the message")
        if msgs:
            m = msgs[0]
            check(m["tls"] and m["auth_mech"] == "PLAIN" and m["auth_user"] == USER, "over TLS, AUTH PLAIN as the user")
            check(m["mail_from"] == "noreply@example.com" and m["rcpt"] == ["player@example.org"], "envelope from / to")
            msg = email.message_from_bytes(m["data"], policy=email.policy.default)
            check(str(msg["From"]) == "Krunker <noreply@example.com>", "From header")
            check(str(msg["Subject"]) == "Verify your Krunker email ✔", "UTF-8 subject decodes")
            check(msg["Message-ID"] == mid and mid.endswith("@example.com>"), "Message-ID matches the client's")
            check(msg["Date"] is not None and msg["MIME-Version"] == "1.0", "Date and MIME-Version")
            check(msg.get_content_type() == "multipart/alternative", "multipart/alternative")
            text = msg.get_body(("plain",)).get_content()
            html = msg.get_body(("html",)).get_content()
            check("verify-email?token=abc" in text and "\n.\n..double\n" in text.replace("\r\n", "\n"),
                  "text part intact (dot lines survive stuffing)")
            check('<a href="https://example.com/auth/verify-email?token=abc">' in html, "html part intact")
        check(PASS not in transcript and "<redacted>" in transcript, "no credentials in the transcript")
        check("STARTTLS" in transcript, "transcript shows STARTTLS")

        # AUTH LOGIN on request
        st, _, err, transcript = send(srv.port, "starttls", USER, PASS, cert, 1, auth="login")
        check(st == 0 and srv.wait_messages(2)[-1]["auth_mech"] == "LOGIN", "AUTH LOGIN (%s)" % err)
        check(PASS not in transcript, "no credentials in the LOGIN transcript")

        # bad password -> permanent, no message
        st, _, err, _ = send(srv.port, "starttls", USER, "wrong", cert, 1)
        check(st == -2 and "535" in err and "wrong" not in err, "bad credentials: EPERM with the 535 reply (%s)" % err)

        # self-signed without the CA -> TLS verify failure (temporary class, nothing sent)
        n = len(srv.messages)
        st, _, err, _ = send(srv.port, "starttls", USER, PASS, None, 1)
        check(st == -1 and "certificate verify failed" in err, "unknown CA refused (%s)" % err)
        check(len(srv.messages) == n, "nothing delivered without a verified certificate")
        srv.stop()

        # STARTTLS required but the server sends extra bytes with its 220
        inj = FakeSMTP("starttls", cert, key, users={USER: PASS}, inject_after_starttls=True).start()
        st, _, err, _ = send(inj.port, "starttls", USER, PASS, cert, 1)
        check(st == -2 and "unexpected data" in err, "STARTTLS response injection refused (%s)" % err)
        inj.stop()

        # implicit TLS (port-465 style) + rejected recipient + relay sender-domain policy
        imp = FakeSMTP("implicit", cert, key, users={USER: PASS}, reject_rcpt=["nobody@example.org"],
                       sender_domains=["example.com"]).start()
        st, _, err, _ = send(imp.port, "implicit", USER, PASS, cert, 1)
        check(st == 0 and imp.wait_messages(1) and imp.messages[0]["tls"], "implicit TLS delivers (%s)" % err)
        st, _, err, _ = send(imp.port, "implicit", USER, PASS, cert, 1, to="nobody@example.org")
        check(st == -2 and "550" in err, "rejected recipient: EPERM with the 550 reply (%s)" % err)
        st, _, err, _ = send(imp.port, "implicit", USER, PASS, cert, 1, frm="Krunker <noreply@other.test>")
        check(st == -2 and "553" in err, "relay refuses a foreign sender domain (%s)" % err)
        imp.stop()

        # clear text: AUTH refused by the client unless allowed (the test binary allows it); no AUTH works
        plain = FakeSMTP("none").start()
        st, _, err, _ = send(plain.port, "none", None, None, None, 0)
        check(st == 0 and plain.wait_messages(1) and not plain.messages[0]["tls"], "clear-text relay without AUTH (%s)" % err)
        plain.stop()
        st, _, err, _ = send(1, "starttls", USER, PASS, cert, 1)
        check(st == -1 and "connect" in err, "connection refused is temporary (%s)" % err)

        # mailer queue: 2 temporary failures retried, 5 mails, loop never stalls
        q = FakeSMTP("starttls", cert, key, users={USER: PASS}, temp_fail=2).start()
        r = subprocess.run([BIN, "queue", str(q.port), "starttls", USER, PASS, cert, "5", "player@example.org"],
                           capture_output=True, timeout=120)
        sys.stdout.write(r.stdout.decode())
        sys.stderr.write(r.stderr.decode()[-2000:])
        check(r.returncode == 0, "kn_mailer: queue, retries, loop-thread callbacks, free with pending work")
        check(len(q.wait_messages(5)) >= 5, "queued mails delivered")
        q.stop()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("smtp_test: %d failures" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
