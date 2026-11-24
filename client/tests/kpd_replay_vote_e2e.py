#!/usr/bin/env python3
"""A K.P.D replay's verdict, end to end (doc/v10/replays.md): a demo server_bin recorded is uploaded to a live
kn_brain as the recorder does (the "acct" ops kpd_demo_chunk / kpd_demo_done of a K.P.D call), then a signed-in
officer's headless client_bin opens it (watchDemo -> GET /kpd/demos/<tok>/download), plays it and gives its
verdict from the replay bar (RESOLVE -> HACKER? YES -> POST /kpd/demos/<tok>/vote). The vote must land: the brain's
kpd_demo_votes row, the case's counts in the Demos list, the bar's own answer.

  python3 -I client/tests/kpd_replay_vote_e2e.py BUILD_DIR      (repository root)

The demo is recorded first by server/tests/kpd_demo_e2e.py (Sandstorm). 77 = skip (no kn_brain, client_bin,
server_bin or python websockets). Logs are kept in ~/.cache/kn-replay-vote-* on failure.
"""
import asyncio
import base64
import glob
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
SERVER = os.path.join(BUILD, "server_bin")
CLIENT = os.path.join(BUILD, "client_bin")
for b in (BRAIN, SERVER, CLIENT):
    if not os.path.exists(b):
        print("kpd_replay_vote_e2e: %s missing: skip" % b)
        sys.exit(77)
try:
    import websockets
except ImportError:
    print("kpd_replay_vote_e2e: python websockets missing: skip")
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
import kn_demo  # noqa: E402
from kn_proto import free_ports  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-replay-vote-", dir=CACHE if os.path.isdir(CACHE) else None)
SECRET = "kn-replay-vote-%d-xxxxxxxxxxxxxxxxxxxxxxxx" % os.getpid()
STAMP = os.getpid() % 100000
COP, REPORTER, SUSPECT, PASS = "cop%d" % STAMP, "rep%d" % STAMP, "sus%d" % STAMP, "kpdpass12"
DB = os.path.join(WORK, "brain.db")
UPLOADS = os.path.join(WORK, "uploads")
TOK, CT = "5eed0a1b2c3d", "c0ffee123456"
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + str(detail)) if detail and not ok else ""), flush=True)


def sql(q, *a):
    con = sqlite3.connect(DB, timeout=10)
    try:
        rows = con.execute(q, a).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def wait(cond, timeout=30, step=0.25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            v = cond()
        except Exception:  # noqa: BLE001
            v = None
        if v:
            return v
        time.sleep(step)
    return None


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, PASS, 0], "captcha"]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 15))
            if f[0] == "a":
                return f


