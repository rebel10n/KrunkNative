#!/usr/bin/env python3
"""Bot chat on a dedicated server (shared/src/bot_chat.c, server/src/bot_banter.c, kn_brain's `bot-banter`
request kind in services/brain/ai.c).

  python3 -I server/tests/bots_chat_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a sqlite scratch database (services/brain/tests/db_scratch.py) with a STUBBED AI gateway
(services/brain/tests/ai_stub.py: never the live provider), one brain-linked server_bin keeping 5 bots, then
  1. a guest alone: match ends still get bot "gg" lines (templates); the stub sees no bot-banter request
  2. a signed-in player joins: over the match ends, at most 2 bot lines per end, on distinct delays, not every
     end chatty and not every end silent; the AI's lines reach the chat (the stub's marker); the requests the
     stub saw are non-streamed, capped, and carry the tight prompt
  3. the stub answers with profanity and extra lines: no such line ever reaches the chat (template fallback),
     every line <= 70 characters
  4. kill / death comments while the bots fight: never closer than the global cooldown
  5. the stub gateway goes down: the lines keep coming (templates), no bot line is lost to the error
  6. a standalone server (no brain): template lines with `bot_chat true`; with no bot_chat key (the default is
     off) and with `bot_chat false` the bots never chat
Prints PASS/FAIL lines; exit code 0 only when everything passed. Exit 77 when sqlite is unavailable.
"""
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, "server/tests"))
sys.path.insert(0, os.path.join(ROOT, "services/brain/tests"))
import kn_proto  # noqa: E402
from ai_stub import Upstream  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
GAME_PORT, RCON_PORT, GAME2_PORT, RCON2_PORT = kn_proto.free_ports(4)  # ephemeral: concurrent runs must not share
WORK = tempfile.mkdtemp(prefix="kn-bots-chat-e2e-", dir=os.path.join(os.path.expanduser("~"), ".cache"))
SECRET = "kn-bots-chat-%d" % os.getpid()
KEY = "sk-stub-%d-SECRET" % os.getpid()
SPAWN, CHAT, END = 4, 9, 16
NAME_SIZE, TEXT_SIZE = 32, 288
PROFANE = ("fuck", "shit", "bitch")
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
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
        time.sleep(0.2)
    return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {}, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


class BanterStub(Upstream):
    """The AI gateway: bot-banter prompts (the tight system prompt names Krunker) get a marked line, or, in
    dirty mode, profanity plus a second line the filter must stop. Everything else is the stock stub."""

    def __init__(self, key):
        super().__init__(key)
        self.dirty = False
        self.banter = 0
        self.banter_bodies = []

    def plan(self, body):
        msgs = body.get("messages", [])
        sysm = msgs[0].get("content", "") if msgs and msgs[0].get("role") == "system" else ""
        if "Krunker" in sysm and "bot player" in sysm:
            with self.lock:
                self.banter += 1
                n = self.banter
                self.banter_bodies.append(body)
                dirty = self.dirty
            if dirty:
                return ("text", None, "what the fuck gg lol\nand a second line that must never show")
            return ("text", None, "KNSTUB%d gg that was wild" % n)
        return super().plan(body)


# ------------------------------------------------------------------ the game socket

class Watcher:
    """A client on the game socket that keeps every bot chat line with the wall time it arrived."""

    def __init__(self, port, name, session=""):
        self.c = kn_proto.Conn("127.0.0.1", port)
        self.c.hello()
        self.c.join(name=name, session=session)
        self.c.ent(class_index=0, lag_comp=1.0)
        self.uid = None
        self.humans = set()
        self.lines = []  # (t, uid, name, text)
        self.seen = 0
        end = time.time() + 8
        while time.time() < end and self.uid is None:
            self.pump(0.5)
        self.humans.add(self.uid)

    def pump(self, seconds):
        """Reads for `seconds`, folding new packets into lines / spawns."""
        end = time.time() + seconds
        while True:
            left = end - time.time()
            self.c.read(timeout=max(0.05, min(0.2, left)) if left > 0 else 0.05)
            now = time.time()
            for t, p in self.c.packets[self.seen:]:
                if t == SPAWN:
                    info = kn_proto.spawn_info(p)
                    if info and info["is_you"]:
                        self.uid = info["uid"]
                elif t == CHAT and len(p) >= 5 + NAME_SIZE + TEXT_SIZE:
                    uid = struct.unpack("<i", p[:4])[0]
                    kind = p[4]
                    name = kn_proto.uncstr(p[5:5 + NAME_SIZE])
                    text = kn_proto.uncstr(p[5 + NAME_SIZE:5 + NAME_SIZE + TEXT_SIZE])
                    if kind == 0 and uid not in self.humans:
                        self.lines.append((now, uid, name, text))
            self.seen = len(self.c.packets)
            if left <= 0:
                break

    def wait_packet(self, ptype, timeout):
        end = time.time() + timeout
        while time.time() < end:
            before = self.seen
            self.pump(0.3)
            if any(t == ptype for t, _ in self.c.packets[before:self.seen]):
                return True
        return False

    def close(self):
        self.c.close()


