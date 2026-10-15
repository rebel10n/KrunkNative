#!/usr/bin/env python3
"""The in-game Player List (v10 windows[22], showWindow(23): client/src/ui/hud_ui.c + hud_votekick.c) end
to end on a headless client_bin signed in as alice (level 30), in a brain-linked server_bin lobby with
three signed-in protocol peers: bob (6), carol (9, a flagged hacker) and dave (3).

  python3 -I client/tests/playerlist_e2e.py BUILD_DIR      (repository root)

kn_brain on a scratch sqlite database (KN_TEST_DB_DRIVER=postgres: the local 127.0.0.1:5432), ports
from kn_proto.free_ports(), the client headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, a
private XDG_CONFIG_HOME), joined through the matchmaker and driven through KRUNKNATIVE_UI_CMDS.
  1. the list: alice's own row, a row per player with the ping icon, the chat / voice mute buttons
     (accounts), the K.P.D button (alice is level 5+), Votekick, carol's name red (isHacker)
  2. muting: bob's chat mute -> speaker_notes_off, saved in mute_chat (his account id); bob's chat
     line is dropped, carol's shows; bob's voice mute -> voice_over_off, mute_voice; carol muted and
     unmuted again leaves the list as it was
  3. Votekick on dave: the server opens the vote (the peers get "vk" by alice), #voteToKick shows
     "alice voted to kick dave", no Votekick buttons left (initiated); bob's yes -> "Votes: 2/3";
     carol's yes kicks dave: the prompt hides, dave's row goes
  4. Call K.P.D on carol: the confirm popup ("Only call the K.P.D if you strongly suspect carol"),
     Call -> "Reported carol to the K.P.D" in chat, the kpd_calls row (reporter alice)
  5. a second launch (alice now a moderator): the mutes are still there (from disk), and the open call
     on carol is alice's case: carol's row is first in a red frame (the officer's suspect row)
77 = skip (no client_bin / server_bin / kn_brain, no database).
"""
import json
import os
import re
import shutil
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
import kn_proto  # noqa: E402

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
STAMP = os.getpid() % 100000
NAME = "kn_test_plist_%d" % os.getpid()
SECRET = "kn-plist-%d" % os.getpid()
(PORT,) = kn_proto.free_ports(1)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-plist-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PW = "pw-plist-%d" % os.getpid()
ALICE, BOB, CAROL, DAVE = "pla%d" % STAMP, "plb%d" % STAMP, "plc%d" % STAMP, "pld%d" % STAMP

CHAT_SEND, ERROR, FORWARDED, EXT = 8, 30, 85, 254
EXT_AUTH, EXT_RIGHTS, EXT_VOTE_KICK, EXT_VK_STATE = 0x8000, 0x8001, 0x0800, 0x0801
RIGHTS_FMT = "<BBBBii32s64s"
VK_STATE_FMT = "<BBBii32s32s"
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(str(detail)) <= 240)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0, step=0.1):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(step)
    try:
        return fn()
    except Exception:  # noqa: BLE001
        return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


class Sql:
    def __init__(self, db):
        self.db = db

    def __call__(self, text):
        if self.db.get("driver") == "sqlite":
            con = sqlite3.connect(self.db["path"])
            rows = [["" if v is None else str(v) for v in r] for r in con.execute(text).fetchall()]
            con.commit()
            con.close()
            return rows
        env = dict(os.environ, PGPASSWORD=str(self.db.get("password", "")))
        r = subprocess.run(["psql", "-h", str(self.db["host"]), "-p", str(self.db["port"]), "-U", str(self.db["user"]),
                            "-d", str(self.db["name"]), "-v", "ON_ERROR_STOP=1", "-qAt", "-c",
                            "SET search_path TO %s; %s" % (self.db["search_path"], text)],
                           env=env, capture_output=True, text=True, timeout=30)
        if r.returncode:
            return None
        return [l.split("|") for l in r.stdout.splitlines() if l and l != "SET" and not l.startswith(("UPDATE", "INSERT"))]


