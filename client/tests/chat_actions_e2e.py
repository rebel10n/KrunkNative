"""A chat line's v10 actions (NET_EXT_KN_CHAT_ACTIONS, hud_ui_chat_actions) on --render-scenario fixtures.

Krunker v10.0.7's addChat gives a kind-2 line with extra.actions [[0]] a " [Join Queue]" span after its .chatMsg
(class chatMsgAction, #5ce05a, onclick handleChatMsgAction(this, 0)) when its req passes: an account of level 10 or
more (deob-10.0.7.js:228042-228050). An unknown action id and a lower level show nothing; a line without actions
has none.

  python3 -I client/tests/chat_actions_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
"""
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ROOT
BIN = os.path.join(BUILD, "client_bin")
SCRATCH = os.path.expanduser("~/.cache/krunknative/chat-actions/e2e-%d" % os.getpid())
results = []

def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def render(name, state):
    """Renders the state; returns the serialized document ("" on failure)."""
    os.makedirs(SCRATCH, exist_ok=True)
    base = {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Player", "classIndex": 0, "region": "...",
            "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji", "anim_time": 0.8, "window": None}
    base.update(state)
    path = os.path.join(SCRATCH, name + ".json")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": base}, f)
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
        return f.read()


def chat_state(score, rows):
    """A signed-in Box Office account (its level is progression_level(score)) in a game, with these chat rows."""
    return {"menu": False, "inGame": True, "chat": rows,
            "account": {"name": "Zelda", "score": score, "funds": 0, "premium": 0, "badges": [], "stats": {}, "challenges": []}}


SPAN = ('<span class="chatMsgAction" onclick="handleChatMsgAction(this, 0)" style="color: #5ce05a;"> [Join Queue]</span>')


def main():
    if not os.path.isfile(BIN):
        print("chat_actions_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    queued = {"text": "Quentin queued for ranked", "color": "#5ce05a", "actions": [0]}
    plain = {"text": "Rhea hosted a custom game", "color": "#fc03ec"}
    doc = render("high", chat_state(500000, [plain, queued]))
    check("level >= 10: one [Join Queue] link", doc.count("chatMsgAction") == 1, doc.count("chatMsgAction"))
    m = re.search(r'Quentin queued for ranked\u200e?</span>(<span class="chatMsgAction"[^>]*>[^<]*</span>)', doc)
    check("level >= 10: right after the line's .chatMsg, v10's markup", m is not None and m.group(1) == SPAN, m.group(1) if m else doc[:0])
    check("level >= 10: the line without actions has none", re.search(r"Rhea hosted a custom game</span><span", doc) is None)
    doc = render("low", chat_state(100, [queued]))
    check("a level under 10: no link (req: level >= 10)", "Quentin queued for ranked" in doc and "chatMsgAction" not in doc)
    doc = render("unknown", chat_state(500000, [{"text": "Something", "color": "#fc03ec", "actions": [7]}]))
    check("an unknown action id: nothing", "Something" in doc and "chatMsgAction" not in doc)
    failed = [r for r in results if not r[1]]
    print("chat_actions_e2e: %d/%d passed" % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
