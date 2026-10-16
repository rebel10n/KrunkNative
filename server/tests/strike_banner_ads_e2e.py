#!/usr/bin/env python3
"""Krunker Strike's banner ad slot on the round end (client/src/ui/strike_banner_ad.c), end to end.

  python3 -I server/tests/strike_banner_ads_e2e.py BUILD_DIR      (run from the repository root)

FRVR 1.37.2 shows a bottom banner on the lean end screen when its ads are supported; Krunker Box Office's
placeholder ad always is, so the slot is always there. Its click is the match's Double XP claim
(double_xp_offer.h): double_xp_claim sends ["rqdxp"] on the social socket at once (no ad), and the answer
["rrqdxp", {playerScore, xp, games}] doubles the match. A stand-in brain serves one account per run over ws /social/ws (the saved session's token login, ["rqdxp"], and the account's "dxp" object:
offer {games, xp, claimable} and the weekend schedules); nothing else is contacted. A headless Strike client
(--offline --flavor strike, one input script) then plays a match and ends it, per run:
  offer: signed in, games % 3 == 0 and the offer claimable:
    1. the slot is in the lean end screen's bottom strip, and its status is empty before any click;
    2. the first click sends one rqdxp (dxp-client's claim has no ad step); the status reads "2x XP"
       and the button reads Collected;
    3. the second click sends nothing.
  no offer: signed in, games % 3 != 0 (the offer is not claimable):
    4. the clicks watch no ad and send no rqdxp; the status says the match has no Double XP, and the
       button is dimmed.
  guest (no saved session):
    5. the clicks watch no ad and send nothing; the status asks to sign in.
The account's state (games, schedules) is set before the run: the offer comes with the login's payload
(the brain's commit path is not run offline). The retired HTTP grant route is gone; no
request goes to it.
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
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.getcwd(), "server", "tests"))
import kn_proto  # noqa: E402  (free_ports)

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
WORK = tempfile.mkdtemp(prefix="kn-strike-banner-e2e-", dir=os.path.expanduser("~/.cache"))
USER = "bannertester"
TOKEN = "ban" + "5c2e8a1f7d3b9046ce21a8f5d0b37941e6c4a2f8d9b0e3c7a5f1d6b2e9c4a803"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OFFER_XP = 1200  # the match's XP: the claim credits it once more
SCORE = 5400     # the account's score before the claim (playerScore = SCORE + OFFER_XP)
SIGN_IN = "Sign in to claim Double XP."
NO_OFFER = "No Double XP for this match."
ALL_ON = "2x XP"
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


class Brain:
    """One account for one run: its games count and weekend schedules, and the claim as dxp's brain answers it."""

    def __init__(self, port):
        self.port = port
        self.games = 0
        self.class_xp = False
        self.raid_drops = False
        self.claimed_games = -1
        self.logged_in = False
        self.logins = 0
        self.claims = []  # the rqdxp frames received in this run
        self.lock = threading.Lock()
        self.server = None

    def configure(self, games, class_xp, raid_drops):
        with self.lock:
            self.games, self.class_xp, self.raid_drops = games, class_xp, raid_drops
            self.claimed_games = -1
            self.logged_in = False
            self.logins = 0
            self.claims = []

    def claimable(self):
        """The brain's dxp_offer_claimable: the account's last match, games % 3 == 0 above zero, not claimed yet."""
        return self.games > 0 and self.games % 3 == 0 and self.claimed_games != self.games

    def account_info(self):
        return {"level": 1, "levelProg": 0, "alias": USER, "mail_unread": 0,
                "dxp": {"offer": {"games": self.games, "xp": OFFER_XP, "claimable": self.claimable()},
                        "doubleClassXP": self.class_xp, "doubleRaidDrops": self.raid_drops}}

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
                    return self.reply(200, {"brain": "banner-ads-mock"})
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
        """The social frames the client sends that matter here: the token login and the Double XP claim."""
        if not isinstance(frame, list) or not frame:
            return []
        if frame[0] == "rqdxp":
            with self.lock:
                self.claims.append(frame)
                if not self.logged_in:
                    return [["rrqdxp", None, "Must be logged in"]]
                if not self.claimable():
                    return [["rrqdxp", None, "No double XP for this match"]]
                self.claimed_games = self.games
                return [["rrqdxp", {"playerScore": SCORE + OFFER_XP, "xp": OFFER_XP, "games": self.games}]]
        if frame[0] != "a":
            return []
        payload = frame[2] if len(frame) > 2 and isinstance(frame[2], list) else []
        if len(payload) < 3 or payload[0] != USER or payload[2] != TOKEN:
            return [["a", "Wrong username or password."]]
        with self.lock:
            self.logged_in = True
            self.logins += 1
            info = self.account_info()
        return [["a", None, 4242, USER, [], TOKEN, info]]

    def stop(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()


# The input script (times in ms from the client's start; the round ends at 4 s with the bot's con end).
SCRIPT = """
0 menu
300 con bot_add 1
1500 play
4000 con end
4000 waitfor .frvr-in-game-ui__victory-screen
4000 nodes @banner .frvr-in-game-ui__victory-screen
4000 waitfor [id=leanEndScreen]
4000 check @endanim leanEndScreen=1
4000 nodes @banner-after .frvr-in-game-ui__victory-screen
15000 check @results leanEndScreen=1 strikeBannerAd=1
15000 nodes @slot .lean-end-screen__bottom .strike-banner-ad
15000 rects @box .lean-end-screen__bottom-buttons .frvr-button
15000 rects @strip .strike-banner-ad
15000 text @label .strike-banner-ad__label
15000 text @before .strike-banner-ad__status
15000 nodes @dim-before .strike-banner-ad__button--off
15100 clicksel .strike-banner-ad
16500 text @status .strike-banner-ad__status
16500 text @button .strike-banner-ad__button
16500 nodes @dim .strike-banner-ad__button--off
16600 clicksel .strike-banner-ad
17500 text @again .strike-banner-ad__status
17600 quit
"""

# (run, signed in, games, weekend class XP, weekend raid drops)
RUNS = [
    ("offer", True, 3, True, True),
    ("no-offer", True, 4, False, False),
    ("guest", False, 3, True, True),
]


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


def run_client(brain_port, script_text, signed_in, tag):
    cfg = os.path.join(WORK, tag, "cfg")
    # the Strike flavor's own config directory (flavor_storage_subdir: "krunknative-strike", not Box Office's
    # "krunknative"): the --flavor strike client reads its sign-in there
    folder = os.path.join(cfg, "krunknative-strike")
    os.makedirs(folder, exist_ok=True)
    # a returning player (the FTUE gate, client/src/ui/strike_ftue.c strike_ftue_first_run): a first run quick
    # connects to the controls tutorial's sandbox instead of the lobby, and its tap blur takes the clicks
    with open(os.path.join(folder, "knFtue.json"), "w") as f:
        json.dump({name: {"played": True} for name in ("guest", USER)}, f)
    if signed_in:
        for k, v in (("krunker_username", USER), ("krunker_token", TOKEN)):
            with open(os.path.join(folder, k + ".json"), "w") as f:
                f.write(v)
    path = os.path.join(WORK, tag, "banner.script")
    with open(path, "w") as f:
        f.write(script_text)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, tag, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_FIXED_DELTA": "16.6667",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSETS_DIR": os.environ.get("KRUNKNATIVE_ASSETS_DIR", os.path.join(REPO, "assets")),
                "KRUNKNATIVE_BRAIN_URL": f"http://127.0.0.1:{brain_port}"})
    args = ["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "--map", "kanji", "--flavor", "strike"]
    log = os.path.join(WORK, tag, "client.log")
    with open(log, "w") as out:
        p = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                             start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{tag}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read().splitlines(), log


def parse(lines):
    got, nodes, texts = {}, {}, {}
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
    return got, nodes, texts


def rects_of(lines, label):
    """The first box x, y, w, h of a rects probe (window coordinates: "-@x,y,w,h"), or None."""
    for line in lines:
        m = re.search(r"\[input_script\] rects t=\d+ @?" + label + r" n=\d+ \S*?@(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)", line)
        if m:
            return [float(m.group(i)) for i in range(1, 5)]
    return None


def check_slot_geometry(lines, nodes, tag):
    check(f"{tag}: the banner slot sits in the lean end screen's bottom strip (one slot)", nodes.get("slot") == 1,
          f"{nodes.get('slot')}")
    box = rects_of(lines, "box")
    strip = rects_of(lines, "strip")
    check(f"{tag}: the slot sits under the bottom row, inside the 1280x720 window",
          box and strip and abs(strip[1] - (box[1] + box[3])) <= 2 and strip[1] + strip[3] <= 720 and strip[2] > 0 and strip[3] > 0,
          f"row {box} slot {strip}")


def check_probes(tag, expects, got):
    for label, want in expects:
        have = got.get(label, {})
        bad = {k: have.get(k, "no probe") for k, v in want.items() if have.get(k) != v}
        check(f"{tag}: {label}: " + " ".join(f"{k}={v}" for k, v in want.items()), not bad, f"got {bad}" if bad else "")


# the lean screen's poll (waitfor [id=leanEndScreen]) must end within this many wall seconds of the banner showing:
# the endAnim hold is 2.5 s, a loaded host runs slower frames, so 15 s is the ceiling
WAIT_LEAN_MAX_S = 15.0


def line_index(lines, pattern):
    for i, line in enumerate(lines):
        if re.search(pattern, line):
            return i
    return -1


def check_hold(tag, lines, nodes):
    """The round-end banner (endAnim, 2.5 s) comes first and the lean screen after it, and the lean screen's wait is a
    poll that ends within WAIT_LEAN_MAX_S: the banner is sampled as it first shows, sampled again once the lean
    screen is up (gone), and the logged poll time (wall seconds) is checked."""
    i_banner = line_index(lines, r"\[input_script\] nodes t=\d+ @banner elements=")
    i_lean = line_index(lines, r"\[input_script\] vis t=\d+ @endanim ")
    check(f"{tag}: endanim: the banner shows first, then the lean screen replaces it (gone once it is up)",
          nodes.get("banner") == 1 and nodes.get("banner-after") == 0 and 0 <= i_banner < i_lean,
          f"banner {nodes.get('banner')} after {nodes.get('banner-after')} order {i_banner} < {i_lean}")
    waited = [float(m.group(1)) for line in lines
              for m in [re.search(r"waitfor done \[id=leanEndScreen\] after ([\d.]+) s", line)] if m]
    check(f"{tag}: endanim: the lean screen comes within {WAIT_LEAN_MAX_S:.0f} s of the banner (poll)",
          bool(waited) and waited[0] <= WAIT_LEAN_MAX_S, f"waited {waited}")


def run():
    text, expects = expand(SCRIPT)
    port = kn_proto.free_ports(1)[0]
    brain = Brain(port)
    brain.start()
    runs = {}
    try:
        for tag, signed_in, games, class_xp, raid in RUNS:
            brain.configure(games, class_xp, raid)
            lines, log = run_client(port, text, signed_in, tag)
            runs[tag] = {"lines": lines, "log": log, "claims": list(brain.claims), "logins": brain.logins,
                         "signed_in": signed_in}
    finally:
        brain.stop()

    # ---- offer: signed in, games % 3 == 0, the offer claimable
    r = runs["offer"]
    lines = r["lines"]
    check("offer: the client played the script", any("[input_script] vis" in l for l in lines), r["log"])
    check("offer: the stand-in brain answered the saved session's token login", r["logins"] >= 1, f"logins {r['logins']}")
    got, nodes, texts = parse(lines)
    check_probes("offer", expects, got)
    check_hold("offer", lines, nodes)
    check_slot_geometry(lines, nodes, "offer")
    check("offer: the slot's label names Double XP", "Double XP" in texts.get("label", (0, ""))[1], repr(texts.get("label")))
    check("offer: before any click the status is empty (no buff, no schedule line yet)",
          texts.get("before", (0, ""))[1] == "", repr(texts.get("before")))
    check("offer: the button is not dimmed before the click", nodes.get("dim-before") == 0, f"{nodes.get('dim-before')}")
    ads = [l for l in lines if "[ad] placeholder" in l]
    check("offer: no rewarded ad is shown (the claim is sent at once, dxp-client has no ad step)", len(ads) == 0, f"{len(ads)} ad lines")
    check("offer: one rqdxp over the social socket, the first click's, nothing else",
          r["claims"] == [["rqdxp"]], repr(r["claims"]))
    check("offer: the claim is granted (the match doubled)", any("[strike] banner ad: double XP granted" in l for l in lines))
    check("offer: the status reads 2x XP after the claim",
          texts.get("status", (0, ""))[1] == ALL_ON, repr(texts.get("status")))
    check("offer: the button reads Collected after the claim", texts.get("button", (0, ""))[1] == "Collected", repr(texts.get("button")))
    check("offer: the spent button is dimmed", nodes.get("dim") == 1, f"{nodes.get('dim')}")
    check("offer: the second click sends nothing (no rqdxp, no ad)",
          r["claims"] == [["rqdxp"]] and len(ads) == 0 and any("[strike] banner ad: double XP not offered" in l for l in lines))
    check("offer: the status after the second click is unchanged", texts.get("again", (0, ""))[1] == ALL_ON, repr(texts.get("again")))

    # ---- no offer: signed in, games % 3 != 0
    r = runs["no-offer"]
    lines = r["lines"]
    check("no-offer: the client played the script", any("[input_script] vis" in l for l in lines), r["log"])
    got, nodes, texts = parse(lines)
    check_probes("no-offer", expects, got)
    check_hold("no-offer", lines, nodes)
    check_slot_geometry(lines, nodes, "no-offer")
    check("no-offer: before any click the status says the match has no Double XP",
          texts.get("before", (0, ""))[1] == NO_OFFER, repr(texts.get("before")))
    check("no-offer: the button is dimmed", nodes.get("dim-before") == 1 and nodes.get("dim") == 1,
          f"before {nodes.get('dim-before')} after {nodes.get('dim')}")
    check("no-offer: the clicks watch no ad", not [l for l in lines if "[ad] placeholder" in l],
          f"{len([l for l in lines if '[ad] placeholder' in l])} ad lines")
    check("no-offer: no rqdxp is sent", r["claims"] == [], repr(r["claims"]))
    check("no-offer: the status says the match has no Double XP after the clicks",
          texts.get("status", (0, ""))[1] == NO_OFFER and texts.get("again", (0, ""))[1] == NO_OFFER,
          repr((texts.get("status"), texts.get("again"))))
    check("no-offer: the button still reads Double XP (not Collected)", texts.get("button", (0, ""))[1] == "Double XP",
          repr(texts.get("button")))

    # ---- guest: no saved session
    r = runs["guest"]
    lines = r["lines"]
    check("guest: the client played the script", any("[input_script] vis" in l for l in lines), r["log"])
    got, nodes, texts = parse(lines)
    check_probes("guest", expects, got)
    check_hold("guest", lines, nodes)
    check_slot_geometry(lines, nodes, "guest")
    check("guest: the slot asks to sign in, before and after the click",
          texts.get("before", (0, ""))[1] == SIGN_IN and texts.get("status", (0, ""))[1] == SIGN_IN
          and texts.get("again", (0, ""))[1] == SIGN_IN, repr((texts.get("before"), texts.get("status"))))
    check("guest: no ad is watched", not [l for l in lines if "[ad] placeholder" in l])
    check("guest: no rqdxp is sent", r["claims"] == [], repr(r["claims"]))
    check("guest: the click reports the guest result", any("[strike] banner ad: double XP guest" in l for l in lines))
    return all(results)


def main():
    if not os.path.exists(os.path.join(BUILD, "client_bin")):
        print("strike_banner_ads_e2e: client_bin not built: skipped")
        return 77
    ok = run()
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    print(f"\n{sum(results)}/{len(results)} passed  (logs: {WORK})")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
