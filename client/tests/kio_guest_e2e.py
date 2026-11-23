#!/usr/bin/env python3
"""A krunker.io guest's FRVR anonymous session and its Unclaimed Rewards (krunker_account.c krunker_account_frvr_token,
v10_rewards.c), end to end against the krunker.io stubs of server/tests/krunker_account_e2e.py and a FRVR auth stub of
its own. Nothing leaves the machine; no kn_brain.

  python3 -I client/tests/kio_guest_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7: krunker.io's page configures the FRVR SDK with auth.enableAnonymousLogin (live/index.html), so the SDK's init
signs a player with no account in anonymously (frvr-sdk loginAsAnonymous: POST <auth>/login {"platform":"anonymous"} ->
201 {accessToken, refreshToken}; the refresh token's extra.platform is getCurrentPlatform()). That pair is the Bearer of
the API clients (deob-10.0.7.js:27762-27790), the seek's and the game socket's token (:149727, :229296), and with
"anonymous" === getCurrentPlatform() the page asks gapi GET /guest/profile (:230083, :119590-119594); GUEST_EARNED_REWARDS
("ger") asks again (:229362). The box shows a guest's funds / score when either is above 0 (:117496).
The client starts with no krunker.io session and joins the stub's game:
  login    one POST <auth>/login with {"platform":"anonymous"}
  join     seek-game's accessToken and the socket's at= are the anonymous access token
  profile  GET gapi /guest/profile with it as the Bearer; the box shows the guest's KR
  ger      the game server's "ger" (sent after ENTER_GAME): /guest/profile again, the box shows the new amount
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
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-guest-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ANON_ACCESS = kae.jwt({"exp": int(time.time()) + 3600, "sub": "frvr-anon-1", "extra": {"platform": "anonymous"}})
ANON_REFRESH = kae.jwt({"exp": int(time.time()) + 30 * 86400, "sub": "frvr-anon-1", "extra": {"platform": "anonymous"}})
PROFILE = "/api/guest/profile"
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


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


class Frvr:
    """FRVR's auth service: POST /v1/auth/login {"platform":"anonymous"} -> 201 the anonymous pair."""

    def __init__(self, port):
        self.port, self.requests, self.httpd = port, [], None

    def start(self):
        stub = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(n) if n else b""
                try:
                    body = json.loads(raw or b"null")
                except ValueError:
                    body = raw.decode(errors="replace")
                stub.requests.append((self.path, body, {k.lower(): v for k, v in self.headers.items()}))
                if self.path == "/v1/auth/login" and body == {"platform": "anonymous"}:
                    out, code = {"accessToken": ANON_ACCESS, "refreshToken": ANON_REFRESH}, 201
                else:
                    out, code = {"error": "not found"}, 404
                data = json.dumps(out).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.port), H)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()


def script():
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "9000 text @box1 [id=knGuestEarned]",
         "13000 respawn",
         "16000 down ESC", "16050 up ESC",
         "18500 text @box2 [id=knGuestEarned]",
         "19000 quit"]
    return "\n".join(s) + "\n"


def run_client(stub, frvr):
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
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:9", "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": "http://127.0.0.1:%d/v1/auth" % frvr.port, "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off", "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH,
                "KRUNKNATIVE_DEBUG": "1"})
    p = subprocess.Popen(["xvfb-run", "-a", "stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(240)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client finished its script", False, "timed out; log " + log)
    return read_log(log)


def checks(text, stub, frvr):
    logins = [r for r in frvr.requests if r[0] == "/v1/auth/login"]
    check("one FRVR anonymous login: POST <auth>/login {\"platform\":\"anonymous\"}",
          len(logins) == 1 and logins[0][1] == {"platform": "anonymous"} and "authorization" not in logins[0][2],
          [(r[0], r[1]) for r in frvr.requests])
    seeks = stub.find("GET", "/mm/seek-game")
    check("seek-game carries the anonymous access token (v10's seek: getFreshAccessToken, :149727)",
          bool(seeks) and seeks[0][2].get("accessToken") == ANON_ACCESS, [("accessToken" in x[2]) for x in seeks][:2])
    check("the game socket's at= is the anonymous access token (:229296)",
          bool(stub.ws_paths) and ("at=" + ANON_ACCESS) in stub.ws_paths[0].replace("%2E", "."), [p.split("?")[0] for p in stub.ws_paths][:2])
    check("a guest with FRVR's anonymous session is not a krunker.io account (no \"signed in\")",
          "joining as a guest with FRVR's anonymous session" in text and "joining with the krunker.io account" not in text,
          re.findall(r"\[krunker\] joining.*", text)[:2])
    profiles = stub.find("GET", PROFILE)
    check("gapi GET /guest/profile with the anonymous session's Bearer (platform \"anonymous\", :230083)",
          bool(profiles) and all(r[3].get("authorization") == "Bearer " + ANON_ACCESS for r in profiles),
          [r[3].get("authorization", "")[:16] for r in profiles])
    check("the Unclaimed Rewards box shows the guest's 7 KR", "7" in text_of(text, "box1"), text_of(text, "box1") or "(no box)")
    check("GUEST_EARNED_REWARDS (\"ger\") asks /guest/profile again (:229362)", len(profiles) >= 2, len(profiles))
    check("the box then shows the new 12 KR", "12" in text_of(text, "box2"), text_of(text, "box2") or "(no box)")
    check("the anonymous tokens are never logged", ANON_ACCESS not in text and ANON_REFRESH not in text)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_guest_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port, frvr_port = kn_proto.free_ports(3)
    stub = kae.Stub(http_port, ws_port)
    stub.players = []
    frvr = Frvr(frvr_port)
    kae.GAPI_READS[PROFILE] = {"profile": {"funds": 7, "score": 900}}

    async def on_enter(send, raw):  # ENTER_GAME: the guest earned more; the server says so ("ger")
        kae.GAPI_READS[PROFILE] = {"profile": {"funds": 12, "score": 1500}}
        await send("ger")

    stub.frame_handlers["en"] = on_enter
    try:
        stub.start()
        frvr.start()
        text = run_client(stub, frvr)
        checks(text, stub, frvr)
    finally:
        stub.stop()
        frvr.stop()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
