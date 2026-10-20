#!/usr/bin/env python3
"""Hide player names on a headless client: the menu account, a scoreboard row, a chat line, Strike's
mini scoreboard and the developer console's chat log.

  python3 -I client/tests/hide_names_e2e.py BUILD_DIR      (run from the repository root)

Two scenarios (client_bin --render-scenario, headless, no window) rendered twice each: the setting
Client > Hide player names on (knClient.json hidePlayerNames true) and off (the store without it). The
DOM each render writes (KRUNKNATIVE_SCENARIO_HTML) is checked for:
  menu (signed in as Zelda, clan KN):
    on   the account header reads Player, its clan tag is gone, the picture is the menu class's
         icon (textures/classes/icon_0.png, the scenario's class 0), and the name Zelda is nowhere in the page
    off  the header reads Zelda with [KN] and the same class icon
  in game (scoreboard: Zelda is you, Quentin, Rhea, game and Players play; chat: Quentin says hi):
    on   the other players read Player_<n> (Quentin and Rhea are numbered in the order the scoreboard
         lists them; the chat line of Quentin gets the same number), you read Player, and no raw
         name is left in the page outside the text a player typed; the join, leave and ranked chat lines
         read the generic names, every chat line is printable text. A line's text is never rewritten: a
         player named "game" or "Players" says "Players joined the game, game hi" and the words stay
         (Player_3: ..., Player_4: ...), and Rhea's "Quentin, gg" keeps Quentin as typed
    off  the raw names are shown, and no Player_<n> appears
  Krunker Strike (state.strike.board, the mini scoreboard Tab holds): on, the rows read Player_1, Player (you),
    Player_2, Player_3 in score order and no raw name is left; off, the raw names are shown.
  console (stdout "[console] " lines of the in-game scenario, the chat the game received): on, the sender
    reads Player_<n> / Player and the text is echoed as typed; off, the raw names.
Exit code 0 only when every check passed. Needs a headless GL device (KRUNKNATIVE_HEADLESS=1; set
KRUNKNATIVE_EGL_DEVICE to pick one) and the checkout's assets (KRUNKNATIVE_ASSETS_DIR, default assets/; in a
worktree point it at the checkout, the font files are untracked).
"""
import copy
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
CLIENT = os.path.join(BUILD, "client_bin")
SCENARIOS = os.path.join(REPO, "tools", "uidiff", "scenarios")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-hide-names-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
# the header picture: the menu class icon (v10:209536, menu_class_index 0 in the scenario), not the stock face
CLASS_PICTURE = "textures/classes/icon_0.png"
ASSETS = ""
LAST_OUT = ""  # the stdout of the last render (the dev console's "[console] " lines)


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def scenario(base, edit, hidden, tag):
    with open(os.path.join(SCENARIOS, base), encoding="utf-8") as f:
        doc = json.load(f)
    doc = copy.deepcopy(doc)
    doc["ref"] = ""
    edit(doc["state"])
    # the Client settings store (knClient, the stored settings): a scenario's storage is its own (never the user's)
    doc["state"]["storage"] = {"knClient": json.dumps({"hidePlayerNames": True} if hidden else {})}
    path = os.path.join(WORK, tag + "-" + os.path.basename(base))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return path


def menu_edit(st):
    st["menu"] = True
    st["window"] = None
    st["inGame"] = False
    st["account"] = {"name": "Zelda", "clan": "KN", "score": 50000, "funds": 2500, "premium": 0, "level": 12,
                     "badges": [], "stats": {}, "challenges": []}


