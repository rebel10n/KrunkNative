#!/usr/bin/env python3
"""v10's notification center, the emoji panel and the item tooltips end to end (client/src/ui/
v10_notify.c, ui_tooltip.c) on the v10 page:

  1. Friend request (native + kn_brain on sqlite, temp dir): alice follows bob, so bob's
     notification center raises "Friend Request" (10 s); its countdown bar narrows over time; a
     click dismisses it. bob follows alice back; alice's direct message raises "Message from
     alice" (6 s) that counts down and expires on its own.
  2. Emoji panel (offline, in game): ":lo" opens the panel, Enter picks the selected emoji
     (:love:) without sending, Enter sends; the chat line shows the emoji's image.
  3. Loadout hover (offline, Change Loadout > Hat picker): a card's item tooltip (#knTip) appears
     above the card, 9 px clear of its top edge (4 px + the 5 px gap of v10's tooltip helper).
  4. Game announcement (server_bin, its console `announce`): set before the client joins, the
     joining client shows it (.game-announcement.show, the message line); `announce clear` hides it.

  python3 -I client/tests/notify_e2e.py BUILD_DIR      (repository root)
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs); the
client's inputs come from an input script (KRUNKNATIVE_INPUT_SCRIPT) or KRUNKNATIVE_UI_CMDS;
ports from kn_proto.free_ports(). 77 = skip (no client_bin / kn_brain / websockets).
"""
import asyncio
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.getcwd()
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CLIENT = os.path.join(BUILD, "client_bin")
if not (os.path.exists(BRAIN) and os.path.exists(CLIENT)):
    print("needs client_bin and kn_brain in %s: skip" % BUILD)
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

# the game assets (css/fonts, img, ...) are git-ignored in a worktree: KRUNKNATIVE_ASSETS_DIR picks them
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-notify-e2e.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PASS_WORD = "notify_pass_1"


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + ("" if cond or not detail else "  [%s]" % detail), flush=True)
    if not cond:
        FAILS.append(what)


def wait(fn, timeout=25.0, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


# ------------------------------------------------------------------ social (kn_brain)

async def auth(port, name, action):
    """["a", 0 = register | 1 = login, [name, password]] over the social socket; returns (id, token)
    and closes the socket"""
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", action, [name, PASS_WORD, None] if action == 0 else [name, PASS_WORD]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


async def social_do(port, name, frame, hold=0.6):
    """logs `name` in over its own socket, sends one frame and keeps the socket open `hold` s"""
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 1, [name, PASS_WORD]]))
        while json.loads(await asyncio.wait_for(ws.recv(), 20))[0] != "a":
            pass
        await ws.send(json.dumps(frame))
        await asyncio.sleep(hold)


# ------------------------------------------------------------------ clients

