#!/usr/bin/env python3
"""The punish popup's Chat Ban for a krunker.io account (client/src/ui/hud_ui.c), end to end against the krunker.io stubs of
server/tests/krunker_account_e2e.py. Nothing leaves the machine; no kn_brain.

  python3 -I client/tests/kio_chatban_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7's punishPopup asks getChatBan when it opens (GET /moderation/chat/<name>, deob-10.0.7.js:224084-224090) and its
Chat Ban button sends chatBan (POST /moderation/chat {player_name, ban}, :224104-224125), both on krunker.io's gapi with the
account's Bearer (the moderation client, :27208-27217). Before this, the krunker.io account's requests went to kn_brain,
which the gate refuses while krunker.io is the backend: the label stayed "Chat Ban" and the button did nothing.
The client starts with a stored krunker.io session (a krunker.io DEV: player_developer 1, so canChatBan), joins the stub's
game with two signed-in players, opens the player list and a player's punish popup:
  get    GET /api/moderation/chat/<name> with the Bearer; the stub says chat_banned, so the label reads "Chat Unban"
  post   the button: POST /api/moderation/chat {player_name: <name>, ban: false} with the Bearer; "Chat unbanned <name>"
         in chat and the label back to "Chat Ban"
Exit 0 when every check passed, 77 when it cannot run.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-chatban-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
OTHERS = [("sockALLY", 5, "StubAlly"), ("sockFOE", 6, "StubFoe")]
ACCID = 31  # ADD_PLAYERS' account id offset (shared/tests/krunker_proto_test.c: KRB_ADD_ACCID); the stub leaves it null
results = []
procs = []

# the other players are signed in (an account id): v10 offers Chat Ban for a level > 0 player, KrunkNative for an account
_entry = kae.player_entry


def _signed_in_entry(sock, sid, name, x, y, z, level=0):
    e = _entry(sock, sid, name, x, y, z, level=level)
    if sock != "sockME":
        e[ACCID] = 4000 + sid
    return e


kae.player_entry = _signed_in_entry
# krunker.io's answers, as new entries of the stub's tables: both players are chat banned; chatBan answers {data}
for _sock, _sid, _name in OTHERS:
    kae.GAPI_READS["/api/moderation/chat/" + _name] = {"chat_banned": True}
kae.GAPI_WRITES["/api/moderation/chat"] = {"data": {"chat_banned": False}, "error": None}


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


def script(shots):
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "13000 respawn",
         "14400 down ESC", "14450 up ESC",
         "15000 call showWindow 23",
         "15010 waitfor [onclick*=\"punishPopup\"]",
         "15500 clicksel [onclick*=\"punishPopup\"]",
         "15510 waitfor [id^=chatBanBtn_]",
         "15520 text @label0 [id^=chatBanBtn_]",
         "17000 text @label1 [id^=chatBanBtn_]",
         "17010 text @pop [id=genericPop]",
         "17020 shot " + shots["popup"],
         "17500 clicksel [id^=chatBanBtn_]",
         "19000 text @label2 [id^=chatBanBtn_]",
         "19010 text @chat [id=chatList]",
         "19020 shot " + shots["after"],
         "19500 quit"]
    return "\n".join(s) + "\n"


def run_client(stub, shots):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script(shots))
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # a stored krunker.io session (as kio_market_e2e.py): signed in at start, refreshed against the stub's FRVR auth
    for key, value in (("kio_refresh_token", kae.REFRESH1), ("kio_username", kae.ACCOUNT_NAME)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, "client.log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:9", "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off",
                "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH})
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


def checks(text, stub, shots):
    check("the official game is the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:2])
    pop = text_of(text, "pop")
    check("the krunker.io DEV's punish popup offers Chat Ban (canChatBan, a signed-in target)", "Chat" in pop and "Kick" in pop, pop[:200])
    gets = [r for r in stub.requests if r[0] == "GET" and r[1].startswith("/api/moderation/chat/")]
    name = gets[0][1].rsplit("/", 1)[1] if gets else ""
    bearer = lambda rows: bool(rows) and all(r[3].get("authorization", "") in ("Bearer " + stub.access1, "Bearer " + stub.access2) for r in rows)
    check("getChatBan: GET /api/moderation/chat/<name> on krunker.io's gapi with the Bearer, for a player of the game",
          len(gets) == 1 and bearer(gets) and name in [n for _, _, n in OTHERS], [(r[1], r[3].get("authorization", "")[:7]) for r in gets])
    check("getChatBan's chat_banned turns the label to \"Chat Unban\" (the markup says \"Chat Ban\")",
          text_of(text, "label1") == "Chat Unban", (text_of(text, "label0"), text_of(text, "label1")))
    posts = stub.find("POST", "/api/moderation/chat")
    check("chatBan: POST /api/moderation/chat {player_name, ban: false} with the Bearer",
          len(posts) == 1 and bearer(posts) and posts[0][4] == {"player_name": name, "ban": False}, [r[4] for r in posts])
    chat = text_of(text, "chat")
    check("the answer: \"Chat unbanned <name>\" in chat, the label back to \"Chat Ban\"",
          ("Chat unbanned " + name) in chat and text_of(text, "label2") == "Chat Ban", (chat[-160:], text_of(text, "label2")))
    check("no brain request for the Chat Ban (the gate)", "moderation" not in "".join(re.findall(r".*brain.*", text)),
          re.findall(r".*moderation.*", text)[:3])
    check("the screenshots were taken", all(os.path.exists(p) for p in shots.values()), shots)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_chatban_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.players = OTHERS
    stub.account_data = [{"player_developer": 1}] * 2  # isDev: canChatBan (deob-10.0.7.js:115236)
    shots = {k: os.path.join(WORK, k + ".png") for k in ("popup", "after")}
    try:
        stub.start()
        text = run_client(stub, shots)
        checks(text, stub, shots)
    finally:
        stub.stop()
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