def rcon(port, *cmd):
    out = subprocess.run([f"{BUILD}/server_bin", "--rcon", f"127.0.0.1:{port}", "--rcon-password", "rc-pw", *cmd],
                         capture_output=True, text=True, timeout=10)
    return out.returncode, out.stdout + out.stderr


def respawn(w):
    """After a map change: back in the game (ENT again when the server left us in the menu). Waits for OUR
    SPAWN (is_you): the bots' SPAWNs of the new round arrive either way."""
    def own_spawn(timeout):
        end = time.time() + timeout
        while time.time() < end:
            before = w.seen
            w.pump(0.3)
            for t, p in w.c.packets[before:w.seen]:
                if t == SPAWN and (kn_proto.spawn_info(p) or {}).get("is_you"):
                    return True
        return False
    if not own_spawn(3):
        w.c.ent(class_index=0, lag_comp=1.0)
        if not own_spawn(8):
            check("respawned after the map change", False)


def match_end(w, rcon_port, screen=8.5, others=()):
    """Ends the running round (`end`: 3 s), watches the end screen for `screen` s, starts the next round
    (`map`). Every watcher in `others` is kept drained and respawned too (a signed-in player in the menu is
    not in the game, so bot_banter_request would rightly fall back to templates). Returns the bot lines of
    the end screen as `w` saw them [(t, uid, name, text)]."""
    rcon(rcon_port, "end")
    if not w.wait_packet(END, 10):
        check("round ended (END packet)", False)
        return []
    start = len(w.lines)
    t_end = time.time()
    end = t_end + screen
    while time.time() < end:
        w.pump(min(0.5, max(0.05, end - time.time())))
        for o in others:
            o.pump(0.05)
    lines = [l for l in w.lines[start:] if l[0] >= t_end - 0.05]
    rcon(rcon_port, "map", "Sandstorm")
    for x in (w, *others):
        respawn(x)
    w.pump(0.5)
    return lines


def end_lines_ok(tag, lines):
    check(f"{tag}: at most 2 bot lines ({len(lines)})", len(lines) <= 2, " | ".join(f"{n}: {t}" for _, _, n, t in lines))
    if len(lines) == 2:
        check(f"{tag}: two bots, distinct delays", lines[0][1] != lines[1][1] and abs(lines[1][0] - lines[0][0]) >= 0.5,
              "%.2f s apart" % abs(lines[1][0] - lines[0][0]))
    for _, _, _, text in lines:
        check(f"{tag}: line fits and is clean ('{text}')", 0 < len(text) <= 70 and not any(b in text.lower() for b in PROFANE))


def streak_ok(tag, chatty):
    """chatty: [bool] per match end; the streak bounds of bot_chat.c (3 chatty / 2 silent in a row)."""
    s = "".join("C" if c else "s" for c in chatty)
    check(f"{tag}: not every match end chatty ({s})", not all(chatty) or len(chatty) < 4)
    check(f"{tag}: not every match end silent ({s})", any(chatty) or len(chatty) < 3)
    check(f"{tag}: no 4 chatty in a row", "CCCC" not in s)
    check(f"{tag}: no 3 silent in a row", "sss" not in s)


def start_server(path_cfg, log_path, env):
    out = open(log_path, "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", path_cfg], cwd=ROOT, env=env, stdout=out,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    return json.loads(data[11:])


# ------------------------------------------------------------------ scenarios