async def node_ops(port, ops):
    """The game server's side of /mm/internal/ws: "acct" ops in order -> [(err, data)]"""
    out = []
    async with websockets.connect("ws://127.0.0.1:%d/mm/internal/ws" % port, additional_headers={"Authorization": "Bearer " + SECRET},
                                  max_size=8 << 20) as ws:
        await ws.send(json.dumps({"type": "hello", "nodeId": "KPDV", "region": "local", "publicHost": "127.0.0.1",
                                  "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(ws.recv(), 10)).get("type") != "welcome":
            pass
        for rid, (op, args) in enumerate(ops, 1):
            if callable(args):
                args = args(out)
            await ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
            while True:
                msg = json.loads(await asyncio.wait_for(ws.recv(), 15))
                if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                    out.append((msg.get("err"), msg.get("data")))
                    break
    return out


def record_demo():
    keep = os.path.join(WORK, "demos")
    r = subprocess.run([sys.executable, "-I", "server/tests/kpd_demo_e2e.py", BUILD, "Sandstorm", keep], cwd=ROOT,
                       capture_output=True, text=True, timeout=300, env=dict(os.environ, TMPDIR=WORK))
    files = sorted(glob.glob(os.path.join(keep, "sandstorm-*.kndemo")))
    check("server_bin recorded a Sandstorm demo (kpd_demo_e2e)", r.returncode == 0 and files, r.stdout[-400:])
    return open(files[0], "rb").read() if files else None


LOGIN = """0 menu
500 clickid knV10LoginBtn
800 clickid knV10LoginMode
1000 clickid accName
1100 type {name}
1200 clickid accPass
1300 type {pw}
1400 down ENTER
1410 up ENTER
"""


def run_client(brain_url, game_port, timeout=240):
    script = LOGIN.format(name=COP, pw=PASS) + """
4000 call watchDemo "{tok}" "{ct}"
4100 waitfor [id=replayResolveBtn]
4400 down ESC
4410 up ESC
4600 text @before [id=replayResolve]
4700 clickid replayResolveBtn
5000 text @question [id=replayResolve]
5100 clickid replayVoteY
8000 text @after [id=replayResolve]
8100 text @suspect [id=replaySusBtn]
8200 text @time [id=replayTime]
8210 clickid replaySpeed
8220 clickid replayPlayBtn
8260 text @speed [id=replaySpeed]
8270 text @play [id=replayPlayBtn]
8300 clickid replayExitBtn
9500 quit
""".format(tok=TOK, ct=CT)
    path = os.path.join(WORK, "vote.script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    after_exit = os.path.join(WORK, "after-exit.script")
    with open(after_exit, "w") as f:
        # The clock does not run until the first event is `menu` (a bare quit waits for a spawn, and the
        # relaunch's hot seek has no server). menu starts it; quit once the page is up.
        f.write("0 menu\n500 quit\n2000 quit\n8000 quit\n")
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY") and (not k.startswith("KRUNKNATIVE_") or k == "KRUNKNATIVE_EGL_DEVICE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH": after_exit, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_DEBUG": "1",
                "KRUNKNATIVE_BRAIN_URL": brain_url, "KRUNKNATIVE_REPLAY_LOG": "1",
                "KRUNKNATIVE_EGL_DEVICE": os.environ.get("KRUNKNATIVE_EGL_DEVICE", "mesa"),
                # nothing reaches production: no server finder, krunker.io's hosts are a dead local port
                "KRUNKNATIVE_FINDER_URL": "off", "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:9",
                "KRUNKNATIVE_KRUNKER_SOCIAL": "http://127.0.0.1:9/social", "KRUNKNATIVE_KRUNKER_EDITOR": "http://127.0.0.1:9/editor"})
    with open(log, "w") as out:
        p = subprocess.Popen([CLIENT, "--mode", "online", "--brain", brain_url, "--finder", "off", "--connect", "127.0.0.1:%d" % game_port],
                             cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        deadline = time.time() + timeout
        reloaded_at = None
        while p.poll() is None:
            now = time.time()
            if now >= deadline:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                check("client_bin finished its script", False, "timeout, log " + log)
                break
            try:
                with open(log, errors="replace") as seen_f:
                    seen = "[app] reload: restarting" in seen_f.read()
            except OSError:
                seen = False
            if seen and reloaded_at is None:
                reloaded_at = now
            # Exit Replay is location.reload(). The new process then hot-seeks and, with no lobby, never
            # becomes game-ready, so its script clock (and quit) never starts. The reload line is the result.
            if reloaded_at is not None and now - reloaded_at > 20:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                break
            time.sleep(0.5)
    with open(log, errors="replace") as f:
        return f.read()


def text(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=\d value=.*? text=(.*)", log)
    return re.sub(r"\s+", " ", m.group(2)).strip() if m else None


def scenario(brain_port, brain_url, game_port):
    demo = record_demo()
    if not demo:
        return
    _, meta, _ = kn_demo.parse(demo)
    regs = {}
    for name in (COP, REPORTER, SUSPECT):
        reg = asyncio.run(register(brain_port, name))
        check("registered " + name, reg and reg[1] is None, reg)
        regs[name] = reg[2] if reg else 0
    sql("UPDATE players SET player_moderator = 1 WHERE player_id = ?", regs[COP])  # a K.P.D officer

    # the recorder's upload of a call's demo (server/src/demo_rec.c rec_upload): the call, the slices, done
    half = (len(demo) + 1) // 2
    ops = [("kpd_call", dict(id=regs[REPORTER], name=REPORTER, level=12, suspect=regs[SUSPECT], suspect_name=SUSPECT,
                             lobby="KPDV:ab12c", node="KPDV", region="local", map="Sandstorm", mode=0, typ=0)),
           ("kpd_demo_chunk", dict(tok=TOK, off=0, data=base64.b64encode(demo[:half]).decode())),
           ("kpd_demo_chunk", dict(tok=TOK, off=half, data=base64.b64encode(demo[half:]).decode())),
           ("kpd_demo_done", lambda out: dict(tok=TOK, size=len(demo), call=(out[0][1] or {}).get("id", 0), ct=CT,
                                               suspect=regs[SUSPECT], suspect_name=SUSPECT, typ=0, map="Sandstorm",
                                               mode="Free for All", dur=meta.get("duration", 0)))]
    res = asyncio.run(node_ops(brain_port, ops))
    check("the K.P.D call and the demo upload are accepted", all(err is None for err, _ in res), res)
    row = sql("SELECT id, opened, yes_votes, no_votes FROM kpd_demos WHERE token = ?", TOK)
    check("kpd_demos has the case, open, no votes", row and row[0][1] == 1 and row[0][2] == 0 and row[0][3] == 0, row)

    log = run_client(brain_url, game_port)
    check("the officer signed in", "[account] logged in as %s" % COP in log, "log in " + WORK)
    check("watchDemo downloaded and started the replay", re.search(r"\[replay\] demo %s: \d+ records" % TOK, log), "log in " + WORK)
    check("the replay played (its playhead moved)", re.search(r"^\[replay\] \d+ / \d+ ms", log, re.M))
    before, question, after = text(log, "@before") or "", text(log, "@question") or "", text(log, "@after") or ""
    check("the bar offers RESOLVE", "RESOLVE" in before, before)
    check("RESOLVE asks HACKER? (YES / NO)", "HACKER?" in question and "YES" in question and "NO" in question, question)
    check("the bar shows the vote's answer: 1Y 0N and the officer's dot", "1Y" in after and "0N" in after, after)
    check("the suspect button names the anonymised suspect", (text(log, "@suspect") or "").startswith("SUSPECT: Player_"),
          text(log, "@suspect"))
    check("the speed button cycles the speed (1x -> 2x)", text(log, "@speed") == "2x", text(log, "@speed"))
    check("the play button pauses", text(log, "@play") == "play_arrow", text(log, "@play"))
    check("Exit Replay reloads the client", "[app] reload: restarting" in log)

    votes = sql("SELECT v.officer_id, v.verdict FROM kpd_demo_votes v JOIN kpd_demos d ON d.id = v.demo_id WHERE d.token = ?", TOK)
    check("the vote landed: kpd_demo_votes (officer, verdict 1)", votes == [(regs[COP], 1)], votes)
    row = sql("SELECT yes_votes, no_votes FROM kpd_demos WHERE token = ?", TOK)
    check("the case counts it (yes 1, no 0)", row and row[0] == (1, 0), row)
    res = asyncio.run(node_ops(brain_port, [("kpd_demos", dict(off=0, q=SUSPECT))]))
    rows = [d for d in ((res[0][1] or {}).get("demos", []) if res and res[0][0] is None else []) if d.get("tok") == TOK]
    check("the Demos list (kpd_demos) shows the case with 1 yes", rows and rows[0].get("yes") == 1, res)

def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, game_port = free_ports(2)
    brain_url = "http://127.0.0.1:%d" % brain_port
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET)
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--database.driver=sqlite",
                                       "--database.path=" + DB, "--migrate", "--brain.public_url=" + brain_url,
                                       "--accounts.login_throttle_s=0", "--uploads.dir=" + UPLOADS],
                                      cwd=ROOT, env=env, stdout=open(os.path.join(WORK, "brain.log"), "w"),
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True))
        up = wait(lambda: b'"brain"' in urllib.request.urlopen(brain_url + "/mm/status", timeout=1).read(), 20, 0.1)
        check("kn_brain is up", up)
        # the menu's own game (the client's --connect): the replay leaves it
        procs.append(subprocess.Popen([SERVER, "--headless", "--cfg", "none", "--port", str(game_port)], cwd=ROOT,
                                      stdout=open(os.path.join(WORK, "server.log"), "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        scenario(brain_port, brain_url, game_port)
    except Exception as e:  # noqa: BLE001
        check("exception", False, repr(e))
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(20)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
    ok = results and all(results)
    print("%s: %d/%d" % ("PASS" if ok else "FAIL", sum(results), len(results)))
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
