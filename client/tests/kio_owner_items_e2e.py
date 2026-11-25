#!/usr/bin/env python3
"""A krunker.io account's streamer and skin maker items in getSkins (account_ui.c kio_build, ui_loadout.c
owned_add_free), end to end against the krunker.io stubs of server/tests/krunker_account_e2e.py. No kn_brain.

  python3 -I client/tests/kio_owner_items_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7's getSkins(account, true) (deob-10.0.7.js:217752-217795) also lists the store items an account owns by who it
is: item.streamers holds acc.twitch (tw_twitchname, :115113) lower-cased, and a skin maker (acc.isSkinMaker =
~~player_skinmaker, :115136) gets the season 6+ items no creator made. A krunker.io account carries both in the game
server's "a" (the Account constructor's keys) and in setData's row (an "ua", :115150: [23] tw_twitchname, :115174;
[42] player_skinmaker, :115193). The loadout reads acc.twitch from the account data's slot 31 (kn_brain's order) and the
skin maker flag from AccountInfo.skin_maker, so account_ui.c has to put the krunker.io account's values there.

The client starts with a stored krunker.io session, joins the stub's game, goes back to the menu and searches the Hat
picker (showWindow 16) for one item, as v10_customize_e2e.py does for a Box Office account:
  plain   the "a" has neither: Emoji Beanie (2904, streamers ["kaarson"]) and Citadel Tophat (5256, season 6, no
          creator) are not listed
  a       the "a" has tw_twitchname "KaarSon" and player_skinmaker 1: both are listed
  ua      a plain "a", then on ENTER_GAME an "ua" whose setData row has [23] "KaarSon" and [42] 1: both are listed
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
WORK = tempfile.mkdtemp(prefix="kn-kio-owner-items-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
TWITCH = "KaarSon"
ITEMS = (("Emoji", 2904, "the streamer's Emoji Beanie (item.streamers)"),
         ("Citadel", 5256, "Citadel Tophat (a skin maker's season 6+ item, no creator)"))
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


def count(log, label):
    m = re.search(r"\[input_script\] rects t=\d+ @%s n=(\d+)" % re.escape(label), log)
    return int(m.group(1)) if m else -1


def script(query, index):
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "13000 respawn",
         "14400 down ESC", "14450 up ESC",
         "15000 call showWindow 16",
         "15010 waitfor [id=itemSearch]",
         "16000 clickid itemSearch",
         "16100 type " + query,
         "16200 call searchItems",
         '17000 rects @card .skinCard[data-index="%d"]' % index,
         "17100 quit"]
    return "\n".join(s) + "\n"


def run_client(stub, name, query, index):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script(query, index))
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # a stored krunker.io session (as kio_hostmap_e2e.py): signed in at start, refreshed against the stub's FRVR auth
    for key, value in (("kio_refresh_token", kae.REFRESH1), ("kio_username", kae.ACCOUNT_NAME)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
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
        check(name + ": client finished its script", False, "timed out; log " + log)
    return read_log(log)


def ua_row():
    """setData's row (deob-10.0.7.js:115150): [6] the score, [23] twitch (:115174), [42] isSkinMaker (:115193)."""
    row = [None] * 43
    row[6] = kae.SCORE
    row[23] = TWITCH
    row[42] = 1
    return row


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_owner_items_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    if not shutil.which("xvfb-run"):
        print("kio_owner_items_e2e: no xvfb-run: skipped")
        return 77
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.players = []
    send_ua = [False]
    ua_sent = []

    async def on_enter(send, raw):  # ENTER_GAME: the "ua" case sends the account's setData row after it
        if send_ua[0]:
            await send("ua", ua_row())
            ua_sent.append(1)

    stub.frame_handlers["en"] = on_enter
    cases = (("plain", {}, False, 0), ("a", {"tw_twitchname": TWITCH, "player_skinmaker": 1}, False, 1), ("ua", {}, True, 1))
    try:
        stub.start()
        for case, data, ua, want in cases:
            for query, index, what in ITEMS:
                name = "%s-%s" % (case, query.lower())
                # this run's sign-ins get this case's "a" data (a refresh may sign in more than once)
                stub.account_data = [{}] * len(stub.a_sent) + [dict(data) for _ in range(8)]
                send_ua[0] = ua
                before_a, before_ua = len(stub.a_sent), len(ua_sent)
                text = run_client(stub, name, query, index)
                if case == "plain":
                    check("%s: the official game is the active backend" % name, "[account] active account backend: krunker.io" in text,
                          re.findall(r"\[account\] active.*", text)[:2])
                check("%s: the game server's \"a\" signed the krunker.io account in" % name, len(stub.a_sent) > before_a,
                      "a=%d" % (len(stub.a_sent) - before_a))
                if ua:
                    check("%s: the stub sent the \"ua\" on ENTER_GAME" % name, len(ua_sent) > before_ua, "ua=%d" % (len(ua_sent) - before_ua))
                label = {"plain": "a plain krunker.io account", "a": "the \"a\" with tw_twitchname and player_skinmaker",
                         "ua": "the \"ua\" row's [23] and [42]"}[case]
                check("%s: %s %s %s" % (name, label, "lists" if want else "does not list", what), count(text, "card") == want,
                      "card=%d" % count(text, "card"))
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
