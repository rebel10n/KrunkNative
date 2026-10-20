#!/usr/bin/env python3
"""Hide player names in a live offline game: the scoreboard shows the other players as Player_<n> and you as
Player (the server's names never reach the scoreboard), with the setting on; the names back with it off. The
join and leave chat lines read each row exactly: Player_<n> (you: Player) with the setting on, the names with it off.

  python3 -I client/tests/hide_names_live_e2e.py BUILD_DIR      (run from the repository root)

An offline Kanji game (headless client_bin, the sandbox server in-process, fixed 60 fps step) with three bots
(random names) and two named bots with join and leave messages: "game" (a word of the server's own lines, so a
name matched in the text would be rewritten inside "joined the game") and "Zorbo". The scoreboard rows
(#leaderContainer, .leaderItem) are read through the input script's DOM probes; the chat rows ([id^=chatMsg_]) are
read whole by the "rows" probe. Run twice: the Client switch hidePlayerNames on (knClient.json in a private
XDG_CONFIG_HOME) and off.
Needs a headless GL device (KRUNKNATIVE_EGL_DEVICE picks it) and the checkout's assets (KRUNKNATIVE_ASSETS_DIR).
Exit code 0 only when every check passed.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-hide-live-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []
# a join / leave line with the setting on: the player's shown name (you: Player; the others: Player_<n>)
JOIN_HIDDEN = re.compile(r"(Player|Player_\d+) joined the game")
LEAVE_HIDDEN = re.compile(r"Player_\d+ left the game")


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def probe(lines, cmd, label):
    for l in lines:
        if l.startswith(f"[input_script] {cmd} ") and f" {label} " in l + " ":
            return dict(kv.split("=", 1) for kv in l.split() if "=" in kv)
    return None


def rows(lines, label):
    """The whole text of each row the "rows" probe read under `label`, in document order (the bidi marks the
    chat rows wrap their text in are dropped)."""
    out = []
    for l in lines:
        if l.startswith("[input_script] row ") and f" {label} " in l + " " and " text=" in l:
            out.append(l.split(" text=", 1)[1].replace("‎", "").replace("‏", ""))
    return out


def run(tag, hidden):
    cfg = os.path.join(WORK, tag + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knClient.json"), "w", encoding="utf-8") as f:
        json.dump({"hidePlayerNames": True} if hidden else {}, f)
    script = os.path.join(WORK, tag + ".script")
    with open(script, "w", encoding="utf-8") as f:
        f.write("\n".join([
            "0 play",
            "300 con bot_add 3 leaveMessage=1",
            "300 con bot_add 1 joinMessage=1 leaveMessage=1 name=game",
            "300 con bot_add 1 joinMessage=1 leaveMessage=1 name=Zorbo",
            "5000 nodes @rows [class^=leaderItem]",
            "5000 grep @pn Player_ [class^=leaderName]",
            "5000 grep @me Player [class^=leaderNameM]",
            "5000 rows @joins [id^=chatMsg_]",
            "5200 con bot_kick all",
            "5600 rows @leaves [id^=chatMsg_]",
            "5700 quit",
        ]) + "\n")
    log = os.path.join(WORK, tag + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": "16.667", "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSETS_DIR": os.environ.get("KRUNKNATIVE_ASSETS_DIR", "/home/user/src/KrunkNative/assets")})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        try:
            p.wait(timeout=300)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()
    rows_probe = probe(lines, "nodes", "@rows")
    pn = probe(lines, "grep", "@pn")
    me = probe(lines, "grep", "@me")
    n_rows = int(rows_probe["match"]) if rows_probe else -1
    n_pn = int(pn["hits"]) if pn else -1
    n_me = int(me["hits"]) if me else -1
    # every row that is a join or a leave (by its verb), so a garbled one is checked too, not dropped
    join_lines = [t for t in rows(lines, "@joins") if " joined " in t]
    leave_lines = [t for t in rows(lines, "@leaves") if " left " in t]
    if hidden:
        check("on: the scoreboard lists you and the three bots", n_rows >= 4, f"rows {n_rows}")
        check("on: the bots read Player_<n> on the scoreboard", n_pn >= 3, f"Player_ rows {n_pn}")
        check("on: your own row reads Player", n_me == 1, f"Player rows {n_me}")
        check("on: every join line names you Player or the bot Player_<n> (game and Zorbo too)",
              len(join_lines) == 3 and all(JOIN_HIDDEN.fullmatch(t) for t in join_lines)
              and sum(t == "Player joined the game" for t in join_lines) == 1, f"join lines {join_lines}")
        # the bots leave after their LEAVE packet removed them from the players: their leave lines are still hidden
        check("on: every leave line reads Player_<n> left the game (the three seeded bots and the two named ones)",
              len(leave_lines) == 5 and all(LEAVE_HIDDEN.fullmatch(t) for t in leave_lines), f"leave lines {leave_lines}")
    else:
        check("off: the bots are not renamed (no Player_<n> on the scoreboard)", n_pn == 0, f"Player_ rows {n_pn}")
        check("off: the scoreboard lists you and the three bots", n_rows >= 4, f"rows {n_rows}")
        check("off: the join lines keep the names (game and Zorbo included)",
              "game joined the game" in join_lines and "Zorbo joined the game" in join_lines
              and not any("Player_" in t for t in join_lines), f"join lines {join_lines}")
        check("off: the leave lines keep the names (game and Zorbo included)",
              "game left the game" in leave_lines and "Zorbo left the game" in leave_lines
              and not any("Player_" in t for t in leave_lines), f"leave lines {leave_lines}")
    return lines


def main():
    try:
        run("on", True)
        run("off", False)
    finally:
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
        shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"hide_names_live_e2e: {len(results) - failed} passed, {failed} failed")
    return 0 if failed == 0 and results else 1


if __name__ == "__main__":
    sys.exit(main())
