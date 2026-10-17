#!/usr/bin/env python3
"""The Server Type reset is once per profile (client/src/ui/seek_prefs.c seek_prefs_migrate_servers,
knSeekServersMigrated): a saved "official" (or "both") from the old first-run popup becomes Box Office
("kn") on the first launch, and a later krunker.io pick is never touched again.

  python3 -I server/tests/seek_servers_migrate_e2e.py BUILD_DIR      (run from the repository root)

One headless client_bin run at a time (--offline: no network), each on a profile whose knSettings.json is
written before the launch (XDG_CONFIG_HOME/krunknative), driven by KRUNKNATIVE_INPUT_SCRIPT:

  M1 a profile with knSeekServers "official" and no flag: the first launch stores "kn" and the flag 1, and
     logs the reset once; the next launch keeps "kn" and logs nothing;
  M2 the same for "both";
  M3 a profile where the player picks "official" after the flag is set (setSetting): the next launch keeps it;
  M4 a profile with no Server Type saved: the launch stores the flag only (knSeekServers stays unset), and
     the next launch with "kn" saved keeps "kn".

The web build (KN_WEB_DIST, default dist/web: `make web`) runs the same reset on its localStorage knSettings,
in headless Chromium over CDP (the i18n_e2e.py pattern): W1 "official" with no flag becomes "kn" with the flag,
W2 the next load keeps "kn", W3 a pick of "official" made after the flag survives the next load. SKIPped with a
SKIP line when chromium, the dist's client.wasm or the python websockets package is missing.

Prints PASS/FAIL lines; exit 0 only when everything passed.
"""
import json
import os
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-servers-migrate-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
MIGRATED_LOG = "Quickplay knSeekServers="  # the line seek_prefs_migrate_servers prints when it resets a choice


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def assets_dir():
    """the game assets: the checkout's own (worktrees lack the untracked asset dirs)"""
    try:
        common = subprocess.check_output(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                         cwd=os.path.dirname(os.path.abspath(__file__)), text=True).strip()
        main = os.path.dirname(common)
        if os.path.isdir(os.path.join(main, "assets")):
            return os.path.join(main, "assets")
    except (OSError, subprocess.CalledProcessError):
        pass
    return os.path.abspath("assets")


def new_profile(name, saved):
    """a profile directory; `saved` is the knSettings.json object to start from (None: no file)"""
    prof = os.path.join(WORK, name)
    os.makedirs(os.path.join(prof, "krunknative"), exist_ok=True)
    if saved is not None:
        with open(os.path.join(prof, "krunknative", "knSettings.json"), "w") as f:
            json.dump(saved, f)
    return prof


def run_client(name, profile, script):
    """One headless client_bin run on `profile` (XDG_CONFIG_HOME, kept across runs); returns its log text."""
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL",
                                                             "KRUNKNATIVE_FIRST_RUN", "KRUNKNATIVE_MOBILE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": profile,
                "XDG_STATE_HOME": profile + "-state", "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    env.setdefault("KRUNKNATIVE_ASSETS_DIR", assets_dir())
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--offline"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=90)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
    text = read(log)
    if "[input_script] " not in text:
        print(text[-2000:])
    return text


def settings(profile):
    """the stored settings object (knSettings.json), {} when there is none"""
    try:
        with open(os.path.join(profile, "krunknative", "knSettings.json")) as f:
            v = json.load(f)
    except (OSError, ValueError):
        return {}
    return v if isinstance(v, dict) else {}


def quit_script(*lines):
    return "0 menu\n" + "".join(line + "\n" for line in lines) + "1500 quit\n"


