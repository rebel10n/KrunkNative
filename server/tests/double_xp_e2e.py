#!/usr/bin/env python3
"""Double XP end to end on Krunker Box Office (v10 page) and on Krunker Strike (the Strike UI).

  python3 -I server/tests/double_xp_e2e.py BUILD_DIR [base|strike]     (run from the repository root)

A stand-in brain (this file: HTTP GET /mm/status, and the social socket /social/ws with the saved
session's token login and v10's claim: "rqdxp" answered by "rrqdxp") serves one account with games = 3,
the count v10 offers Double XP on (games % 3 == 0), whose dxp.offer is claimable. Nothing else is
contacted. A headless client_bin (--offline, one input script, the account signed in from the saved
session) then:
  1. plays a match in All or Nothing (KRUNKNATIVE_MODE=aon, a doubleXP mode) on the base page, so the
     in-game "2x XP" badge (#dxpHudBadge) shows; the end screen's Double XP button (v10 #doubleXPButton)
     is shown because the offer is claimable;
  2. presses it: no ad is shown, the client sends ["rqdxp"] once, the brain answers ["rrqdxp",
     {playerScore, xp, games}] and the XP Doubled! pop comes up; the offer is spent, the button goes.
The strike flavor runs the same account and offer with Krunker Strike's own markup: the lean end
screen's Double XP button (.lean-end-screen__double-reward, "Collected" once spent) and the "2x XP"
badge (.booster-time-left, FRVR's booster layout) in the doubleXP mode.
Prints PASS/FAIL lines; exit code 0 only when everything passed; 77 when the client is not built.
"""
import base64
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.getcwd(), "server", "tests"))
import kn_proto  # noqa: E402  (free_ports)

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
FLAVOR = sys.argv[2] if len(sys.argv) > 2 else "base"
REPO = os.getcwd()
WORK = tempfile.mkdtemp(prefix="kn-double-xp-e2e-", dir=os.path.expanduser("~/.cache"))
USER = "dxptester"
TOKEN = "dxp" + "7f3a9c1e5b8d2046af13c9e7d05b24817c6e0f9a3d5b8c2e4f6a1d9b7c3e5f20"
GAMES = 3                # the account's games: v10 offers Double XP when games % 3 == 0
OFFER_XP = 1234          # the offer's XP (the match's, before the doubling)
PLAYER_SCORE = 2468      # rrqdxp's playerScore: the doubled score
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


