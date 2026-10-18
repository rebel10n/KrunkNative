#!/usr/bin/env python3
"""Hide player names on a headless client: the menu account, a scoreboard row and a chat line.

  python3 -I client/tests/hide_names_e2e.py BUILD_DIR      (run from the repository root)

Two scenarios (client_bin --render-scenario, headless, no window) rendered twice each: the setting
Client > Hide player names on (knClient.json hidePlayerNames true) and off (the store without it). The
DOM each render writes (KRUNKNATIVE_SCENARIO_HTML) is checked for:
  menu (signed in as Zelda, clan KN):
    on   the account header reads Player, its clan tag is gone, the picture is the default
         (./img/player_pfp.png), and the name Zelda is nowhere in the page
    off  the header reads Zelda with [KN] and the same default picture
  in game (scoreboard: Zelda is you, Quentin and Rhea play; chat: Quentin says hi):
    on   the other players read Player_<n> (Quentin and Rhea are numbered in the order the scoreboard
         lists them; the chat line of Quentin gets the same number), you read Player, and no raw
         name is left in the page
    off  the raw names are shown, and no Player_<n> appears
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
DEFAULT_PICTURE = "./img/player_pfp.png"
ASSETS = ""


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
    path = os.path.join(WORK, tag + "-" + base)
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
    ]
    st["chat"] = [{"name": "Quentin", "msg": "gg, hi"},
                  {"text": "Quentin joined the game", "color": "#fc03ec"},
                  {"text": "Quentin left for a ranked match", "color": "#fc03ec"}]


def death_edit(st):
    st["menu"] = False
    st["window"] = None
    st["inGame"] = True
    st["dead"] = True
    st["name"] = "Zelda"
    st["killcard"] = {"killer": "Quentin", "level": 57, "weapon": "Assault Rifle", "classIndex": 2, "score": 120}


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
    if proc.returncode != 0 or not os.path.exists(html):
        print(proc.stdout[-2000:], proc.stderr[-2000:])
        return None
    with open(html, encoding="utf-8", errors="replace") as f:
        return f.read()


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

    doc = render(menu_on, "menu-on")
    if doc is None:
        check("menu on renders", False)
    else:
        check("menu on: the header reads Player", 'id="knV10Name">Player</span>' in doc and 'menuClassPlayerName">Player</span>' in doc)
        check("menu on: Zelda is nowhere in the page", "Zelda" not in doc)
        check("menu on: the clan tag KN is gone", "[KN]" not in doc and "menuClassPlayerClan" not in doc)
        check("menu on: the default picture", avatar_src(doc) == DEFAULT_PICTURE, f"src={avatar_src(doc)}")

    doc = render(menu_off, "menu-off")
    if doc is None:
        check("menu off renders", False)
    else:
        check("menu off: the header reads Zelda", "Zelda" in doc)
        check("menu off: the clan tag KN shows", "KN" in doc and "menuClassPlayerClan" in doc)
        check("menu off: the default picture", avatar_src(doc) == DEFAULT_PICTURE, f"src={avatar_src(doc)}")

    doc = render(game_on, "game-on")
    if doc is None:
        check("game on renders", False)
    else:
        check("game on: Quentin reads Player_1 on the scoreboard", "Player_1" in doc)
        check("game on: Rhea reads Player_2 on the scoreboard", "Player_2" in doc)
        check("game on: the chat line of Quentin is Player_1 too", doc.count("Player_1") >= 2, f"count={doc.count('Player_1')}")
        check("game on: no raw name is left (Quentin, Rhea, Zelda)", not any(n in doc for n in ("Quentin", "Rhea", "Zelda")))
        check("game on: you read Player on the scoreboard", 'newLeaderNameM">Player</div>' in doc)
        check("game on: the join line names Player_1", "Player_1 joined the game" in doc and "Quentin joined" not in doc)
        check("game on: the ranked line names Player_1", "Player_1 left for a ranked match" in doc and "Quentin left" not in doc)
        check("game on: Quentin's premium badge is absent", ">beenhere</" not in doc, "")
        check("game on: Quentin's verified badge is absent", ">check_circle</" not in doc, "")

    doc = render(game_off, "game-off")
    if doc is None:
        check("game off renders", False)
    else:
        check("game off: the scoreboard shows Quentin and Rhea", "Quentin" in doc and "Rhea" in doc)
        check("game off: the chat sender is Quentin", "Quentin" in doc and "gg, hi" in doc)
        check("game off: no Player_<n> appears", not re.search(r"Player_\d", doc))
        check("game off: the join and ranked lines keep the names", "Quentin joined the game" in doc and "Quentin left for a ranked match" in doc)
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

    shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"hide_names_e2e: {len(results) - failed} passed, {failed} failed")
    return 0 if failed == 0 and results else 1


if __name__ == "__main__":
    sys.exit(main())