class Peer(kn_proto.Conn):
    """A bridged browser on protocol 1.4, signed in by session token (a TEST-NET address: not DEV)."""

    def __init__(self, remote, token):
        super().__init__("127.0.0.1", PORT)
        self.closed = False
        self.lock = threading.RLock()
        self.send(FORWARDED, remote.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 4, 2, 1]))
        self.join()
        # like server_votekick's peers: the join-settled push (status STATE) first, then the sign-in
        self.wait(lambda: any(s == 0 for s, _, _ in self.rights_seen()), 10)
        body = struct.pack("<BB32s64s", 1, 1, kn_proto.cstr("", 32), kn_proto.cstr(token, 64))
        self.send(EXT, struct.pack("<H", EXT_AUTH) + body)
        self.ok = self.wait(lambda: self.rights_ok(), 30)
        if not self.ok:
            print("rights seen: %s" % self.rights_seen())
            print("packets: %d, types %s, ext %s, closed %s" % (
                len(self.packets), sorted({t for t, _ in self.packets}),
                [(p[:2].hex(), len(p)) for t, p in self.packets if t == EXT], self.closed))
        self.ent(class_index=0, lag_comp=1.0)
        self.uid = -1
        end = time.time() + 6
        while time.time() < end and self.uid < 0:
            p = self.read(until=kn_proto.SPAWN, timeout=1)
            info = kn_proto.spawn_info(p) if p is not None else None
            if info and info["is_you"]:
                self.uid = info["uid"]

    def rights_ok(self):
        for t, p in self.packets:
            if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == EXT_RIGHTS:
                v = struct.unpack(RIGHTS_FMT, p[2:2 + struct.calcsize(RIGHTS_FMT)])
                if v[1] == 1 and v[0] == 1:
                    return True
        return False

    def rights_seen(self):
        return [struct.unpack(RIGHTS_FMT, p[2:2 + struct.calcsize(RIGHTS_FMT)])[:3] for t, p in self.packets
                if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == EXT_RIGHTS]

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            with self.lock:
                if self.read(timeout=max(0.05, min(0.2, end - time.time()))) is None and self.closed_socket():
                    self.closed = True

    def closed_socket(self):
        # a peek without waiting: with a timeout set, recv(MSG_DONTWAIT) waits out the timeout and raises
        # socket.timeout (an OSError), which would read as a closed peer
        try:
            self.s.setblocking(False)
            try:
                return self.s.recv(1, socket.MSG_PEEK) == b""
            except BlockingIOError:
                return False
            finally:
                self.s.settimeout(5.0)
        except OSError:
            return True

    def wait(self, fn, secs=8.0):
        end = time.time() + secs
        while time.time() < end and not self.closed:
            v = fn()
            if v:
                return v
            self.pump(0.1)
        return fn()

    def say(self, text):
        self.send(CHAT_SEND, text.encode() + b"\0")

    def vote(self, alias, yes=1):
        self.send(EXT, struct.pack("<H", EXT_VOTE_KICK) + struct.pack("<32sB", kn_proto.cstr(alias, 32), yes))

    def vk_states(self):
        out = []
        for t, p in self.packets:
            if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == EXT_VK_STATE:
                v = struct.unpack(VK_STATE_FMT, p[2:2 + struct.calcsize(VK_STATE_FMT)])
                out.append({"active": v[0], "opened": v[1], "votes": v[3], "needed": v[4], "target": kn_proto.uncstr(v[5]),
                            "voter": kn_proto.uncstr(v[6])})
        return out

    def error(self):
        for t, p in self.packets:
            if t == ERROR:
                return kn_proto.error_info(p)
        return None