class Brain:
    """One account (its games and dxp.offer) on the social socket: the login and v10's claim."""

    def __init__(self, port):
        self.port = port
        self.claimable = True    # dxp.offer.claimable
        self.claims = []         # every "rqdxp" frame the client sent
        self.logins = 0
        self.lock = threading.Lock()
        self.server = None

    def account_data(self):
        data = [0] * 40
        data[3] = GAMES
        data[5] = 2500
        return data

    def account_info(self):
        offer = {"games": GAMES, "xp": OFFER_XP, "claimable": self.claimable} if self.claimable or GAMES else None
        return {"level": 1, "levelProg": 0, "alias": USER, "mail_unread": 0,
                "dxp": {"offer": offer, "doubleClassXP": False, "doubleRaidDrops": False}}

    def start(self):
        brain = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def reply(self, status, obj):
                body = json.dumps(obj).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                if path == "/mm/status":
                    return self.reply(200, {"brain": "double-xp-mock"})
                if path == "/social/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
                    return self.websocket()
                return self.reply(404, {"error": "not found"})

            def websocket(self):
                key = self.headers["Sec-WebSocket-Key"]
                accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
                self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
                self.wfile.flush()
                self.close_connection = True
                while True:
                    head = self.rfile.read(2)
                    if len(head) < 2:
                        return
                    opcode, masked, ln = head[0] & 0x0F, head[1] & 0x80, head[1] & 0x7F
                    if ln == 126:
                        ln = struct.unpack(">H", self.rfile.read(2))[0]
                    elif ln == 127:
                        ln = struct.unpack(">Q", self.rfile.read(8))[0]
                    mask = self.rfile.read(4) if masked else b"\0\0\0\0"
                    data = bytearray(self.rfile.read(ln))
                    for i in range(len(data)):
                        data[i] ^= mask[i % 4]
                    if opcode == 8:
                        return
                    if opcode == 9:
                        self.send_frame(10, bytes(data))
                        continue
                    if opcode != 1:
                        continue
                    try:
                        frame = json.loads(data.decode())
                    except ValueError:
                        continue
                    for reply in brain.social(frame):
                        self.send_frame(1, json.dumps(reply).encode())

            def send_frame(self, opcode, payload):
                n = len(payload)
                if n < 126:
                    head = bytes([0x80 | opcode, n])
                elif n < 65536:
                    head = bytes([0x80 | opcode, 126]) + struct.pack(">H", n)
                else:
                    head = bytes([0x80 | opcode, 127]) + struct.pack(">Q", n)
                self.wfile.write(head + payload)
                self.wfile.flush()

        self.server = ThreadingHTTPServer(("127.0.0.1", self.port), Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def social(self, frame):
        """The social frames that matter here: the saved session's token login ("a") and the claim ("rqdxp")."""
        if not isinstance(frame, list) or not frame:
            return []
        if frame[0] == "a":
            payload = frame[2] if len(frame) > 2 and isinstance(frame[2], list) else []
            if len(payload) < 3 or payload[0] != USER or payload[2] != TOKEN:
                return [["a", "Wrong username or password."]]
            self.logins += 1
            with self.lock:
                return [["a", None, 4242, USER, self.account_data(), TOKEN, self.account_info()]]
        if frame[0] == "rqdxp":
            with self.lock:
                self.claims.append(frame)
                if not self.claimable:
                    return [["rrqdxp", None, "Nothing to double"]]
                self.claimable = False
                return [["rrqdxp", {"playerScore": PLAYER_SCORE, "xp": OFFER_XP, "games": GAMES}]]
        return []

    def stop(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()


# The input scripts (times in ms from the client's start). Base: Box Office, the v10 page, in All or Nothing.
BASE_SCRIPT = """
0 menu
300 con bot_add 1
1000 check @menu doubleXPAd=1 menuHolder=1
1500 play
3500 check @alive timerHolder=1
3500 attr @alive-hud style [id=dxpHudBadge]
4000 con end
10000 check @endanim endUI=1 inGameUI=0
15000 check @results endUI=1 endTabbedView=1
15000 check @offer doubleXPButton=1
15000 nodes @button .doubleXPButton
15000 text @label .doubleXPButton
15100 clickid doubleXPButton
16000 nodes @popup .dxp-header
16000 text @popup-head .dxp-header
16000 check @claimed doubleXPButton=0
17000 quit
"""

# Strike: the same account and offer; the end screen is LEAN_END_SCREEN and its Double XP is the reward row's
# button (FRVR's lean end screen, a claimed offer reads Collected).
STRIKE_SCRIPT = """
0 menu
300 con bot_add 1
1000 check @menu dxpBadge=0
1500 play
3500 check @alive dxpBadge=0
4000 con end
10000 check @endanim leanEndScreen=1
15000 check @results leanEndScreen=1
15000 nodes @button .lean-end-screen__double-reward .frvr-button
15000 text @label .lean-end-screen__double-reward
15100 clicksel .lean-end-screen__double-reward .frvr-button
16000 nodes @spent .lean-end-screen__double-reward .frvr-button.is-disabled
16000 text @spent-label .lean-end-screen__double-reward
16000 text @popup-head .dxp-popup .frvr-window-title
17000 quit
"""


def expand(script):
    lines, expects = [], []
    for raw in script.strip().splitlines():
        t, cmd, *rest = raw.split()
        if cmd == "check":
            label = rest[0].lstrip("@")
            expects.append((label, dict(kv.split("=", 1) for kv in rest[1:])))
            lines.append(f"{t} vis @{label} " + " ".join(kv.split("=", 1)[0] for kv in rest[1:]))
        else:
            lines.append(raw)
    return "\n".join(lines) + "\n", expects


def run_client(brain_port, script_text):
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for k, v in (("krunker_username", USER), ("krunker_token", TOKEN)):
        with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
            f.write(v)
    path = os.path.join(WORK, "double_xp.script")
    with open(path, "w") as f:
        f.write(script_text)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_MODE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_FIXED_DELTA": "16.6667",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSETS_DIR": os.path.join(REPO, "assets"),
                "KRUNKNATIVE_BRAIN_URL": f"http://127.0.0.1:{brain_port}"})
    args = ["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "--map", "kanji"]
    if FLAVOR == "strike":
        args += ["--flavor", "strike"]
    else:
        env["KRUNKNATIVE_MODE"] = "aon"  # All or Nothing: mode.doubleXP, the flag the badge follows
    log = os.path.join(WORK, "client.log")
    with open(log, "w") as out:
        p = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                             start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read().splitlines(), log


def parse(lines):
    got, nodes, texts, attrs = {}, {}, {}, {}
    for line in lines:
        m = re.search(r"\[input_script\] vis t=\d+ @(\S+)(.*)", line)
        if m:
            got.setdefault(m.group(1), {}).update(kv.split("=", 1) for kv in m.group(2).split())
        m = re.search(r"\[input_script\] nodes t=\d+ @?(\S+) elements=\d+ match=(\d+)", line)
        if m:
            nodes[m.group(1)] = int(m.group(2))
        m = re.search(r"\[input_script\] text t=\d+ @?(\S+) n=(\d+) checked=\d+ value=.*? text=(.*)$", line)
        if m:
            texts[m.group(1)] = (int(m.group(2)), m.group(3))
        m = re.search(r"\[input_script\] attr t=\d+ @?(\S+) n=\d+ (\S+)=(.*)$", line)
        if m:
            attrs[m.group(1)] = m.group(3)
    return got, nodes, texts, attrs


def run():
    text, expects = expand(BASE_SCRIPT if FLAVOR == "base" else STRIKE_SCRIPT)
    port = kn_proto.free_ports(1)[0]
    brain = Brain(port)
    brain.start()
    try:
        lines, log = run_client(port, text)
    finally:
        brain.stop()
    check("the client loaded the page and played the script", any("[input_script] vis" in l for l in lines), log)
    check("the stand-in brain logged the saved session in over the social socket", brain.logins >= 1, f"logins {brain.logins}")
    check("no rewarded ad is shown for the claim (v10: the claim is sent at once)",
          not any("[ad]" in l for l in lines))
    check("exactly one claim: [\"rqdxp\"] on the social socket", brain.claims == [["rqdxp"]], repr(brain.claims))
    check("the client took the doubled score (rrqdxp's playerScore)",
          any(f"[double_xp] doubled: score {PLAYER_SCORE}" in l for l in lines))
    check("the brain spent the offer for the match", brain.claimable is False)

    got, nodes, texts, attrs = parse(lines)
    for label, want in expects:
        have = got.get(label, {})
        bad = {k: have.get(k, "no probe") for k, v in want.items() if have.get(k) != v}
        check(f"{label}: " + " ".join(f"{k}={v}" for k, v in want.items()), not bad, f"got {bad}" if bad else "")

    if FLAVOR == "base":
        check("the Double XP button is on the end screen (claimable)", nodes.get("button") == 1, f"{nodes.get('button')}")
        label = texts.get("label", (0, ""))[1]
        check("its label is Double XP (v10 window.endgame.player.doubleGainedXP)", label == "Double XP", repr(label))
        head = texts.get("popup-head", (0, ""))[1]
        check("the XP Doubled! pop is up after the claim", nodes.get("popup") == 1 and head == "XP Doubled!", f"{nodes.get('popup')} {head!r}")
        style = attrs.get("alive-hud", "-")
        check("the in-game 2x XP badge shows in All or Nothing (mode.doubleXP)", "display: block" in style or "display:block" in style, repr(style))
    else:
        check("the Double XP button is in the lean end screen's reward row", nodes.get("button") == 1, f"{nodes.get('button')}")
        label = texts.get("label", (0, ""))[1]
        check("the Strike button reads Double XP", label == "Double XP", repr(label))
        spent_label = texts.get("spent-label", (0, ""))[1]
        check("the button is spent after the claim (is-disabled, Collected)",
              nodes.get("spent") == 1 and spent_label == "Collected", f"{nodes.get('spent')} {spent_label!r}")
        head = texts.get("popup-head", (0, ""))[1]
        check("the XP Doubled! popup is up after the claim", head == "XP Doubled!", repr(head))
    return all(results)


def main():
    if not os.path.exists(os.path.join(BUILD, "client_bin")):
        print("double_xp_e2e: client_bin not built: skipped")
        return 77
    try:
        ok = run()
    finally:
        shutil.rmtree(os.path.join(WORK, "cfg"), ignore_errors=True)
    print(f"\n{sum(results)}/{len(results)} passed  (log: {os.path.join(WORK, 'client.log')})")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