def game_edit(st):
    st["menu"] = False
    st["window"] = None
    st["inGame"] = True
    st["name"] = "Zelda"
    st["account"] = {"name": "Zelda", "clan": "KN", "score": 50000, "funds": 2500, "premium": 0, "badges": [], "stats": {}, "challenges": []}
    st["scoreboard"] = [
        {"rank": 1, "name": "Zelda", "score": 9, "me": True},
        {"rank": 2, "name": "Quentin", "score": 5, "premium": 1, "verified": 1},
        {"rank": 3, "name": "Rhea", "score": 2},
        {"rank": 4, "name": "game", "score": 1},  # a template word: "joined the game" is the server's own line
        {"rank": 5, "name": "Players", "score": 0},  # a template word: "Players (N): ..." is the server's own list header
    ]
    st["chat"] = [{"name": "Quentin", "msg": "gg, hi"},
                  {"name": "Rhea", "msg": "Quentin, gg"},
                  # a player named after a word of a line says that line's words: the sender is numbered by its uid, and
                  # the text is never searched for a name (the words stay as typed, nothing is rewritten)
                  {"name": "game", "msg": "Players joined the game, game hi"},
                  {"name": "Players", "msg": "game joined Players"},
                  # the official server's lines ("chi" [key, name]): the name is the argument, the template is never
                  # searched, so a player named "game" reads Player_3 joined the game, not a rewrite inside the words
                  {"official": "server.message.join", "args": ["Zelda"]},
                  {"official": "server.message.join", "args": ["Quentin"]},
                  {"official": "server.message.left", "args": ["Rhea"]},
                  {"official": "server.message.ranked", "args": ["Quentin"]},
                  {"official": "server.message.join", "args": ["game"]}]


def death_edit(st):
    st["menu"] = False
    st["window"] = None
    st["inGame"] = True
    st["dead"] = True
    st["name"] = "Zelda"
    st["killcard"] = {"killer": "Quentin", "level": 57, "weapon": "Assault Rifle", "classIndex": 2, "score": 120}


def chat_texts(doc):
    """The text of every chat row's message span (the LRM marks dropped): what a player reads."""
    out = []
    for m in re.finditer(r'<span class="chatMsg"[^>]*>(.*?)</span>', doc, re.S):
        out.append(m.group(1).replace("\u200e", "").replace("\u200f", ""))
    return out


def printable(text):
    return all(0x20 <= ord(c) < 0x7f or c in "\u00a0" for c in text)


def assets_root():
    """A scratch assets tree: every entry of the checkout's assets (KRUNKNATIVE_ASSETS_DIR, the untracked fonts
    included) symlinked in, so the client's on-demand user-assets land here and never in the checkout."""
    src = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(REPO, "assets")
    root = os.path.join(WORK, "assets")
    os.makedirs(root, exist_ok=True)
    for name in os.listdir(src):
        os.symlink(os.path.join(src, name), os.path.join(root, name))
    return root


def render(scenario_path, tag):
    png = os.path.join(WORK, tag + ".png")
    html = os.path.join(WORK, tag + ".html")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_SCENARIO_HTML": html, "KRUNKNATIVE_ASSETS_DIR": ASSETS})
    proc = subprocess.run([CLIENT, "--render-scenario", scenario_path, png], env=env, capture_output=True, text=True, timeout=600)
    global LAST_OUT
    LAST_OUT = proc.stdout
    if proc.returncode != 0 or not os.path.exists(html):
        print(proc.stdout[-2000:], proc.stderr[-2000:])
        return None
    with open(html, encoding="utf-8", errors="replace") as f:
        return f.read()


def console_lines(out):
    """The developer console's log lines (ui_console.c append prints each one as "[console] <text>")."""
    return [line[len("[console] "):] for line in out.splitlines() if line.startswith("[console] ")]


def console_senders(lines):
    """The sender of each console line (the text after its first ": " is what was typed, not a name); a server line whole."""
    return [line if line.startswith("Server: ") else line.split(": ", 1)[0] for line in lines]


def mini_board_names(doc):
    """The names of Strike's mini scoreboard rows, in the order the board shows them."""
    return re.findall(r'mini-score-board__name">([^<]*)<', doc)


def avatar_src(doc):
    m = re.search(r"<img[^>]*id=\"knV10Avatar\"[^>]*>", doc)
    if not m:
        return None
    s = re.search(r"src=\"([^\"]*)\"", m.group(0))
    return s.group(1) if s else None


