#!/usr/bin/env python3
"""The chat's globe icon (#chatSwitch, the text & voice channel) end to end (client/src/ui/hud_ui.c
hud_switch_chat, the page markup in ui/index.html, v10.0.3 window.switchChat: doc/v10/chat-globe.md).

  python3 -I client/tests/chat_globe_e2e.py BUILD_DIR      (run from the repository root)

The globe is a toggle, not a picker: a click flips data-tab between "public" (Global) and "groups"
(Team) only in a team mode; elsewhere it stays Global. Each click plays select_0 at 0.1 (v10's
playSelect(0.1)); a team switch adds the system line "Team Text & Voice Chat" /
"Global Text & Voice Chat"; nothing is stored (no setting, no file); the channel is what the next
typed line is sent on ("[chat team]" on the server log, the sandbox's line for a team message).

Headless client_bin runs, offline Kanji, input scripts, the Escape menu open (the cursor is free there;
in game the cursor is locked and a click is the shot, in v10 too). Three runs per mode: a control
without the click and the run with it, so the select_0 count is compared, not guessed.
  A. Free for All: the globe is Global; a click keeps it Global, no system line, one select_0 more
     than the control. Screenshots: the menu open with the globe, and after the click.
  B. Team Deathmatch (KRUNKNATIVE_MODE=tdm): a click shows Team (data-tab groups, the team system line,
     one select_0 more); a typed line is sent on the team channel; a second click shows Global again
     (the global system line); a typed line after it is sent on the global channel. Screenshots:
     open, after the first choice, after the second.
  C. Nothing is stored: no key of the saved settings (knSettings.json) names the channel.
  D. Two teams on a server (server_bin --mode tdm, port from kn_proto.free_ports): two protocol peers
     (server/tests/kn_proto.py, HELLO 1.3 = v7 shapes: NET_PACKET_CHAT_V7 "ch" with its channel) spawn on
     the two teams, a spectator peer joins (ENT spectate: no player, no team), then the headless client
     joins (--connect), switches the globe to Team and types a line. v7.1.4 sendPlayerChat
     (server/index.ts:2065), the server side of v10's "ct" [channel, msg]: the client's team line reaches
     its teammate (channel 1) and never the enemy; the teammate's team line shows in the client's chat,
     the enemy's team line is not in its page at all (the server never sent it); the spectator's team line
     reaches nobody else (no team: the sender alone); the enemy's Global line reaches everyone (channel 0)
     and sits in the client's chat on tab 0, hidden under Team. The globe's glyph is its data-tab
     ("groups"), the input's placeholder is never changed (v10 has no placeholder swap).
  E. The round's INIT outside a team mode (the server console's `mode ffa` restarts the map): the client's
     Team channel is forced back to Global (v10 deob-10.0.3.js:257141 switchChat(chatSwitch): data-tab
     public, the select sound, no channel line because the mode has no teams).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it); D / E need
server_bin next to client_bin (skipped with a note when it is missing).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
sys.path.insert(0, os.path.join(REPO, "server", "tests"))
import kn_proto  # noqa: E402

CHAT_V7 = 245  # NET_PACKET_CHAT_V7 (shared/include/social_v7.h): C->S channel + text, S->C NetChatV7Packet (330 bytes)
CHAT_V3 = 9    # NET_PACKET_CHAT: the v3 line (no channel) an older peer would get
CHAT_V7_FMT = "<bi" + "iB32s288s"  # channel, account_id, uid, kind, name, text
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-chat-globe-", dir=CACHE if os.path.isdir(CACHE) else None)
SHOTS = os.environ.get("KN_CHAT_GLOBE_SHOTS") or os.path.join(CACHE, "kn-chat-globe", "shots")
os.makedirs(SHOTS, exist_ok=True)
CFG = os.path.join(WORK, "cfg")
STORE = os.path.join(CFG, "krunknative")
KEY = os.path.join(STORE, "knSettings.json")
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script, mode=None, timeout=180):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_MODE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": CFG, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_AUDIO_LOG": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    if mode:
        env["KRUNKNATIVE_MODE"] = mode
    env.setdefault("KRUNKNATIVE_ASSETS_DIR", os.path.join(REPO, "assets"))
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        text = f.read()
    return text


def attrs(text):
    # "[input_script] attr t=1700 @a0 n=1 data-tab=public" -> {label: value}
    return {m.group(1): m.group(2) for m in re.finditer(r"\[input_script\] attr t=\d+ @(\S+) n=\d+ data-tab=(\S+)", text)}


def hits(text, label):
    m = re.search(r"\[input_script\] grep t=\d+ @" + re.escape(label) + r" needle=\S+ n=\d+ hits=(\d+)", text)
    return int(m.group(1)) if m else None


def selects(text):
    # every 2D select_0 the audio layer starts (KRUNKNATIVE_AUDIO_LOG prints "[audio] t=.. play2d select_0 vol=..")
    return len(re.findall(r"\bplay2d select_0\b", text))


def sent_channel(text, word):
    # the sandbox echoes a chat line as "[sandbox] [chat team] Guest_1: team-typed-line" (team) and
    # "[sandbox] [chat] Guest_1: global-typed-line" (global)
    tag = r"\[chat team\]" if word == "team" else r"\[chat\]"
    return bool(re.search(r"\[sandbox\] " + tag + r" \S+: " + re.escape(word) + r"-typed-line", text))


def stored_channel():
    try:
        with open(KEY) as f:
            text = f.read()
    except OSError:
        return None
    return bool(re.search(r"chatSwitch|chatChannel|\"groups\"|chat.*[Cc]hannel", text))


def open_menu(t):
    return f"{t} down ESC\n{t + 50} up ESC\n"


def typed(t, word):
    return f"{t} down ENTER\n{t + 50} up ENTER\n{t + 100} type {word}\n{t + 200} down ENTER\n{t + 250} up ENTER\n"


def ffa_script(click):
    s = "0 play\n" + open_menu(1500)
    s += f"1800 frames {SHOTS}/a-ffa-open\n"
    s += "1850 attr @a0 data-tab [id=chatSwitch]\n"
    if click:
        s += "1900 clickid chatSwitch\n"
    s += "2100 attr @a1 data-tab [id=chatSwitch]\n"
    s += "2150 grep @g1 Team [id=chatList]\n"
    s += f"2200 frames {SHOTS}/a-ffa-after\n"
    s += "2300 quit\n"
    return s


def team_script(click):
    s = "0 play\n" + open_menu(1500)
    s += f"1800 frames {SHOTS}/b-team-open\n"
    s += "1850 attr @b0 data-tab [id=chatSwitch]\n"
    if click:
        s += "1900 clickid chatSwitch\n"
    s += "2100 attr @b1 data-tab [id=chatSwitch]\n"
    s += "2150 grep @b1t Team [id=chatList]\n"
    s += f"2200 frames {SHOTS}/b-team-first\n"
    s += "2250 down ESC\n2300 up ESC\n"
    s += typed(2400, "team-typed-line")
    s += open_menu(2900)
    if click:
        s += "3000 clickid chatSwitch\n"
    s += "3200 attr @b2 data-tab [id=chatSwitch]\n"
    s += "3250 grep @b2g Global [id=chatList]\n"
    s += f"3300 frames {SHOTS}/b-team-second\n"
    s += "3350 down ESC\n3400 up ESC\n"
    s += typed(3500, "global-typed-line")
    s += "4200 quit\n"
    return s


class Peer:
    """A protocol peer on the server (kn_proto.Conn): HELLO 1.3 announces the v7 social shapes, so a chat line
    comes back as NET_PACKET_CHAT_V7 with its channel. spawn(spectate=1) joins as a spectator (no player)."""

    def __init__(self, port, label):
        self.label = label
        self.c = kn_proto.Conn("127.0.0.1", port, timeout=5.0)
        self.c.send(kn_proto.HELLO, bytes([1, 3, 0]))  # protocol 1.3: NET_PROTOCOL_MINOR_V7
        self.c.join()
        self.uid = None
        self.team = None

    def spawn(self, spectate=0):
        self.c.ent(spectate=spectate)

    def pump(self, secs=0.5):
        self.c.read(timeout=secs)
        for t, p in self.c.packets:
            if t == kn_proto.SPAWN and len(p) >= 14:
                uid, is_you, _active, _cls, team = struct.unpack("<iBBii", p[:14])
                if is_you:  # the latest one: the JOIN's SPAWN is the slot (team 0), the ENT's the team
                    self.uid, self.team = uid, team

    def spawns(self):
        """{uid: team} of every SPAWN read so far."""
        out = {}
        for t, p in self.c.packets:
            if t == kn_proto.SPAWN and len(p) >= 14:
                uid, _is_you, _active, _cls, team = struct.unpack("<iBBii", p[:14])
                out[uid] = team
        return out

    def chats(self):
        """[(channel, uid, name, text)] of the chat lines read so far (CHAT_V7 with its channel; a v3 CHAT as None)."""
        out = []
        for t, p in self.c.packets:
            if t == CHAT_V7 and len(p) == struct.calcsize(CHAT_V7_FMT):
                ch, _acct, uid, _kind, name, text = struct.unpack(CHAT_V7_FMT, p)
                out.append((ch, uid, kn_proto.uncstr(name), kn_proto.uncstr(text)))
            elif t == CHAT_V3 and len(p) >= 37:
                uid, _kind, name = struct.unpack("<iB32s", p[:37])
                out.append((None, uid, kn_proto.uncstr(name), kn_proto.uncstr(p[37:])))
        return out

    def got(self, text):
        return [c for c in self.chats() if c[3] == text]

    def send_chat(self, channel, text):
        self.c.send(CHAT_V7, bytes([channel]) + text.encode() + b"\0")

    def close(self):
        self.c.close()


def wait_log(path, needle, secs, peers=(), step=0.1):
    """True once `needle` is in the file; pumps the peers meanwhile (their sockets must keep reading)."""
    end = time.time() + secs
    while time.time() < end:
        try:
            with open(path, errors="replace") as f:
                if needle in f.read():
                    return True
        except OSError:
            pass
        for p in peers:
            p.pump(step)
        if not peers:
            time.sleep(step)
    return False


def server_script():
    # the globe to Team, a typed team line, then the peers' lines (the test sends them once @d1 is logged); the
    # enemy's Global line is the last one sent and the first tab-0 row of #chatList: the script waits for it
    s = "0 play\n" + open_menu(1500)
    s += "1800 attr @d0 data-tab [id=chatSwitch]\n"
    s += "1850 attr @p0 placeholder [id=chatInput]\n"
    s += "1900 clickid chatSwitch\n"
    s += "2100 attr @d1 data-tab [id=chatSwitch]\n"
    s += "2150 attr @p1 placeholder [id=chatInput]\n"
    s += "2200 down ESC\n2250 up ESC\n"
    s += typed(2300, "team-typed-line")
    # no `#` in a script line: it starts a comment (input_script.c), so the ids are attribute selectors
    s += "2700 waitfor [id=chatList] > div[data-tab=\"0\"]\n"
    s += "2800 grep @dm mate-team-line [id=chatList]\n"
    s += "2810 grep @de enemy-team-line [id=chatList]\n"
    s += "2820 grep @dgh ghost-team-line [id=chatList]\n"
    s += "2830 grep @dg enemy-global-line [id=chatList]\n"
    s += "2840 grep @dmine team-typed-line [id=chatList]\n"
    s += "2850 nodes @t1 [id=chatList] > div[data-tab=\"1\"]\n"
    s += "2860 nodes @t0 [id=chatList] > div[data-tab=\"0\"]\n"
    s += open_menu(2900)
    s += f"3200 frames {SHOTS}/d-team-lines\n"
    # E: the test sends `mode ffa` to the server console once @e0 is logged; the INIT forces Global
    # the channel rows ("... Text & Voice Chat"), counted per row: the grep is case-insensitive over an element's
    # text, so a needle like Global would match the enemy's enemy-global-line in the whole list
    s += "3300 attr @e0 data-tab [id=chatSwitch]\n"
    s += "3310 grep @e0v voice [id=chatList] > div\n"
    s += "3400 waitfor [id=chatSwitch][data-tab=\"public\"]\n"
    s += "3500 attr @e1 data-tab [id=chatSwitch]\n"
    s += "3550 grep @e1v voice [id=chatList] > div\n"
    s += "3600 quit\n"
    return s


def nodes_match(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @" + re.escape(label) + r" elements=\d+ match=(\d+)", text)
    return int(m.group(1)) if m else None


def server_flow():
    server = os.path.join(BUILD, "server_bin")
    if not os.path.exists(server):
        print(f"D/E skipped: no server_bin in {BUILD}")
        return
    (port,) = kn_proto.free_ports(1)
    slog_path = os.path.join(WORK, "d-server.log")
    slog = open(slog_path, "w")
    srv = subprocess.Popen([server, "--cfg", "none", "--port", str(port), "--mode", "tdm"], cwd=REPO, stdin=subprocess.PIPE,
                           stdout=slog, stderr=subprocess.STDOUT, start_new_session=True, text=True)
    peers = []
    client = None
    log = os.path.join(WORK, "d-client.log")
    try:
        if not wait_log(slog_path, "Listening on port", 20):
            check("D: server_bin is listening", False, slog_path)
            return
        a, b, ghost = Peer(port, "a"), Peer(port, "b"), Peer(port, "ghost")
        peers = [a, b, ghost]
        a.spawn()
        a.pump(1.0)
        b.spawn()
        b.pump(1.0)
        ghost.spawn(spectate=1)
        for p in peers:
            p.pump(0.5)
        check("D: the two peers spawned on the two teams", a.uid is not None and b.uid is not None and a.team != b.team,
              f"a uid {a.uid} team {a.team}, b uid {b.uid} team {b.team}")

        script = os.path.join(WORK, "d.script")
        with open(script, "w") as f:
            f.write(server_script())
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_MODE")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": CFG, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_AUDIO_LOG": "1",
                    "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
        env.setdefault("KRUNKNATIVE_ASSETS_DIR", os.path.join(REPO, "assets"))
        out = open(log, "w")
        client = subprocess.Popen([f"{BUILD}/client_bin", "--connect", f"127.0.0.1:{port}"], cwd=REPO, stdout=out,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)

        # the client in Team (its @d1 line), its SPAWN seen by the peers: which peer is its teammate
        check("D: the client joined and switched the globe to Team", wait_log(log, "@d1 ", 240, peers), log)
        known = {a.uid, b.uid}
        me = [(uid, team) for uid, team in a.spawns().items() if uid not in known]
        check("D: the peers saw the client spawn (its team)", len(me) == 1, f"other spawns {me}")
        my_team = me[0][1] if me else None
        mate, enemy = (a, b) if my_team == a.team else (b, a)
        print(f"      client uid {me[0][0] if me else '?'} team {my_team}: teammate peer {mate.label}, enemy peer {enemy.label}")

        mate.send_chat(1, "mate-team-line")
        enemy.send_chat(1, "enemy-team-line")
        ghost.send_chat(1, "ghost-team-line")
        for p in peers:
            p.pump(0.4)
        enemy.send_chat(0, "enemy-global-line")  # the sync line: the first tab-0 row of the client's #chatList
        check("D: the client's chat got the peers' lines (the enemy's Global line)", wait_log(log, "@dg ", 120, peers), log)
        for p in peers:
            p.pump(0.5)
        text = open(log, errors="replace").read()
        d = attrs(text)

        # the server's routing, as the peers received it
        mine = mate.got("team-typed-line")
        check("D: the client's team line reached its teammate on channel 1 (\"ch\" [1, name, msg])",
              len(mine) == 1 and mine[0][0] == 1, str(mine))
        check("D: the client's team line never reached the enemy", not enemy.got("team-typed-line"), str(enemy.got("team-typed-line")))
        check("D: the client's team line never reached the spectator (no team)", not ghost.got("team-typed-line"),
              str(ghost.got("team-typed-line")))
        check("D: the teammate's team line echoed to itself on channel 1", [c[0] for c in mate.got("mate-team-line")] == [1],
              str(mate.got("mate-team-line")))
        check("D: the enemy's team line stayed on its team (not the teammate's)", not mate.got("enemy-team-line"),
              str(mate.got("enemy-team-line")))
        check("D: the spectator's team line reached the spectator alone, on channel 1",
              [c[0] for c in ghost.got("ghost-team-line")] == [1] and not mate.got("ghost-team-line") and not enemy.got("ghost-team-line"),
              f"ghost {ghost.got('ghost-team-line')} mate {mate.got('ghost-team-line')} enemy {enemy.got('ghost-team-line')}")
        check("D: the enemy's Global line reached everyone on channel 0",
              all([c[0] for c in p.got("enemy-global-line")] == [0] for p in peers), str([p.got("enemy-global-line") for p in peers]))

        # the client's page: the globe, the placeholder, the rows and their tabs
        check("D: the globe showed Global (public) before the click and Team (groups) after it", d.get("d0") == "public" and d.get("d1") == "groups", str(d))
        ph = {m.group(1): m.group(2) for m in re.finditer(r"\[input_script\] attr t=\d+ @(p\d) n=\d+ placeholder=(.*)", text)}
        check("D: the chat input's placeholder is the same on both channels (v10 swaps none)",
              ph.get("p0") == "Enter Message" and ph.get("p1") == "Enter Message", str(ph))
        check("D: the client's chat shows the teammate's team line", hits(text, "dm") == 1, f"hits {hits(text, 'dm')}")
        check("D: the client's chat shows its own team line", hits(text, "dmine") == 1, f"hits {hits(text, 'dmine')}")
        check("D: the enemy's team line is not in the client's page at all", hits(text, "de") == 0, f"hits {hits(text, 'de')}")
        check("D: the spectator's team line is not in the client's page at all", hits(text, "dgh") == 0, f"hits {hits(text, 'dgh')}")
        check("D: the enemy's Global line is in the client's page (hidden under Team: a tab-0 row)",
              hits(text, "dg") == 1 and nodes_match(text, "t0") == 1, f"hits {hits(text, 'dg')} tab-0 rows {nodes_match(text, 't0')}")
        check("D: the team rows are on tab 1 (the client's and the teammate's lines)", nodes_match(text, "t1") == 2,
              f"tab-1 rows {nodes_match(text, 't1')}")

        # E: the INIT of a round without teams forces Global
        check("E: the client is still on Team before the mode change", wait_log(log, "@e0 ", 60, peers) and attrs(open(log, errors="replace").read()).get("e0") == "groups", log)
        srv.stdin.write("mode ffa\n")
        srv.stdin.flush()
        try:
            client.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(client.pid, signal.SIGKILL)
            client.wait()
            check("E: client_bin finished its script", False, f"timeout, log {log}")
        text = open(log, errors="replace").read()
        e = attrs(text)
        check("E: the FFA round's INIT forced the globe back to Global (data-tab public)", e.get("e1") == "public", str(e))
        check("E: the forced switch shows no channel line (no teams: switchChat says nothing)", hits(text, "e1v") == hits(text, "e0v"),
              f"channel rows before {hits(text, 'e0v')} after {hits(text, 'e1v')}")
        check("E: select_0 played for the click and for the forced switch (2 in all, v10 playSelect(0.1) per switchChat)",
              selects(text) == 2, f"select_0 count {selects(text)}")
        check("E: the server logged the client's team line as a team line", bool(re.search(r"\[chat team\] \S+: team-typed-line", open(slog_path, errors="replace").read())),
              slog_path)
    finally:
        for p in peers:
            p.close()
        if client is not None and client.poll() is None:
            os.killpg(client.pid, signal.SIGKILL)
            client.wait()
        try:
            srv.stdin.close()
        except OSError:
            pass
        srv.terminate()
        try:
            srv.wait(timeout=10)
        except subprocess.TimeoutExpired:
            srv.kill()
        slog.close()


def main():
    os.makedirs(STORE, exist_ok=True)
    with open(os.path.join(STORE, "krk_advanced.json"), "w") as f:
        f.write("1")

    # A. Free for All
    ctrl = run("ffa-control", ffa_script(False))
    text = run("ffa", ffa_script(True))
    a = attrs(text)
    check("A: the globe is Global (public) before a click in FFA", attrs(ctrl).get("a0") == "public", str(attrs(ctrl)))
    check("A: a click in FFA keeps the globe Global", a.get("a1") == "public", str(a))
    check("A: a click in FFA shows no team system line", hits(text, "g1") == 0, f"hits {hits(text, 'g1')}")
    check("A: a click in FFA plays select_0 once (v10 playSelect(0.1))", selects(text) - selects(ctrl) == 1,
          f"clicked {selects(text)} control {selects(ctrl)}")

    # B. Team Deathmatch
    ctrl = run("tdm-control", team_script(False), mode="tdm")
    text = run("tdm", team_script(True), mode="tdm")
    b = attrs(text)
    check("B: the globe is Global before the first click", b.get("b0") == "public", str(b))
    check("B: the first click switches to Team (data-tab groups)", b.get("b1") == "groups", str(b))
    check("B: the team switch says so in the chat", (hits(text, "b1t") or 0) >= 1, f"hits {hits(text, 'b1t')}")
    check("B: each of the two clicks plays select_0 (v10 playSelect(0.1) on every switchChat)",
          selects(text) - selects(ctrl) == 2, f"clicked {selects(text)} control {selects(ctrl)}")
    check("B: a line typed in Team is sent on the team channel", sent_channel(text, "team"), "server log")
    check("B: the second click switches back to Global (data-tab public)", b.get("b2") == "public", str(b))
    check("B: the Global switch says so in the chat", (hits(text, "b2g") or 0) >= 1, f"hits {hits(text, 'b2g')}")
    check("B: a line typed after it is sent on the global channel", sent_channel(text, "global"), "server log")

    # C. nothing stored
    check("C: the channel is not stored (no saved setting names it)", stored_channel() in (None, False),
          f"stored {stored_channel()!r}")

    # D / E. two teams on a server_bin, a spectator, the forced Global of an FFA round
    server_flow()

    print(f"\nscreenshots: {SHOTS}")
    ok = bool(results) and all(results)
    print(f"{'ALL PASSED' if ok else 'FAILED'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