class Client:
    """One headless client_bin signed in (the stored token), joined through the matchmaker."""

    def __init__(self, brain, name, token, tag):
        self.tag = tag
        self.cmds = os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        self.cfg = os.path.join(WORK, "client-cfg")  # shared by every launch: the saved mutes persist there
        os.makedirs(os.path.join(self.cfg, "krunknative"), exist_ok=True)
        for k, v in (("krunker_username", name), ("krunker_token", token), ("knRuleset", "v10")):
            with open(os.path.join(self.cfg, "krunknative", k + ".json"), "w") as f:
                f.write(v)
        # Settings > Chat "Show Player Messages" on (default off): this test reads the player chat lines
        with open(os.path.join(self.cfg, "krunknative", "knSettings.json"), "w") as f:
            f.write('{"showMessages": true}')
        script = os.path.join(WORK, tag + ".script")
        with open(script, "w") as f:
            f.write("0 play\n")
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": self.cfg,
                    "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                    "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_INPUT_SCRIPT": script,
                    "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain,
                    "KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain})
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(BUILD, "client_bin")], cwd=ROOT,
                                     stdout=open(self.log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     env=env, start_new_session=True)
        procs.append(self.proc)

    def text(self):
        return read_log(self.log)

    def cmd(self, line):
        with open(self.cmds, "a") as f:
            f.write(line + "\n")

    def ask(self, line, pattern, secs=6.0):
        """Send a probe and return the first match of `pattern` in the log after it."""
        mark = len(self.text())
        self.cmd(line)
        m = wait_for(lambda: re.search(pattern, self.text()[mark:]), secs)
        return m

    def textof(self, what):
        m = self.ask("textof " + what, r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(what))
        return m.group(1) if m else None

    def count(self, sel):
        m = self.ask("count " + sel, r"\[ui_cmds\] count %s = (\d+)" % re.escape(sel))
        return int(m.group(1)) if m else -1

    def has_box(self, elem_id):
        m = self.ask("rect " + elem_id, r"\[ui_cmds\] #%s(?: rect|: no box)" % re.escape(elem_id))
        return bool(m) and "no box" not in m.group(0)

    def clicksel(self, nth, sel):
        m = self.ask("clicksel %d %s" % (nth, sel), r"\[ui_cmds\] sel %s\[%d\](?: rect|: no box)" % (re.escape(sel), nth))
        time.sleep(0.4)
        return bool(m) and "no box" not in m.group(0)

    def open_list(self):
        self.cmd("key 342")  # Alt: pListKey frees the pointer (the page takes clicks), then showWindow(23)
        return wait_for(lambda: (self.count("#playerListH tr") or 0) > 0, 8)

    def rows(self):
        """{name: row number (1-based nth-child)} of the list's rows after alice's own (the first)."""
        n = self.count("#playerListH tr")
        out = {}
        for k in range(1, max(0, n) + 1):
            t = self.textof("#playerListH tr:nth-child(%d)" % k) or ""
            out[k] = t
        return out

    def row_of(self, name, skip_first=True):
        for k, t in self.rows().items():
            if (k > 1 or not skip_first) and name in t:  # textContent runs the cells together; the names are unique
                return k, t
        return None, None

    def shot(self, label):
        path = os.path.join(WORK, "%s-%s.png" % (self.tag, label))
        self.cmd("shot " + path)
        wait_for(lambda: os.path.exists(path), 10)
        return path

    def close(self):
        self.cmd("quit")
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGTERM)
            self.proc.wait(10)


def register(port, name):
    st, reg = http(port, "/auth/register", {"username": name, "password": PW})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    st2, me = http(port, "/auth/me?token=" + token) if token else (0, None)
    acc = me.get("account", me) if isinstance(me, dict) else {}
    aid = acc.get("id", 0) if isinstance(acc, dict) else 0
    check(f"account {name} registered", st == 200 and token and aid, f"{st} {str(reg)[:120]} {st2}")
    return token, aid