def main():
    global ASSETS
    ASSETS = assets_root()
    menu_on = scenario("kn-account-native-default.json", menu_edit, True, "menu-on")
    menu_off = scenario("kn-account-native-default.json", menu_edit, False, "menu-off")
    game_on = scenario("ingame-scoreboard.json", game_edit, True, "game-on")
    game_off = scenario("ingame-scoreboard.json", game_edit, False, "game-off")
    death_on = scenario("ingame-death.json", death_edit, True, "death-on")
    death_off = scenario("ingame-death.json", death_edit, False, "death-off")
    strike_on = scenario(os.path.join("strike", "ingame-scoreboard.json"), lambda st: None, True, "strike-on")
    strike_off = scenario(os.path.join("strike", "ingame-scoreboard.json"), lambda st: None, False, "strike-off")

    doc = render(menu_on, "menu-on")
    if doc is None:
        check("menu on renders", False)
    else:
        check("menu on: the header reads Player", 'id="knV10Name">Player</span>' in doc and 'menuClassPlayerName">Player</span>' in doc)
        check("menu on: Zelda is nowhere in the page", "Zelda" not in doc)
        check("menu on: the clan tag KN is gone", "[KN]" not in doc and "menuClassPlayerClan" not in doc)
        check("menu on: the class icon picture", avatar_src(doc) == CLASS_PICTURE, f"src={avatar_src(doc)}")

    doc = render(menu_off, "menu-off")
    if doc is None:
        check("menu off renders", False)
    else:
        check("menu off: the header reads Zelda", "Zelda" in doc)
        check("menu off: the clan tag KN shows", "KN" in doc and "menuClassPlayerClan" in doc)
        check("menu off: the class icon picture", avatar_src(doc) == CLASS_PICTURE, f"src={avatar_src(doc)}")

    doc = render(game_on, "game-on")
    if doc is None:
        check("game on renders", False)
    else:
        check("game on: Quentin reads Player_1 on the scoreboard", "Player_1" in doc)
        check("game on: Rhea reads Player_2 on the scoreboard", "Player_2" in doc)
        check("game on: the chat line of Quentin is Player_1 too", doc.count("Player_1") >= 2, f"count={doc.count('Player_1')}")
        # the text a player typed is not a name: the chat rows' messages and the developer console's log (its own text) aside
        outside = re.sub(r'<span class="chatMsg"[^>]*>.*?</span>', "", doc, flags=re.S)
        outside = re.sub(r'<div class="logs".*?</div></div></div>', "", outside, count=1, flags=re.S)
        check("game on: no raw name is left outside a typed message (Quentin, Rhea, Zelda, game, Players)",
              not any(re.search(r"\b" + n + r"\b", outside) for n in ("Quentin", "Rhea", "Zelda")),
              f"raw names outside messages: {[n for n in ('Quentin', 'Rhea', 'Zelda') if n in outside]}")
        check("game on: you read Player on the scoreboard", 'newLeaderNameM">Player</div>' in doc)
        check("game on: the join line names Player_1", "Player_1 joined the game" in doc and "Quentin joined" not in doc)
        check("game on: your join line reads Player (not a garbled name)", "Player joined the game" in doc)
        check("game on: the leave line names Player_2", "Player_2 left the game" in doc and "Rhea left" not in doc)
        check("game on: the ranked line names Player_1", "Player_1 left for a ranked match" in doc and "Quentin left" not in doc)
        texts = chat_texts(doc)
        bad = [t for t in texts if not printable(t)]
        check("game on: every chat line is printable text", texts and not bad, f"lines {len(texts)}, odd {bad[:3]}")
        check("game on: the chat lines read exactly", all(t in texts for t in (
            "Player joined the game", "Player_1 joined the game", "Player_2 left the game", "Player_1 left for a ranked match",
            "gg, hi", "Quentin, gg", "Players joined the game, game hi", "game joined Players")),
              f"{texts}")
        check("game on: a player named game says its words as typed (no rewrite inside them)",
              "Player_3" in doc and "Players joined the game, game hi" in texts and "game joined Players" in texts, f"{texts}")
        check("game on: Quentin's premium badge is absent", ">beenhere</" not in doc, "")
        check("game on: Quentin's verified badge is absent", ">check_circle</" not in doc, "")
        console = console_lines(LAST_OUT)
        check("game on: the console's sender reads Player_1", "Player_1: gg, hi" in console, f"{console[:6]}")
        check("game on: the console keeps the text as typed (Rhea's line keeps Quentin)", "Player_2: Quentin, gg" in console, f"{console[:6]}")
        check("game on: the console's sender game reads Player_3, its words as typed",
              "Player_3: Players joined the game, game hi" in console, f"{console[:6]}")
        check("game on: the console's sender Players reads Player_4, its words as typed",
              "Player_4: game joined Players" in console, f"{console[:6]}")
        check("game on: the console's join line reads Player", "Server: Player joined the game" in console)
        check("game on: the official join line of game reads Player_3 in the chat and the console (no garbled template)",
              "Player_3 joined the game" in texts and "Server: Player_3 joined the game" in console
              and not any("Player_3 joined the Player" in t or t.startswith("game joined the game") for t in texts + console),
              f"{texts} | {console[:6]}")
        check("game on: no raw name is left in the console's senders (Quentin, Rhea, Zelda)",
              console and not any(n in s for s in console_senders(console) for n in ("Quentin", "Rhea", "Zelda")), f"{console[:6]}")

    doc = render(game_off, "game-off")
    if doc is None:
        check("game off renders", False)
    else:
        check("game off: the scoreboard shows Quentin and Rhea", "Quentin" in doc and "Rhea" in doc)
        check("game off: the chat sender is Quentin", "Quentin" in doc and "gg, hi" in doc)
        check("game off: no Player_<n> appears", not re.search(r"Player_\d", doc))
        check("game off: the join and ranked lines keep the names", "Quentin joined the game" in doc and "Quentin left for a ranked match" in doc)
        check("game off: your join and the leave line keep the names", "Zelda joined the game" in doc and "Rhea left the game" in doc)
        check("game off: the official join line keeps the name game", "game joined the game" in doc)
        console = console_lines(LAST_OUT)
        check("game off: the console keeps the sender and the text", "Quentin: gg, hi" in console and "Rhea: Quentin, gg" in console, f"{console[:6]}")
        check("game off: the player named game says its words, the console too",
              "game: Players joined the game, game hi" in console and "Players: game joined Players" in console, f"{console[:6]}")
        check("game off: the console has no Player_<n>", console and not any(re.search(r"Player_\d", line) for line in console))
        check("game off: Quentin's premium badge shows", ">beenhere</" in doc)
        check("game off: Quentin's verified badge shows", ">check_circle</" in doc)

    doc = render(death_on, "death-on")
    if doc is None:
        check("death on renders", False)
    else:
        check("death on: the killer's level is absent", "lvl 57" not in doc)
        check("death on: the killer reads Player_<n>", re.search(r'death-row-user-text">Player_\d+<', doc) is not None and "Quentin" not in doc)
    doc = render(death_off, "death-off")
    if doc is None:
        check("death off renders", False)
    else:
        check("death off: the killer's level shows", "lvl 57" in doc)
        check("death off: the killer reads Quentin", "Quentin" in doc)

    names = ["Krunky", "Guest_4821", "Botman", "Nova"]
    doc = render(strike_on, "strike-on")
    if doc is None:
        check("strike on renders", False)
    else:
        rows = mini_board_names(doc)
        check("strike on: the mini scoreboard rows read Player_1, Player (you), Player_2, Player_3 in score order",
              rows == ["Player_1", "Player", "Player_2", "Player_3"], f"rows {rows}")
        check("strike on: no raw name is left (Krunky, Botman, Nova, Guest_4821)", not any(n in doc for n in names))
    doc = render(strike_off, "strike-off")
    if doc is None:
        check("strike off renders", False)
    else:
        rows = mini_board_names(doc)
        check("strike off: the mini scoreboard shows the raw names in score order",
              rows == ["Krunky", "Guest_4821", "Botman", "Nova"], f"rows {rows}")
        check("strike off: no Player_<n> appears", not re.search(r"Player_\d", doc))

    shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"hide_names_e2e: {len(results) - failed} passed, {failed} failed")
    return 0 if failed == 0 and results else 1


if __name__ == "__main__":
    sys.exit(main())
