#!/usr/bin/env python3
"""Developer console autofill, audited against the parser that runs each command (ui_console.c arg_candidates
and shared/src/chat_commands.c, shared/src/target_select.c, shared/src/bots.c, client/src/ui/ui_console.c locals).

  python3 -I client/tests/console_autofill_audit_e2e.py BUILD_DIR      (run from the repository root)

Each case types a line in the console of an offline Kanji client (the sandbox: its player Guest_1 is a dev and
may run every command; two bots, Blue_Bot and Red_Bot, are spawned first) and reads the hint rows
(`text @label .hints>.hintItem:nth-child(n)`; row 1 is the signature). The expected rows are what the parser
of that command accepts. Some cases press Enter and read the server's reply, so a failure shows whether the
autofill or the parser is wrong. Families: player targets (kick, ip, ping, tp, bring, kill, ...), toggles
(god, ungod, ...), maps (map, offline), class, bots, bind / alias / exec / help (free text after the command),
and the commands whose argument had no list (autofill, update, kpd).

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-autofill-audit-", dir=CACHE if os.path.isdir(CACHE) else None)
SELECTORS = ["*", "@me", "@others", "@bots", "@humans", "@team", "@enemies", "@alive", "@dead"]
results = []

# Hint cases: (key, typed line, expectation). Expectation: a list (the rows as a set), "empty" (no rows),
# ("has", [...]) (those rows among others). Keys are hyphenated words (no underscores: the row label parses them).
PLAYER_CASES = [
    ("ip-gu", "/ip gu", ["Guest_1"]),
    ("ip-guest1", "/ip guest1", ["Guest_1"]),
    ("ip-guest-space-1", "/ip guest 1", ["Guest_1"]),
    ("ip-at", "/ip @", ["@me"]),
    ("ip-star", "/ip *", "empty"),
    ("kick-at", "/kick @", SELECTORS[1:]),
    ("kick-bluebo", "/kick bluebo", ["Blue_Bot"]),
    ("kick-blue-space", "/kick blue ", "empty"),
    ("kick-blue-bot", "/kick blue bot", ["Blue_Bot"]),
    ("kill-gu", "/kill gu", ["Guest_1"]),
    ("kill-contains", "/kill uest", ["Guest_1"]),
    ("ping-gu", "/ping gu", ["Guest_1"]),
    ("tp-at", "/tp @", ["@me"]),
    ("bring-all", "/bring all", ["all"]),
    ("bring-guest-space-1", "/bring guest 1", ["Guest_1"]),
    ("banip-at", "/banip @", ["@me"]),
    ("unban-at", "/unban @", ["@me"]),
]
TOGGLE_CASES = [
    ("god-blank-has-onoff", "/god ", ("has", ["on", "off", "Guest_1"])),
    ("god-guest1", "/god guest1", ["Guest_1"]),
    ("god-blue-bot-space", "/god blue bot ", ["on", "off"]),  # "1" would read as the mode (is_toggle_literal)
    ("god-o", "/god o", ["on", "off"]),
    ("ungod-blank", "/ungod ", ["Guest_1"]),
    ("ungod-guest-space-1", "/ungod guest 1 ", "empty"),
    ("noclip-guest1", "/noclip guest1", ["Guest_1"]),
]
MAP_CASES = [
    ("map-so", "/map so", ["Soul Sanctum"]),
    ("map-soulsa", "/map soulsa", "empty"),
    ("map-burg-underscore", "/map burg_", ["burg_new", "burg_2018"]),  # official and community keys by prefix
    ("map-sanc", "/map sanc", ["sanctum"]),
    ("offline-sanc", "/offline sanc", ["sanctum"]),
    ("offline-soulsa", "/offline soulsa", ["Soul Sanctum"]),
    ("offline-soul-space-sanctum", "/offline soul sanctum", ["Soul Sanctum"]),
]
CLASS_CASES = [
    ("class-quoted", '/class "Spr', ["Spray N Pray"]),
    ("class-sprayn", "/class sprayn", ["Spray N Pray"]),
]
BOT_CASES = [
    ("botkick-bluebo", "/bot_kick bluebo", ["Blue_Bot"]),
    ("botkick-blue-underscore", "/bot_kick blue_", ["Blue_Bot"]),
    ("botkill-red-space-bot", "/bot_kill red bot", ["Red_Bot"]),
    ("botconfig-target-underscore", "/bot_config target_b", ["targetBots="]),
    ("botconfig-targetb", "/bot_config targetb", ["targetBots="]),
    ("bot-diff", "/bot diff", ["difficulty="]),
    ("summon-weep", "/summon weep", ("has", ["weep", "weeping"])),
    ("botdiff-blank", "/bot_difficulty ", ["easy", "normal", "hard", "max"]),
]
FREE_TEXT_CASES = [
    ("bind-say-hel", "/bind F1 say hel", "empty"),
    ("bind-say-blank", "/bind F1 say ", "empty"),
    ("alias-say-he", "/alias hi say he", "empty"),
    ("exec-say-he", "/exec say he", "empty"),
    ("exec-after-semi", "/exec ping; he", ["help"]),
    ("exec-semi-say-he", "/exec ping; say he", "empty"),
    ("help-second-arg", "/help ping pi", "empty"),
]
LIST_CASES = [
    ("autofill-blank", "/autofill ", ["on", "off"]),
    ("autofill-o", "/autofill o", ["on", "off"]),
    ("update-blank", "/update ", ["check", "now", "download"]),
    ("strike-blank", "/strike_reward ", ["battlepass"]),
    ("kpd-blank", "/kpd ", ["apps"]),
    ("kpd-x-blank", "/kpd apps ", "empty"),
    ("toggle-god-blank", "/toggle god ", ["Guest_1", "on", "off"]),
    ("toggle-god-gu", "/toggle god gu", ["Guest_1"]),
    ("toggle-god-blue-bot-space", "/toggle god blue bot ", ["on", "off"]),
    ("toggle-ungod-blank", "/toggle ungod ", ["Guest_1"]),
]
LINES = PLAYER_CASES + TOGGLE_CASES + MAP_CASES + CLASS_CASES + BOT_CASES + FREE_TEXT_CASES + LIST_CASES

# Enter cases: (key, typed line, substring of the server's reply line "[console] Server: ..." that the parser gives)
ENTER_CASES = [
    ("enter-ip-guest1", "/ip guest1", "[console] Server: Guest_1: "),
    ("enter-ip-guest-space-1", "/ip guest 1", "[console] Server: Guest_1: "),
    ("enter-ungod-guest-space-1", "/ungod guest 1", "[console] Server: God mode OFF"),
    ("enter-ungod-guest-1-on", "/ungod guest 1 on", "[console] Server: God mode OFF"),  # Enter over the "on" hint sends the line
    ("enter-toggle-god-guest-1-on", "/toggle god guest 1 on", "[console] Server: God mode ON"),  # toggle keeps its arguments
    ("enter-update-download", "/update download", "[console] opening the client download"),
]
TAB_CASES = [
    ("tab-map-soul", "/map so", '/map "Soul Sanctum" '),
    ("tab-class-sprayn-quoted", "/class sprayn", '/class "Spray N Pray" '),
]


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def typing(text):
    """The script lines that type `text` in the focused input: a space is a SPACE key (the give harness's note)."""
    lines = []
    parts = text.split(" ")
    for j, part in enumerate(parts):
        if j:
            lines.append("down SPACE\nup SPACE")
        if part:
            lines.append(f"type {part}")
    return lines


def typed_line(out, t, text):
    out.append(f"{t} down CTRL+A"); out.append(f"{t + 10} up CTRL+A"); t += 40
    for line in typing(text):
        for part in line.split("\n"):
            out.append(f"{t} {part}"); t += 12
    return t


def build_script(lines, enters, tabs, bots):
    out = ["0 play", "200 down GRAVE", "220 up GRAVE"]
    t = 400
    if bots:  # the bots the cases name: spawned by the sandbox's bot command (a dev's line)
        for bot in ("/bot 1 name=Blue_Bot", "/bot 1 name=Red_Bot"):
            t = typed_line(out, t, bot)
            out.append(f"{t} down ENTER"); out.append(f"{t + 10} up ENTER"); t += 1500
    t += 1500  # the chat limiter allows 3 lines a second (net_chat_allow): the sends are spaced
    for key, text, _ in lines:
        t = typed_line(out, t, text)
        t += 250  # the hint rows update a few frames after the last key (a debug client: ~0.5 s a frame)
        for n in range(1, 13):  # row 1 is the signature, then up to 11 candidates
            out.append(f"{t} text @row_{key}_{n} .hints>.hintItem:nth-child({n})"); t += 6
        t += 20
    for key, text, _ in enters:
        t = typed_line(out, t, text)
        out.append(f"{t} down ENTER"); out.append(f"{t + 10} up ENTER"); t += 1500
    for key, text, _ in tabs:
        t = typed_line(out, t, text)
        t += 40
        out.append(f"{t} down TAB"); out.append(f"{t + 10} up TAB"); t += 60
        out.append(f"{t} text @tab_{key} .row input"); t += 30
    out.append(f"{t} quit")
    return "\n".join(out) + "\n"


def run(group, lines, enters, tabs, bots, timeout=900):
    script = os.path.join(WORK, f"audit-{group}.script")
    with open(script, "w") as f:
        f.write(build_script(lines, enters, tabs, bots))
    cfg = os.path.join(WORK, "cfg")
    state = os.path.join(WORK, "state")
    os.makedirs(cfg, exist_ok=True)
    os.makedirs(state, exist_ok=True)
    log = os.path.join(WORK, f"client-{group}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": state,
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{group}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read()


ROW = re.compile(r"\[input_script\] text t=\d+ @row_(\S+?)_(\d+) n=(\d+) checked=\d value=.*? text=(.*)$")
TAB = re.compile(r"\[input_script\] text t=\d+ @tab_(\S+) n=(\d+) checked=\d value=(.*) text=")


def read_rows(text):
    """{case key: (signature row, candidate rows)} and {tab key: the input's value after Tab}."""
    rows, tabs = {}, {}
    for line in text.splitlines():
        m = ROW.search(line)
        if m:
            key, n, count, shown = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4).strip()
            if count:
                rows.setdefault(key, {})[n] = shown
            continue
        m = TAB.search(line)
        if m:
            tabs[m.group(1)] = m.group(3)
    out = {}
    for key, by_n in rows.items():
        names = []
        for n in range(2, 13):
            if n not in by_n:
                break
            names.append(by_n[n])
        out[key] = (by_n.get(1, ""), names)
    return out, tabs


def matches(names, expect):
    if expect == "empty":
        return not names
    if isinstance(expect, tuple) and expect[0] == "has":
        return all(x in names for x in expect[1])
    return sorted(names) == sorted(expect)


def describe(expect):
    if expect == "empty":
        return "no rows"
    if isinstance(expect, tuple):
        return "rows including " + ", ".join(expect[1])
    return "rows " + ", ".join(expect)


# The cases run in batches (one client each): a debug client at ~0.6 s a frame cannot take the whole list
# in one run. Each batch checks its own hint rows, Enter replies and Tabs.
# (group, hint cases, Enter cases, Tab cases, bots spawned first)
GROUPS = [
    ("players", PLAYER_CASES, [], [], True),
    ("toggles", TOGGLE_CASES, [], [], False),
    ("maps", MAP_CASES, [], TAB_CASES[:1], False),
    ("class", CLASS_CASES, [], TAB_CASES[1:], False),
    ("bots", BOT_CASES, [], [], True),
    ("free-text-lists", FREE_TEXT_CASES + LIST_CASES, [], [], False),
    ("enter", [], ENTER_CASES, [], True),
]


def enter_reply(log_lines, typed, want):
    """Whether the reply `want` came after the console echoed `typed` (up to the next echo), and those lines."""
    echo = "[console] ] " + typed
    starts = [i for i, line in enumerate(log_lines) if line.endswith(echo) or line == echo]
    if not starts:
        return False, []
    i = starts[0] + 1
    j = i
    while j < len(log_lines) and "[console] ] " not in log_lines[j]:
        j += 1
    block = log_lines[i:j]
    return any(want in line for line in block), block


def batch_started(text, lines, enters):
    """The client ran the batch's script, its first hint line read rows and each Enter line was echoed."""
    if "[input_script] " not in text or "events from" not in text:
        return False
    if lines and lines[0][0] not in read_rows(text)[0]:
        return False
    return all("[console] ] " + typed in text for _, typed, _ in enters)


def main():
    only = sys.argv[2:]  # optional: the batch names to run (players, toggles, maps, class, bots, free-text-lists, enter)
    for group, lines, enters, tabs, bots in GROUPS:
        if only and group not in only:
            continue
        if not lines and not enters and not tabs:
            continue
        # a batch whose client did not run its script, or whose first hint line never appeared, is run again
        # once: the debug client sometimes stalls at startup (the console then reads no rows at all)
        for attempt in (1, 2):
            text = run(group, lines, enters, tabs, bots)
            if batch_started(text, lines, enters):
                break
            if attempt == 1:
                print(f"NOTE  {group}: the batch did not start (retrying once)", flush=True)
        check(f"{group}: client_bin ran the script", "[input_script] " in text and "events from" in text, "")
        rows, tab_values = read_rows(text)
        for key, typed, expect in lines:
            if key not in rows:
                check(f"{typed!r}: the hint rows were read", False, "no rows (the console did not open?)")
                continue
            sig, names = rows[key]
            check(f"{typed!r} gives {describe(expect)}", matches(names, expect), f"got {names}")
        log_lines = text.splitlines()
        for key, typed, want in enters:
            ok, block = enter_reply(log_lines, typed, want)
            check(f"Enter {typed!r} -> the reply {want!r}", ok, f"lines after the echo: {block[:6]}")
        for key, typed, want in tabs:
            shown = tab_values.get(key)
            check(f"Tab completes {typed!r} to {want!r}", shown == want, f"value {shown!r}")


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
finally:
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}", flush=True)
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)