def brain_linked_scenario(up, brain_port, env):
    gs = os.path.join(WORK, "gs.json")
    with open(gs, "w") as f:
        json.dump({"gameserver": {"node_id": "BC", "name": "bots chat e2e", "region": "local", "listen_port": GAME_PORT,
                                  "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
                                  "brain_secret": SECRET, "modes": ["ffa"], "maps": ["Sandstorm"], "max_players": 8,
                                  "round_seconds": 600, "bots": 5, "bot_chat": True, "bot_chat_ai": True,
                                  "rcon_host": "127.0.0.1", "rcon_port": RCON_PORT, "rcon_password": "rc-pw"}}, f)
    log = os.path.join(WORK, "gs.log")
    start_server(gs, log, env)
    check("server linked to the brain", wait_for(lambda: "welcomed" in read_log(log), 25), read_log(log)[-300:])
    info = wait_for(lambda: (lambda i: i if i.get("bots", 0) >= 5 else None)(query(GAME_PORT)), 20)
    check("5 bots in the lobby", bool(info), str(info)[:200])

    # 1. a guest alone: templates only
    guest = Watcher(GAME_PORT, "Guest_e2e")
    check("guest spawned", guest.uid is not None)
    guest.pump(1.0)
    chatty = []
    for i in range(3):
        lines = match_end(guest, RCON_PORT)
        end_lines_ok("guest end %d" % i, lines)
        chatty.append(bool(lines))
    check("guests only: the stub saw no bot-banter request (%d)" % up.banter, up.banter == 0)
    check("guests only: templated gg lines still come (%s)" % "".join("C" if c else "s" for c in chatty), any(chatty))
    gg = [t for c in [guest.lines] for _, _, _, t in c if t.lower().startswith("gg")]
    check("plain gg lines seen (%d)" % len(gg), len(gg) >= 1, " | ".join(gg[:4]))

    # 2. a signed-in player: the AI writes lines through the brain
    st, reg = http(brain_port, "/auth/register", {"username": "botchat%d" % (os.getpid() % 100000), "password": "pw-botchat-%d" % os.getpid()})
    token = reg.get("token", "") if st == 200 and isinstance(reg, dict) else ""
    check("account registered", bool(token), str(reg)[:200])
    acct = Watcher(GAME_PORT, "", session=token)
    check("account player spawned", acct.uid is not None)
    guest.humans.add(acct.uid)
    acct.humans.add(guest.uid)
    guest.pump(1.0)
    chatty, ai_seen, all_lines = [], 0, []
    for i in range(10):
        lines = match_end(guest, RCON_PORT, others=(acct,))
        end_lines_ok("end %d" % i, lines)
        chatty.append(bool(lines))
        all_lines += lines
        ai_seen += sum(1 for _, _, _, t in lines if t.startswith("KNSTUB"))
        if i >= 5 and ai_seen:
            break
    streak_ok("signed-in", chatty)
    check("AI lines reached the chat (%d of %d lines, %d stub requests)" % (ai_seen, len(all_lines), up.banter), ai_seen >= 1,
          " | ".join(t for _, _, _, t in all_lines))
    check("the AI was asked (%d requests)" % up.banter, up.banter >= 1)
    check("every AI line came from a stub answer", all(int(t.split()[0][6:]) <= up.banter for _, _, _, t in all_lines if t.startswith("KNSTUB")))
    bodies = list(up.banter_bodies)
    check("bot-banter requests are non-streamed and capped",
          bodies and all(not b.get("stream") and 0 < b.get("max_tokens", 0) <= 400 for b in bodies),
          str([(b.get("stream"), b.get("max_tokens")) for b in bodies[:3]]))
    check("bot-banter prompt: system + one user turn, the event in it",
          bodies and all(len(b["messages"]) == 2 and b["messages"][1]["role"] == "user" and "Event:" in b["messages"][1]["content"]
                         and "at most 60 characters" in b["messages"][0]["content"] for b in bodies),
          str(bodies[0]["messages"])[:300] if bodies else "")
    hdr = {k.lower(): v for k, v in (up.chat_requests()[-1]["headers"] if up.chat_requests() else {}).items()}
    check("the brain's key reached the stub, never the game server's config", hdr.get("authorization") == "Bearer " + KEY)
    gs_log = read_log(log)
    check("the key is not in the game server log", KEY not in gs_log)

    # 3. profane / multi-line answers: the filter keeps them out, the template goes instead
    up.dirty = True
    before = up.banter
    chatty, dirty_lines = [], []
    for i in range(8):
        lines = match_end(guest, RCON_PORT, others=(acct,))
        end_lines_ok("dirty end %d" % i, lines)
        chatty.append(bool(lines))
        dirty_lines += lines
        if i >= 3 and up.banter > before:
            break
    check("dirty phase: lines still come (%s)" % "".join("C" if c else "s" for c in chatty), any(chatty))
    check("dirty phase: no profanity, no second line, nothing from the stub (%d dirty answers)" % (up.banter - before),
          not any(any(b in t.lower() for b in PROFANE) or "never show" in t or t.startswith("KNSTUB") for _, _, _, t in dirty_lines),
          " | ".join(t for _, _, _, t in dirty_lines))
    check("dirty phase: the stub was asked and answered dirty (%d)" % (up.banter - before), up.banter > before)
    up.dirty = False

    # 4. kill / death comments during a round: the global cooldown
    start = len(guest.lines)
    for _ in range(90):
        guest.pump(0.45)
        acct.pump(0.05)
    kills = guest.lines[start:]
    gaps = [b[0] - a[0] for a, b in zip(kills, kills[1:])]
    check("kill comments spaced by the cooldown (%d lines in 45 s, gaps %s)" % (len(kills), ["%.1f" % g for g in gaps]),
          all(g >= 7.0 for g in gaps) and len(kills) <= 6)
    for _, _, _, text in kills:
        check("kill comment clean ('%s')" % text, 0 < len(text) <= 70 and not any(b in text.lower() for b in PROFANE))

    # 5. the gateway goes down: templates, nothing lost
    up.stop()
    time.sleep(1.0)
    before = up.banter
    chatty, down_lines = [], []
    for i in range(3):
        lines = match_end(guest, RCON_PORT, others=(acct,))
        end_lines_ok("gateway down end %d" % i, lines)
        chatty.append(bool(lines))
        down_lines += lines
    check("gateway down: lines still come (%s)" % "".join("C" if c else "s" for c in chatty), any(chatty))
    check("gateway down: no AI line, no new stub request", not any(t.startswith("KNSTUB") for _, _, _, t in down_lines) and up.banter == before)
    failed = re.findall(r"\[bot-chat\] AI line \d+ failed", read_log(log))
    print("      (server logged %d failed AI lines -> templates)" % len(failed))
    acct.close()
    guest.close()