class UiClient:
    """One headless client_bin on the v10 page, logged in as `name` (KRUNKNATIVE_UI_CMDS)."""

    def __init__(self, port, name, token, tag):
        self.cmds = os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        self.tag = tag
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        with open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w") as f:
            f.write(name)
        with open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w") as f:
            f.write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds,
                    "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port,
                    "KRUNKNATIVE_ASSETS_DIR": ASSETS})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--offline"], stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)

    def text(self):
        try:
            with open(self.log, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def cmd(self, line):
        with open(self.cmds, "a") as f:
            f.write(line + "\n")

    def rect(self, elem_id, timeout=10.0):
        """the element's box ('rect'), None while it has none ('no box')"""
        end = time.time() + timeout
        while time.time() < end:
            mark = len(self.text())
            self.cmd("rect " + elem_id)
            t0 = time.time()
            while time.time() - t0 < 1.5:
                tail = self.text()[mark:]
                m = re.search(r"#%s rect ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)" % re.escape(elem_id), tail)
                if m:
                    return tuple(float(v) for v in m.groups())
                if "#%s: no box" % elem_id in tail:
                    return None
                time.sleep(0.1)
            time.sleep(0.2)
        return None

    def click(self, elem_id, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            mark = len(self.text())
            self.cmd("clickid " + elem_id)
            t0 = time.time()
            while time.time() - t0 < 1.5:
                tail = self.text()[mark:]
                if "#%s rect" % elem_id in tail:
                    time.sleep(0.4)
                    return True
                if "#%s: no box" % elem_id in tail:
                    break
                time.sleep(0.1)
            time.sleep(0.3)
        return False

    def close(self):
        self.cmd("quit")
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(5)
        self.out.close()


def friend_request_flow(port, ids, tokens, names):
    """1. the friend request, its countdown and click dismissal; 2. the direct message, its expiry"""
    A, B = ids
    bob = UiClient(port, names[1], tokens[1], "bob")
    try:
        check(bool(bob.rect("menuBtnSocial", 60)), "bob: the v10 page is up with the social button")
        time.sleep(2.0)  # bob's social socket (the login) settles before anyone follows him

        # alice follows bob: the brain pushes friend:request to bob's open socket. A retry unfollows
        # first (the follow that exists makes no event)
        raised = False
        for _ in range(20):
            asyncio.run(social_do(port, names[0], ["f", names[1], 0], 0.3))
            asyncio.run(social_do(port, names[0], ["f", names[1], 1]))
            if wait(lambda: "[notify] show #1" in bob.text(), 3.0):
                raised = True
                break
        check(raised, "bob's notification center raises the friend request (\"[notify] show #1\")")
        if not raised:
            raise RuntimeError("no friend request notification (log %s)" % bob.log)
        check('"Friend Request"' in bob.text(), "the notification is titled Friend Request",
              [l for l in bob.text().splitlines() if "[notify] show #1" in l][:1])

        # the countdown: the bar narrows between two samples (10 s duration)
        time.sleep(0.6)  # the 0.3 s slide-in
        r1 = bob.rect("knNotifBar1")
        time.sleep(2.0)
        r2 = bob.rect("knNotifBar1")
        check(r1 is not None and r2 is not None and r2[2] < r1[2] - 1.0,
              "the friend request's countdown bar narrows (%s -> %s px)" % (r1 and r1[2], r2 and r2[2]))
        check(bool(bob.rect("knNotif1")), "the notification item is in the center")

        # a click dismisses it
        check(bob.click("knNotif1"), "clicking the notification")
        check(wait(lambda: "[notify] dismiss #1" in bob.text(), 5.0), "the click dismisses it (\"[notify] dismiss #1\")")
        check(bob.rect("knNotif1") is None, "the dismissed item is gone from the page")

        # bob follows alice back: they are friends; alice's direct message is a notification too
        asyncio.run(social_do(port, names[1], ["f", names[0], 1]))  # bob follows back: friends
        asyncio.run(social_do(port, names[0], ["eb", {"type": "msg:send", "to": B, "text": "hi bob"}]))
        check(wait(lambda: "[notify] show #2" in bob.text(), 10.0), "a direct message raises a notification (\"[notify] show #2\")")
        check('"Message from %s"' % names[0] in bob.text(), "the message notification names its sender")
        time.sleep(0.6)
        d1 = bob.rect("knNotifBar2")
        time.sleep(1.5)
        d2 = bob.rect("knNotifBar2")
        check(d1 is not None and d2 is not None and d2[2] < d1[2] - 1.0,
              "the message's countdown bar narrows (%s -> %s px)" % (d1 and d1[2], d2 and d2[2]))
        check(wait(lambda: "[notify] expired #2" in bob.text(), 12.0),
              "the message expires on its own after 6 s (\"[notify] expired #2\")")
        check(bob.rect("knNotif2") is None, "the expired item is gone from the page")
        shot = os.path.join(WORK, "bob-center.png")
        bob.cmd("shot " + shot)
        wait(lambda: os.path.exists(shot), 10)
    finally:
        bob.close()


# ------------------------------------------------------------------ offline input-script runs

def run_script(name, script, timeout=180, settings=None):
    """one offline client_bin run of `script` (an input script), `settings` seeded into knSettings.json;
    returns its log text"""
    if settings:
        os.makedirs(os.path.join(WORK, name + "-cfg", "krunknative"), exist_ok=True)
        with open(os.path.join(WORK, name + "-cfg", "krunknative", "knSettings.json"), "w") as f:
            json.dump(settings, f)
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_ASSETS_DIR": ASSETS,
                "KRUNKNATIVE_MODE": "offline"})  # the sandbox: every skin and emoji unlocked for a guest
    with open(log, "w") as out:
        p = subprocess.Popen([CLIENT, "--offline"], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            import signal
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(False, "%s: client_bin finished its script" % name, "timeout, log %s" % log)
    with open(log, errors="replace") as f:
        return f.read()


def emoji_flow():
    """2. the emoji panel: open on ':lo', Enter picks :love:, Enter sends, the chat line shows it"""
    log = run_script("emoji", """0 menu
1500 play
4000 down ENTER
4100 up ENTER
4300 type :lo
5000 down ENTER
5100 up ENTER
5500 down ENTER
5600 up ENTER
8000 quit
""", settings={"showMessages": True})  # Settings > Chat "Show Player Messages" (default off): the line is sent
    check('[emoji] panel open "lo"' in log, "typing :lo opens the emoji panel", "")
    check("[emoji] selected :love:" in log, "Enter picks the selected emoji (:love:) into the chat")
    check("[emoji] chat :love:" in log, "the sent message shows :love: as the emoji's image")
    # the second Enter selects, the third sends: nothing reaches the chat before the send
    sent = log.find("[emoji] chat :love:")
    picked = log.find("[emoji] selected :love:")
    check(0 <= picked < sent, "the emoji is picked before the message is sent")


def tooltip_flow():
    """3. Change Loadout > Hat picker: a hovered card shows its item tooltip above it"""
    head = """0 menu
1500 call showWindow 3
2500 clicksel [onclick*="tabIndex=2"]
4500 clicksel [onclick*="showWindow(16)"]
"""
    probe = run_script("tip-probe", head + "5500 rects @cards .skinCard[data-index]\n5700 quit\n")
    m = re.search(r"\[input_script\] rects t=\d+ @cards n=(\d+) (\S+)", probe)
    if not m or int(m.group(1)) < 1:
        check(False, "the hat picker shows skin cards", "no cards (log %s)" % os.path.join(WORK, "tip-probe.log"))
        return
    x, y, w, h = (float(v) for v in m.group(2).split("@", 1)[1].split(","))
    cx, cy = x + w / 2, y + h / 2
    shot = os.path.join(WORK, "tip-hover.png")
    log = run_script("tip-hover", head + "5500 cursor %.0f %.0f\n5700 rects @tip [id=knTip]\n5800 shot %s\n5900 quit\n" % (cx, cy, shot))
    check("[tooltip] at " in log, "hovering a hat card shows its item tooltip (\"[tooltip] at\")")
    t = re.search(r"\[input_script\] rects t=\d+ @tip n=1 \S+@([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+)", log)
    check(t is not None, "the tooltip (#knTip) is on the page")
    if t:
        tx, ty, tw, th = (float(v) for v in t.groups())
        check(tw > 0 and th > 0 and abs((ty + th) - (y - 9.0)) <= 2.5 and abs(tx - x) <= 2.5,
              "the tooltip sits 9 px above the card's top edge and left-aligned with it",
              "tip (%.0f,%.0f %.0fx%.0f) card (%.0f,%.0f)" % (tx, ty, tw, th, x, y))


def announce_flow():
    """4. the game announcement: server_bin's console sets it, a joining client shows it, clear hides it"""
    server = os.path.join(BUILD, "server_bin")
    if not os.path.exists(server):
        print("announcement flow skipped (no server_bin in %s)" % BUILD)
        return
    import signal
    (port,) = free_ports(1)
    slog = open(os.path.join(WORK, "announce-server.log"), "w")
    srv = subprocess.Popen([server, "--cfg", "none", "--port", str(port)], cwd=ROOT, stdin=subprocess.PIPE, stdout=slog,
                           stderr=subprocess.STDOUT, start_new_session=True, text=True)
    message = "Double XP weekend is live!"
    try:
        wait(lambda: "listening" in open(os.path.join(WORK, "announce-server.log"), errors="replace").read().lower(), 15)
        srv.stdin.write("announce %s\n" % message)
        srv.stdin.flush()
        wait(lambda: "Game announcement: " in open(os.path.join(WORK, "announce-server.log"), errors="replace").read(), 10)
        script = os.path.join(WORK, "announce.script")
        with open(script, "w") as f:
            f.write("0 menu\n5000 nodes @shown .game-announcement.show\n5000 grep @msg Double .ga-message\n"
                    "9000 nodes @cleared .game-announcement.show\n9500 quit\n")
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "announce-cfg"),
                    "XDG_STATE_HOME": os.path.join(WORK, "announce-state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                    "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_ASSETS_DIR": ASSETS})
        log = os.path.join(WORK, "announce.log")
        with open(log, "w") as out:
            p = subprocess.Popen([CLIENT, "--connect", "127.0.0.1:%d" % port], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, env=env, start_new_session=True)
            try:
                # the clear goes out once the client has checked the banner (t=5000 of its script, 4 s before the next check)
                wait(lambda: "@shown elements=" in open(log, errors="replace").read(), 120, 0.25)
                srv.stdin.write("announce clear\n")
                srv.stdin.flush()
                p.wait(timeout=120)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                check(False, "announce: client_bin finished its script", "timeout, log %s" % log)
        text = open(log, errors="replace").read()
        check('[notify] announcement "%s"' % message in text, "the joining client gets the server's announcement", log)
        m = re.search(r"\[input_script\] nodes t=\d+ @shown elements=\d+ match=(\d+)", text)
        check(m is not None and m.group(1) == "1", "the banner is shown (.game-announcement.show)", m.group(0) if m else log)
        m = re.search(r"\[input_script\] grep t=\d+ @msg needle=Double n=(\d+) hits=(\d+)", text)
        check(m is not None and m.group(2) == "1", "the banner carries the message line (.ga-message)", m.group(0) if m else log)
        m = re.search(r"\[input_script\] nodes t=\d+ @cleared elements=\d+ match=(\d+)", text)
        check("[notify] announcement cleared" in text and m is not None and m.group(1) == "0",
              "the console's `announce clear` hides it", m.group(0) if m else log)
    finally:
        try:
            srv.stdin.close()
        except OSError:
            pass
        os.killpg(srv.pid, signal.SIGTERM)
        try:
            srv.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(srv.pid, signal.SIGKILL)
            srv.wait()
        slog.close()


def main():
    if os.environ.get("KN_NOTIFY_SKIP_BRAIN") == "1":
        print("brain flow skipped (KN_NOTIFY_SKIP_BRAIN=1)")
    else:
        (port,) = free_ports(1)
        blog = open(os.path.join(WORK, "brain.log"), "w")
        cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
        with open(cfg, "w") as f:
            json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                       "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "notify-e2e-%d" % os.getpid()},
                       "accounts": {"signup_rewards": {"enabled": False}, "sale_grace_s": 0, "free_premium": False, "starting_kr": 0,
                                    "login_throttle_s": 0, "password_iterations": 1000,
                                    "skins_file": os.path.join(ASSETS, "skins.json")}}, f)
        brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=blog, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            for _ in range(200):
                try:
                    with open(os.path.join(WORK, "brain.log")) as f:
                        if re.search(r"listening on http://127\.0\.0\.1:%d" % port, f.read()):
                            break
                except OSError:
                    pass
                time.sleep(0.1)
            stamp = str(int(time.time()) % 100000)
            names = ["ntfal%s" % stamp, "ntfbo%s" % stamp]
            ids, tokens = [], []
            for n in names:
                i, t = asyncio.run(auth(port, n, 0))
                ids.append(i)
                tokens.append(t)
            friend_request_flow(port, ids, tokens, names)
        except Exception as e:  # noqa: BLE001
            import traceback
            traceback.print_exc()
            check(False, "exception: %r" % e)
        finally:
            brain.terminate()
            try:
                brain.wait(10)
            except subprocess.TimeoutExpired:
                brain.kill()
            blog.close()
    for flow in (emoji_flow, tooltip_flow, announce_flow):
        try:
            flow()
        except Exception as e:  # noqa: BLE001
            import traceback
            traceback.print_exc()
            check(False, "exception in %s: %r" % (flow.__name__, e))
    print("notify e2e (native): " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()
