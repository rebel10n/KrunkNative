"""v10.0.7's vote kick update on a krunker.io lobby (VOTE_KICK [voter, votes, needed], deob-10.0.7.js:226852-226855), on
--render-scenario fixtures (client/src/ui/hud_votekick.c hud_votekick_on_state, ui_scenario.c state.voteKick.update).

A vote opened by another player ("Quentin voted to kick Rhea", the Kick row and the keys), then krunker.io's update:
  - our own vote (the update's alias is ours): "Vote Submitted - 2/3" in the chat (updateGameMessage(-1, null, .., 2)),
    the name and keys rows hidden, #voteKickNum "Votes: 2/3", the box still up;
  - another player's vote: no chat line, the box with "Votes: 2/3";
  - the same update on our own server (not official): no client line (server/src/votekick.c says it to the voter).

  python3 -I client/tests/votekick_official_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
"""
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ROOT
BIN = os.path.join(BUILD, "client_bin")
SCRATCH = os.path.expanduser("~/.cache/krunknative/votekick-official/e2e-%d" % os.getpid())
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def render(name, state):
    """Renders the state; returns the serialized document ("" on failure)."""
    os.makedirs(SCRATCH, exist_ok=True)
    base = {"menu": False, "window": None, "connected": True, "ready": True, "inGame": True, "mapName": "Kanji",
            "mapFile": "maps/311_kanji.json", "modeName": "Free for All", "name": "Local User", "region": "...", "anim_time": 0.8,
            "chat": [], "hud": {"health": 100, "maxHealth": 100},
            "account": {"name": "Local User", "score": 200000, "funds": 500}}
    base.update(state)
    path = os.path.join(SCRATCH, name + ".json")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": base, "frames": 3}, f)
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    html = os.path.join(SCRATCH, name + ".html")
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_DEBUG="1", XDG_CONFIG_HOME=os.path.join(SCRATCH, "cfg-" + name),
               KRUNKNATIVE_SCENARIO_HTML=html)
    png = os.path.join(SCRATCH, name + ".png")
    r = subprocess.run([BIN, "--render-scenario", path, png], cwd=SRC, env=env, capture_output=True, text=True, timeout=180)
    check(name + ": client_bin exits 0", r.returncode == 0, (r.stdout + r.stderr)[-800:] if r.returncode else "")
    if not os.path.isfile(html):
        check(name + ": the document was written", False)
        return ""
    with open(html, encoding="utf-8", errors="replace") as f:
        return f.read().replace("'", '"')


def element(doc, elem_id):
    """The opening tag of #elem_id"""
    m = re.search(r'<[a-z]+[^>]*\bid="%s"[^>]*>' % re.escape(elem_id), doc)
    return m.group(0) if m else ""


def inner_text(doc, elem_id):
    m = re.search(r'<([a-z]+)[^>]*\bid="%s"[^>]*>(.*?)</\1>' % re.escape(elem_id), doc, re.S)
    return re.sub(r"<[^>]+>", "", m.group(2)).strip() if m else None


def hidden(tag):
    return re.search(r"display:\s*none", tag) is not None


def main():
    if not os.path.isfile(BIN):
        print("votekick_official_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    vote = {"target": "Rhea", "voter": "Quentin", "votes": 1, "needed": 3}

    doc = render("own-vote", {"official": True, "voteKick": dict(vote, update={"voter": "@me", "votes": 2})})
    check("official, our own vote: \"Vote Submitted - 2/3\" in the chat", "Vote Submitted - 2/3" in doc)
    check("official, our own vote: the name and keys rows hidden", hidden(element(doc, "voteKickName")) and hidden(element(doc, "voteKickKeys")),
          "%s %s" % (element(doc, "voteKickName"), element(doc, "voteKickKeys")))
    check("official, our own vote: #voteKickNum \"Votes: 2/3\", the box still up",
          inner_text(doc, "voteKickNum") == "Votes: 2/3" and not hidden(element(doc, "voteToKick")),
          "%s %s" % (inner_text(doc, "voteKickNum"), element(doc, "voteToKick")))

    doc = render("other-vote", {"official": True, "voteKick": dict(vote, update={"voter": "Rhea2", "votes": 2})})
    check("official, another player's vote: no chat line, the box with Votes: 2/3",
          "Vote Submitted" not in doc and inner_text(doc, "voteKickNum") == "Votes: 2/3" and not hidden(element(doc, "voteToKick")),
          inner_text(doc, "voteKickNum"))
    check("official, another player's vote: the Kick row and keys stay for us", not hidden(element(doc, "voteKickName")),
          element(doc, "voteKickName"))

    doc = render("own-server", {"voteKick": dict(vote, update={"voter": "@me", "votes": 2})})
    check("our own server: no client \"Vote Submitted\" line (the server sends it to the voter)", doc and "Vote Submitted" not in doc)

    failed = [r for r in results if not r[1]]
    print("votekick_official_e2e: %d/%d passed" % (len(results) - len(failed), len(results)))
    if not failed:
        shutil.rmtree(SCRATCH, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
