#!/usr/bin/env python3
"""Krunker Box Office shows Krunker's version and nothing of its own (AGENTS.md "Krunker Parity").

  python3 -I client/tests/version_menu_e2e.py BUILD_DIR      (run from the repository root)

Headless (client_bin --render-scenario, no window; the serialized DOM through KRUNKNATIVE_SCENARIO_HTML):
  1. `client_bin --version` names the build by its commit (or "no commit") and never by a describe
     string (v1.0.0-935-g400f0079) or a v1.x version: the build is identified by its commit only;
  2. the menu's What's New tile (#updateAd, a signed-in account not yet viewing it) shows exactly one
     version, the newest update of ui/windows/kn-versions.txt, and that is Krunker's line (the same major
     as the newest update of Krunker's own changelog, ui/windows/versions.txt): not v1.x;
  3. the Outdated / Update available box (#knUpdateAd, state.update) names this client by its commit
     7 digits ("This client: 400f007") and the newer build by the label the scenario gives (a commit),
     never a describe string; the box's own text carries no version.
Exit 0 when every check passes, 1 otherwise.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from html.parser import HTMLParser

ROOT = os.getcwd()
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
CLIENT = os.path.join(BUILD, "client_bin")
WINDOWS = os.path.join(ROOT, "ui", "windows")
DESCRIBE = re.compile(r"v\d+\.\d+\.\d+-\d+-g[0-9a-f]+")
VERSION_TOKEN = re.compile(r"\bv\d+(?:\.\d+)+\b")
UPDATE_LABEL = "0123abc" # the scenario's stand-in for the newer build's label (a commit, 7 digits)
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}
WORK = tempfile.mkdtemp(prefix="kn-version-menu-", dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def changelog_versions(path):
    """The "== UPDATE x ==" titles of a docs/versions.txt, newest first (v10_news.c's rule)."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^\s*== UPDATE ([0-9][0-9.]*) ==", line)
            if m:
                out.append(m.group(1))
    return out


class Grab(HTMLParser):
    """The attributes and the text of the elements with the wanted ids (text of nested elements included)."""

    def __init__(self, want):
        super().__init__()
        self.want = want
        self.found = {}
        self.open = []  # (tag, entry or None)

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        entry = None
        if d.get("id") in self.want and d["id"] not in self.found:
            entry = self.found[d["id"]] = {"attrs": d, "text": []}
        if tag not in VOID:
            self.open.append((tag, entry))

    def handle_endtag(self, tag):
        for i in range(len(self.open) - 1, -1, -1):
            if self.open[i][0] == tag:
                del self.open[i:]
                return

    def handle_data(self, data):
        for _, entry in self.open:
            if entry:
                entry["text"].append(data)

    def text(self, ident):
        e = self.found.get(ident)
        return " ".join(" ".join(e["text"]).split()) if e else None


def run(args, env):
    return subprocess.run(args, env=env, cwd=ROOT, capture_output=True, text=True, timeout=300)


def clean_env(extra):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_ASSETS_DIR": ASSETS, "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state")})
    env.update(extra)
    return env


def build_commit(version_line):
    """The commit the --version line names: 12 hex digits (or "no commit" without one)."""
    m = re.search(r"KrunkNative client\) ([0-9a-f]{12}|no commit),", version_line)
    return m.group(1) if m else ""


def scenario_file():
    doc = {
        "size": [1280, 720],
        "background": "#222222",
        "state": {
            "menu": True,
            "connected": True,
            "ready": True,
            "inGame": False,
            "name": "Player",
            "classIndex": 0,
            "region": "...",
            "mapName": "Kanji",
            "mapFile": "maps/311_kanji.json",
            "gameName": "",
            "anim_time": 0.8,
            "account": {"name": "Player", "score": 250000, "funds": 1200},
            "update": {"state": "outdated", "latest": UPDATE_LABEL},
        },
        "frames": 3,
    }
    path = os.path.join(WORK, "menu.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return path


def main():
    if not os.path.exists(CLIENT):
        print(f"client_bin not found: {CLIENT}")
        return 1
    kn_versions = changelog_versions(os.path.join(WINDOWS, "kn-versions.txt"))
    krunker = changelog_versions(os.path.join(WINDOWS, "versions.txt"))
    if not kn_versions or not krunker:
        print("the changelogs have no UPDATE entries")
        return 1
    newest = kn_versions[0]

    # 1. --version: the commit, never a describe
    ver = run([CLIENT, "--version"], clean_env({}))
    line = (ver.stdout or "").strip().splitlines()[0] if ver.stdout.strip() else ""
    commit = build_commit(line)
    check("--version names the build by its commit", ver.returncode == 0 and (re.fullmatch(r"[0-9a-f]{12}", commit) is not None or commit == "no commit"),
          line)
    check("--version has no describe string and no v1.x version", DESCRIBE.search(line) is None and VERSION_TOKEN.search(line) is None, line)
    commit7 = commit[:7] if re.fullmatch(r"[0-9a-f]{12}", commit) else "unknown"

    # 2 and 3: the menu's DOM
    html_out = os.path.join(WORK, "menu.html")
    out_png = os.path.join(WORK, "menu.png")
    rendered = run([CLIENT, "--render-scenario", scenario_file(), out_png],
                   clean_env({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_SCENARIO_HTML": html_out, "KRUNKNATIVE_UI_DIR": os.path.join(ROOT, "ui")}))
    check("the menu renders with a signed-in account and an outdated build", rendered.returncode == 0 and os.path.exists(html_out),
          "\n".join(rendered.stderr.splitlines()[-5:]))
    if not os.path.exists(html_out):
        return 1
    with open(html_out, encoding="utf-8") as f:
        page = f.read()
    grab = Grab({"updateAd", "knUpdateAd", "knUpdateVersion", "knUpdateState"})
    grab.feed(page)

    ad = grab.text("updateAd")
    check("the menu's What's New tile shows the newest changelog update", ad is not None and f"v{newest}" in ad,
          f"expected v{newest}, tile says {ad!r}")
    check("the tile names one version (no other version or build string)", ad is not None and VERSION_TOKEN.findall(ad) == [f"v{newest}"], repr(ad))
    check("that version is Krunker's line (the major of Krunker's own changelog)",
          newest.split(".")[0] == krunker[0].split(".")[0], f"kn-versions {newest}, Krunker {krunker[0]}")

    box = grab.found.get("knUpdateAd")
    check("the Outdated box is shown", box is not None)
    title = box["attrs"].get("title", "") if box else ""
    check(f"the box names this client by its commit: \"This client: {commit7}\"", title == f"This client: {commit7}", repr(title))
    box_text = grab.text("knUpdateAd") or ""
    check("the box has no describe string or version", DESCRIBE.search(title + " " + box_text) is None and
          VERSION_TOKEN.search(title + " " + box_text) is None, repr(title + " | " + box_text))
    check("the box shows the newer build's label (a commit)", grab.text("knUpdateVersion") == UPDATE_LABEL, repr(grab.text("knUpdateVersion")))

    shutil.rmtree(WORK, ignore_errors=True)
    passed = sum(results)
    print(f"{passed}/{len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