def standalone_scenario(env):
    # default (no bot_chat key: off), bot_chat true (on: the lines come back), bot_chat false (off)
    for label, extra, shown in (("default", {}, "bot_chat unset (default off)"), ("on", {"bot_chat": True}, "bot_chat true"),
                                ("off", {"bot_chat": False}, "bot_chat false")):
        enabled = label == "on"
        gs = os.path.join(WORK, "gs2-%s.json" % label)
        with open(gs, "w") as f:
            json.dump({"gameserver": dict({"name": "bots chat standalone", "region": "local", "listen_port": GAME2_PORT,
                                           "modes": ["ffa"], "maps": ["Sandstorm"], "max_players": 8, "round_seconds": 600,
                                           "bots": 4, "rcon_host": "127.0.0.1", "rcon_port": RCON2_PORT,
                                           "rcon_password": "rc-pw"}, **extra)}, f)
        log = os.path.join(WORK, "gs2-%s.log" % label)
        p = start_server(gs, log, env)
        info = wait_for(lambda: (lambda i: i if i.get("bots", 0) >= 4 else None)(query(GAME2_PORT)), 20)
        check("standalone server (%s) up with 4 bots" % shown, bool(info), str(info)[:200])
        w = Watcher(GAME2_PORT, "Guest_sa")
        check("guest spawned (standalone, %s)" % label, w.uid is not None)
        w.pump(1.0)
        chatty, lines = [], []
        for i in range(3 if enabled else 4):
            got = match_end(w, RCON2_PORT)
            chatty.append(bool(got))
            lines += got
            if enabled:
                end_lines_ok("standalone end %d" % i, got)
        if enabled:
            check("standalone: templated lines without a brain (%s)" % "".join("C" if c else "s" for c in chatty), any(chatty))
        else:
            w.pump(10.0)
            check("%s: the bots never chat (%d lines)" % (shown, len(w.lines)), not w.lines, " | ".join(t for _, _, _, t in w.lines[:3]))
        w.close()
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
        procs.remove(p)
        wait_for(lambda: p.poll() is not None, 5)


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", "kn_test_botchat_%d" % os.getpid()], capture_output=True)
    if r.returncode:
        print("bots_chat_e2e: sqlite scratch database unavailable: skipped")
        return 77
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    up = BanterStub(KEY).start()
    env = {k: v for k, v in os.environ.items() if not k.startswith("KN_AI_")}
    env.update(dbenv, KN_BRAIN_INTERNAL_SECRET=SECRET, KN_AI_UPSTREAM_URL=up.url + "/", KN_AI_UPSTREAM_KEY=KEY,
               KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_AUDIO_BACKEND="null")
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                       "--brain.region=local", "--brain.id=botchat-%d" % os.getpid(), "--ai.health_interval_s=1",
                                       "--accounts.email.required=false", "--smtp.host="], cwd=ROOT, env=env,
                                      stdout=open(brain_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 20)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        brain_port = int(m.group(1))
        h = wait_for(lambda: (lambda s: s if s["health"]["state"] == "healthy" else None)(http(brain_port, "/ai/status")[1]), 20)
        check("brain AI proxy healthy against the stub", bool(h), str(h)[:200])
        brain_linked_scenario(up, brain_port, env)
        standalone_scenario(env)
        check("the key never appears in the brain log", KEY not in read_log(brain_log))
    finally:
        try:
            up.stop()
        except Exception:  # noqa: BLE001 - already stopped in scenario 5
            pass
    return 0


code = 1
try:
    code = main()
finally:
    for p in procs:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()

if code == 77:
    sys.exit(77)
print(f"{sum(results)}/{len(results)} passed  (logs: {WORK})")
sys.exit(0 if results and all(results) and code == 0 else 1)