def node(port_brain):
    log = os.path.join(WORK, "node.log")
    path = os.path.join(WORK, "node.json")
    gs = {"node_id": "PLIST", "name": "player list e2e", "listen_port": PORT, "public_host": "127.0.0.1", "region": "local",
          "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0, "ruleset": "v10",
          "bans_file": os.path.join(WORK, "bans.json"), "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port_brain,
          "brain_secret": SECRET, "finder_url": "http://127.0.0.1:%d" % port_brain}
    with open(path, "w") as f:
        json.dump({"gameserver": gs}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", path, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return log


def saved(cfg, key):
    try:
        with open(os.path.join(cfg, "krunknative", key + ".json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def joined(c, node_log, name):
    return wait_for(lambda: re.search(r"^connected to server", c.text(), re.M) and name in read_log(node_log)
                    and re.search(r"\[auth\]|account", read_log(node_log)), 60, 0.5)


def first_launch(brain, node_log, sql, tokens, ids, peers):
    bob, carol, dave = peers[BOB], peers[CAROL], peers[DAVE]
    c = Client(brain, ALICE, tokens[ALICE][0], "a1")
    check("alice's client joins the lobby through the matchmaker", joined(c, node_log, ALICE), c.text()[-500:])
    for p in peers.values():
        p.pump(1.0)
    time.sleep(2)

    # 1. the list
    check("1: showWindow(23) opens the Player List", c.open_list(), c.text()[-300:])
    k_bob, t_bob = c.row_of(BOB)
    k_carol, t_carol = c.row_of(CAROL)
    k_dave, t_dave = c.row_of(DAVE)
    check("1: a row per player (bob, carol, dave)", k_bob and k_carol and k_dave, str(c.rows()))
    first = c.textof("#playerListH tr:nth-child(1)") or ""
    check("1: alice's own row first", ALICE in first, first)
    for who, k, t in ((BOB, k_bob, t_bob), (CAROL, k_carol, t_carol), (DAVE, k_dave, t_dave)):
        row = "#playerListH tr:nth-child(%d)" % (k or 0)
        check(f"1: {who}'s row: ping, chat + voice mute, K.P.D, Votekick",
              t and "signal_cellular_alt" in t and "chat" in t and "record_voice_over" in t and "Votekick" in t
              and c.count(row + " .muteUserBtn") == 2 and c.count(row + " .punishButton.kdf") == 1
              and c.count(row + " .punishButton.vote") == 1, t)
    check("1: carol's name is red (isHacker), nobody else's", c.count("#playerListH tr:nth-child(%d) a.red" % (k_carol or 0)) == 1
          and c.count("#playerListH a.red") == 1)
    c.shot("list")

    # 2. mutes
    row_bob = "#playerListH tr:nth-child(%d)" % k_bob
    check("2: click bob's chat mute", c.clicksel(0, row_bob + " .muteUserBtn"))
    check("2: the button turns speaker_notes_off", wait_for(lambda: "speaker_notes_off" in (c.textof(row_bob) or ""), 4), c.textof(row_bob))
    check("2: mute_chat saved bob's account id", wait_for(lambda: saved(c.cfg, "mute_chat") == [ids[BOB]], 4), str(saved(c.cfg, "mute_chat")))
    bob.say("hello-from-bob")
    time.sleep(0.5)
    carol.say("hello-from-carol")
    chat = wait_for(lambda: (lambda t: t if t and "hello-from-carol" in t else None)(c.textof("chatList")), 8)
    check("2: carol's chat shows, bob's is dropped", chat and "hello-from-bob" not in chat, chat)
    check("2: click bob's voice mute", c.clicksel(1, row_bob + " .muteUserBtn"))
    check("2: the button turns voice_over_off", wait_for(lambda: "voice_over_off" in (c.textof(row_bob) or ""), 4), c.textof(row_bob))
    check("2: mute_voice saved bob's account id", wait_for(lambda: saved(c.cfg, "mute_voice") == [ids[BOB]], 4), str(saved(c.cfg, "mute_voice")))
    row_carol = "#playerListH tr:nth-child(%d)" % k_carol
    c.clicksel(0, row_carol + " .muteUserBtn")
    muted = wait_for(lambda: ids[CAROL] in (saved(c.cfg, "mute_chat") or []), 4)
    c.clicksel(0, row_carol + " .muteUserBtn")
    check("2: carol muted and unmuted: mute_chat back to bob alone, her button chat again",
          muted and wait_for(lambda: saved(c.cfg, "mute_chat") == [ids[BOB]], 4) and "speaker_notes_off" not in (c.textof(row_carol) or ""),
          f"{saved(c.cfg, 'mute_chat')} {c.textof(row_carol)}")

    # 3. Votekick on dave
    row_dave = "#playerListH tr:nth-child(%d)" % k_dave
    check("3: click dave's Votekick", c.clicksel(0, row_dave + " .punishButton.vote"))
    opened = bob.wait(lambda: [s for s in bob.vk_states() if s["opened"]], 6)
    s = opened[0] if opened else {}
    check("3: the server opened the vote: dave 1/3 by alice", s.get("target") == DAVE and s.get("voter") == ALICE and s.get("votes") == 1
          and s.get("needed") == 3, str(s))
    # the prompt lives in the in-game HUD, which hides under an open menu window (hud_ui.c hud_apply_visibility),
    # so its box is not visible while the list is up: the text is what is checked here
    check("3: #voteToKick says \"alice voted to kick dave\"", wait_for(lambda: (c.textof("voteKickInfo") or "")
          == "%s voted to kick %s" % (ALICE, DAVE), 5), c.textof("voteKickInfo"))
    check("3: no Votekick button left (one vote per session)", wait_for(lambda: c.count("#playerListH .punishButton.vote") == 0, 4),
          str(c.count("#playerListH .punishButton.vote")))
    bob.vote(DAVE)
    check("3: bob's yes: Votes: 2/3", wait_for(lambda: c.textof("voteKickNum") == "Votes: 2/3", 6), c.textof("voteKickNum"))
    carol.vote(DAVE)
    err = dave.wait(lambda: dave.error(), 6)
    check("3: carol's yes kicks dave (Vote Kicked)", err is not None and "Vote Kicked" in err[0], str(err))
    check("3: dave's row goes", wait_for(lambda: c.row_of(DAVE)[0] is None, 8), str(c.rows()))
    dave.close()

    # 4. Call K.P.D on carol
    k_carol, _ = c.row_of(CAROL)
    row_carol = "#playerListH tr:nth-child(%d)" % (k_carol or 0)
    check("4: click carol's K.P.D button", c.clicksel(0, row_carol + " .punishButton.kdf"))
    pop = wait_for(lambda: (lambda t: t if t and "K.P.D" in t else None)(c.textof("genericPop")), 5)
    check("4: the confirm popup", pop and "Only call the K.P.D if you strongly suspect %s of using Hacks." % CAROL in pop
          and "False Reports can result in a Ban or Loss of Perms." in pop, pop)
    c.cmd("clickid confirmBtn")
    chat = wait_for(lambda: (lambda t: t if t and "Reported %s to the K.P.D" % CAROL in t else None)(c.textof("chatList")), 10)
    check("4: Call: \"Calling K.P.D ...\" then \"Reported carol to the K.P.D\"", chat and "Calling K.P.D" in chat, chat)
    rows = sql("SELECT reporter_id, suspect_id, status FROM kpd_calls")
    check("4: kpd_calls has alice's call on carol (open)", rows == [[str(ids[ALICE]), str(ids[CAROL]), "0"]], str(rows))
    c.shot("kpd")
    c.close()
    check("the first client quits cleanly", c.proc.returncode == 0, str(c.proc.returncode))


def second_launch(brain, node_log, sql, tokens, ids, peers):
    sql("UPDATE players SET player_moderator = 1 WHERE player_id = %d" % ids[ALICE])
    c = Client(brain, ALICE, tokens[ALICE][0], "a2")
    check("alice's second client joins", joined(c, node_log, ALICE), c.text()[-500:])
    for p in peers.values():
        if not p.closed:
            p.pump(1.0)
    time.sleep(2)
    check("5: the server handed alice the open case", wait_for(lambda: "takes the case of %s" % CAROL in read_log(node_log), 8),
          read_log(node_log)[-400:])
    check("5: the list opens", c.open_list())
    first = c.textof("#playerListH tr:nth-child(1)") or ""
    check("5: carol's row is first, in the red frame (the officer's suspect)", CAROL in first
          and c.count("#playerListH tr:nth-child(1)[style*='red']") == 1 and c.count("#playerListH tr:nth-child(1) a.red") == 1, first)
    others = [t for k, t in c.rows().items() if k > 1]
    check("5: and not again below", not any(CAROL in t for t in others), str(others))
    k_bob, t_bob = c.row_of(BOB)
    check("5: bob is still muted (chat and voice, from disk)", t_bob and "speaker_notes_off" in t_bob and "voice_over_off" in t_bob, t_bob)
    c.shot("officer")
    c.close()


def main():
    for b in ("client_bin", "server_bin", "services/brain/kn_brain"):
        if not os.access(os.path.join(BUILD, b), os.X_OK):
            print("playerlist_e2e: no %s in %s: skipped" % (b, BUILD))
            return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("playerlist_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    db = json.loads(r.stdout)
    if str(db.get("port")) == "5434":
        print("playerlist_e2e: refusing database port 5434")
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return 1
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    peers = {}
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=plist-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts on %s" % DRIVER, bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        node_log = node(port)
        check("the node links to the coordinator", wait_for(lambda: "welcomed" in read_log(node_log) or "linked" in read_log(node_log), 20))
        tokens = {n: register(port, n) for n in (ALICE, BOB, CAROL, DAVE)}
        if not all(t for t, _ in tokens.values()):
            return 1
        ids = {n: tokens[n][1] for n in tokens}
        sql = Sql(db)
        for name, score in ((ALICE, 1000000), (BOB, 40000), (CAROL, 90000), (DAVE, 10000)):
            sql("UPDATE players SET player_score = %d WHERE player_id = %d" % (score, ids[name]))
        sql("UPDATE players SET player_hack = 1 WHERE player_id = %d" % ids[CAROL])
        for i, name in enumerate((BOB, CAROL, DAVE)):
            p = Peer("203.0.113.%d" % (90 + i), tokens[name][0])
            check(f"{name} signs in and spawns", p.ok and p.uid >= 0, f"{p.ok} {p.uid}")
            peers[name] = p
        stop = threading.Event()

        def drain():  # the peers keep reading while the client boots and the test waits on it
            while not stop.is_set():
                for p in list(peers.values()):
                    if not p.closed:
                        p.pump(0.05)
                time.sleep(0.05)
        threading.Thread(target=drain, daemon=True).start()
        first_launch(port, node_log, sql, tokens, ids, peers)
        second_launch(port, node_log, sql, tokens, ids, peers)
    finally:
        if "stop" in locals():
            stop.set()
            time.sleep(0.3)
        for p in peers.values():
            p.close()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