def web_check():
    """The web build: the same reset on its localStorage knSettings, the page served over HTTP and driven in
    headless Chromium through CDP (as i18n_e2e.py does)."""
    import asyncio
    import shutil as _shutil
    import urllib.request
    try:
        import websockets
    except ImportError:
        print("SKIP  web: needs python websockets", flush=True)
        return
    chrome = _shutil.which("chromium-browser") or _shutil.which("chromium") or _shutil.which("google-chrome")
    dist = os.environ.get("KN_WEB_DIST") or os.path.abspath("dist/web")
    if not chrome or not os.path.isfile(os.path.join(dist, "client.wasm")):
        print("SKIP  web: needs chromium and a web build (KN_WEB_DIST, make web)", flush=True)
        return
    # the build's assets link points into the checkout's own assets/ (untracked content: worktrees lack it), so the
    # page is served from a staging dir that links the build's entries and the assets_dir() tree (read only)
    stage = os.path.join(WORK, "web-stage")
    os.makedirs(stage)
    for entry in os.listdir(dist):
        if entry != "assets":
            os.symlink(os.path.join(dist, entry), os.path.join(stage, entry))
    os.symlink(assets_dir(), os.path.join(stage, "assets"))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from kn_proto import free_ports
    http_port, cdp_port = free_ports(2)
    own = [subprocess.Popen([sys.executable, "-m", "http.server", "--bind", "127.0.0.1", str(http_port)], cwd=stage,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True),
           subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                             "--enable-unsafe-swiftshader", f"--remote-debugging-port={cdp_port}",
                             f"--user-data-dir={os.path.join(WORK, 'chrome')}", "--window-size=1280,720", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)]
    origin = f"http://127.0.0.1:{http_port}"
    seen = {}
    error = []

    async def session():
        target = None
        for _ in range(200):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{cdp_port}/json/list", timeout=2) as r:
                    target = next((t for t in json.load(r) if t.get("type") == "page"), None)
                if target:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.1)
        if not target:
            error.append("no page target")
            return
        async with websockets.connect(target["webSocketDebuggerUrl"], max_size=1 << 28) as ws:
            ids, replies = iter(range(1, 1 << 30)), {}

            async def reader():
                async for raw in ws:
                    msg = json.loads(raw)
                    if "id" in msg:
                        replies[msg["id"]] = msg

            task = asyncio.create_task(reader())

            async def call(method, **params):
                i = next(ids)
                await ws.send(json.dumps({"id": i, "method": method, "params": params}))
                for _ in range(1200):
                    if i in replies:
                        return replies.pop(i).get("result", {})
                    await asyncio.sleep(0.05)
                return {}

            async def evaluate(expr):
                r = await call("Runtime.evaluate", returnByValue=True, expression=expr)
                return r.get("result", {}).get("value")

            async def until(expr, seconds):
                for _ in range(int(seconds * 4)):
                    if await evaluate(expr):
                        return True
                    await asyncio.sleep(0.25)
                return False

            async def open_page():
                """the page loaded fresh: the menu's Host card is up (the settings are read at startup)"""
                await call("Page.navigate", url=origin + "/?offline=1&map=kanji")
                up = await until("!!(window.windows && document.querySelector('#menuBtnHost .cardLabel'))", 120)
                await asyncio.sleep(2.0)
                return up

            async def stored():
                return json.loads(await evaluate("localStorage.getItem('knSettings') || '{}'") or "{}")

            await call("Page.enable")
            await call("Page.navigate", url=origin + "/favicon.png")  # the origin, for localStorage
            await asyncio.sleep(0.5)
            await evaluate("localStorage.clear(), 1")
            await evaluate("localStorage.setItem('knSettings', %s), 1" % json.dumps(json.dumps({"knSeekServers": "official"})))
            seen["ready"] = await open_page()
            seen["first"] = await stored()
            seen["next_up"] = await open_page()
            seen["next"] = await stored()
            await evaluate("window.setSetting('knSeekServers', 'official'), 1")
            await asyncio.sleep(0.5)
            await open_page()
            seen["pick"] = await stored()
            task.cancel()

    try:
        asyncio.run(asyncio.wait_for(session(), 400))
    except Exception as e:  # noqa: BLE001 - reported as a failed check
        error.append(repr(e))
    finally:
        for p in own:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    check("W0 web: the page and the client came up", seen.get("ready") is True, error or seen)
    if not seen.get("ready"):
        return
    check("W1 web first load: knSeekServers official -> kn, the flag 1",
          seen["first"].get("knSeekServers") == "kn" and seen["first"].get("knSeekServersMigrated") == 1, seen.get("first"))
    check("W2 web next load: still kn", seen.get("next_up") and seen["next"].get("knSeekServers") == "kn", seen.get("next"))
    check("W3 web: a later pick of official is kept on the next load",
          seen.get("pick", {}).get("knSeekServers") == "official" and seen["pick"].get("knSeekServersMigrated") == 1, seen.get("pick"))


def main():
    if not os.path.isdir(BUILD):
        print(f"no build dir {BUILD}")
        return 2

    # M1: "official" from the old popup, no flag: reset once, then kept
    p = new_profile("m1", {"knSeekServers": "official"})
    text = run_client("m1-first", p, quit_script())
    s = settings(p)
    check("M1 first launch: knSeekServers official -> kn", s.get("knSeekServers") == "kn", s.get("knSeekServers"))
    check("M1 first launch: knSeekServersMigrated is 1", s.get("knSeekServersMigrated") == 1, s.get("knSeekServersMigrated"))
    check("M1 first launch: the reset is logged", MIGRATED_LOG in text and "official -> kn" in text)
    text = run_client("m1-next", p, quit_script())
    s = settings(p)
    check("M1 next launch: still kn", s.get("knSeekServers") == "kn", s.get("knSeekServers"))
    check("M1 next launch: no second reset", MIGRATED_LOG not in text)

    # M2: "both" is reset the same way
    p = new_profile("m2", {"knSeekServers": "both"})
    run_client("m2-first", p, quit_script())
    s = settings(p)
    check("M2 both -> kn with the flag set", s.get("knSeekServers") == "kn" and s.get("knSeekServersMigrated") == 1, s)

    # M3: the player picks krunker.io after the flag: the next launch keeps it
    p = new_profile("m3", {"knSeekServers": "official"})
    run_client("m3-reset", p, quit_script())
    text = run_client("m3-pick", p, quit_script('300 call setSetting "knSeekServers" "official"'))
    check("M3 the pick is stored: official", settings(p).get("knSeekServers") == "official", settings(p).get("knSeekServers"))
    text = run_client("m3-next", p, quit_script())
    s = settings(p)
    check("M3 next launch: official is kept", s.get("knSeekServers") == "official", s.get("knSeekServers"))
    check("M3 next launch: no reset", MIGRATED_LOG not in text)

    # M4: nothing saved: the flag only (knSeekServers stays unset); a saved "kn" is kept
    p = new_profile("m4", None)
    run_client("m4-first", p, quit_script())
    s = settings(p)
    check("M4 fresh profile: the flag is set", s.get("knSeekServersMigrated") == 1, s.get("knSeekServersMigrated"))
    check("M4 fresh profile: knSeekServers is not written", "knSeekServers" not in s, s.get("knSeekServers"))
    run_client("m4-pick", p, quit_script('300 call setSetting "knSeekServers" "kn"'))
    run_client("m4-next", p, quit_script())
    check("M4 kn picked and kept on the next launch", settings(p).get("knSeekServers") == "kn", settings(p).get("knSeekServers"))

    web_check()

    passed = sum(results)
    print(f"seek_servers_migrate_e2e: {passed}/{len(results)} passed (work dir {WORK})")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
