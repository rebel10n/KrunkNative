#!/usr/bin/env python3
"""Localisation end-to-end (client/src/ui/i18n.c, ui/i18n/): the Settings > Language select switches the
whole page live, headless, through English, Russian (Cyrillic), Japanese, Simplified Chinese, Korean (CJK)
and Hindi (Devanagari), then back to English and to Japanese: the main menu, the open Settings window and
the in-game HUD show v10's strings of that language (read from ui/i18n/<code>.json), every rendered
character has a glyph (no tofu: the engine's glyph probe), and the choice persists (krk_lang) into a second
run. None of the 17 languages Krunker v10.0.2 ships is right-to-left, so there is no RTL case.
Also: tools/i18n-import/i18n_lint.py (every I18N("key", "English") of the C against the tables).

  python3 -I server/tests/i18n_e2e.py BUILD_DIR   (from the repository root)
  KN_WEB_DIST=<web dist dir> also runs the web build in headless Chromium (else that part is skipped)

Each run is one headless offline client_bin (KRUNKNATIVE_HEADLESS=1, KRUNKNATIVE_AUDIO_BACKEND=null)
driven by an input script. No server, no ports. Exit code 0 when every check passed; logs are kept in
~/.cache/kn-i18n-e2e-* on failure.
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
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-i18n-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
UI_DIR = os.environ.get("KN_V10_UI_DIR", "ui")
failures = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def table(code):
    with open(os.path.join(UI_DIR, "i18n", code + ".json"), encoding="utf-8") as fh:
        return json.load(fh)


EN = table("en")


def t(code, key):
    """v10 get(): the language's value, else en's"""
    return (table(code).get(key) or EN.get(key) or "UNLOCALIZED STRING") if code != "en" else EN[key]


def fold(s):
    return re.sub(r"\s+", " ", s or "").strip()


def run(name, script, timeout=240):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_LANG")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": CFG,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_UI_DIR": UI_DIR, "KRUNKNATIVE_INPUT_SCRIPT": path,
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(False, f"{name}: client_bin finished within {timeout} s")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def probe(log, label):
    m = re.search(r"\[input_script\] text t=\d+ " + re.escape(label) + r" n=(\d+) checked=(\d) value=(.*?) text=(.*)", log)
    return {"n": int(m.group(1)), "value": m.group(3), "text": m.group(4)} if m else {"n": -1, "value": None, "text": None}


def glyphs(log, label):
    m = re.search(r"\[input_script\] glyphs t=\d+ " + re.escape(label) + r" n=(\d+) missing=(-?\d+) first=(\S+)", log)
    return (int(m.group(1)), int(m.group(2)), m.group(3)) if m else (-1, -1, "?")


def locale(log, label):
    m = re.search(r"\[input_script\] locale t=\d+ " + re.escape(label) + r" code=(\S+) gen=(\d+) keys=(-?\d+)", log)
    return (m.group(1), int(m.group(2)), int(m.group(3))) if m else (None, -1, -1)


def storage(key):
    try:
        with open(os.path.join(CFG, "krunknative", key + ".json"), encoding="utf-8") as f:
            return f.read()
    except OSError:
        return None


# What is checked in each language: (label, selector, key) on the main menu ...
MENU = [
    ("host", "[id=menuBtnHost] .cardLabel", "menu.btn.host"),
    ("ranked", "[id=menuBtnRanked] .cardLabel", "menu.btn.ranked"),
    ("custom", "[id=menuBtnCustomGames] .cardLabel", "windows.maps.header"),
    ("customize", "[id=customizeButton]", "menu.btn.customize"),
    ("invite", "[id=inviteButton]", "menu.btn.invite"),
    ("join", "[id=menuBtnJoin]", "menu.btn.join"),
    ("play", "[id=instructions]", "app.play"),
]
# ... and in the open Settings window (General tab of the basic view)
SETTINGS = [
    ("sethdr", "[id=windowHeader]", "windows.settings.header"),
    ("langrow", "[id=knLangSelect]", None),  # the Language select: its options are never translated
]
LANGS = ["en", "ru", "jp", "zh", "kr", "hi", "en", "jp"]


def probes(tag, t0):
    lines = [f"{t0} locale @{tag}"]
    for label, sel, _ in MENU:
        lines.append(f"{t0} text @{tag}-{label} {sel}")
    lines.append(f"{t0} text @{tag}-sethdr [id=windowHeader]")
    lines.append(f"{t0} text @{tag}-langlabel [id=setBod_local] .settName + .settName")
    lines.append(f"{t0} text @{tag}-langsel [id=knLangSelect]")
    lines.append(f"{t0} glyphs @{tag}-page body")
    return lines


# ---------------------------------------------------------------- lint (no client)
lint = subprocess.run([sys.executable, "-I", "tools/i18n-import/i18n_lint.py"], capture_output=True, text=True)
check(lint.returncode == 0, "i18n_lint: every I18N(key, English) of the C matches the tables" +
      ("" if lint.returncode == 0 else ": " + lint.stdout.strip()[-400:]))

# ---------------------------------------------------------------- run 1: switch languages live
script = ["0 menu", "500 call showWindow 1"]
t0 = 1500
script += probes("boot", t0)
for i, code in enumerate(LANGS[1:], 1):
    t0 += 400
    script.append(f'{t0} call setSetting "lang" "{code}"')
    t0 += 800
    script += probes(f"s{i}", t0)
# the HUD: in game, in the last language (jp); the round ends (con end): the end screen's "Next Round in"
# (timer.end) in jp, then live in ru, then jp again
t0 += 400
script += [f"{t0} play", f"{t0 + 2500} glyphs @hud-page [id=inGameUI]", f"{t0 + 2600} con end",
           f"{t0 + 6000} text @hud-end-jp [id=endTimer]", f"{t0 + 6000} glyphs @hud-end-jp-g [id=endUI]",
           f'{t0 + 6100} call setSetting "lang" "ru"', f"{t0 + 7600} text @hud-end-ru [id=endTimer]",
           f"{t0 + 7600} glyphs @hud-end-ru-g [id=endUI]",
           f'{t0 + 7700} call setSetting "lang" "jp"', f"{t0 + 9200} text @hud-end-jp2 [id=endTimer]", f"{t0 + 9300} quit"]
log1 = run("run1", "\n".join(script))

boot = locale(log1, "@boot")
check(boot[0] == "en", f"first start: v10's default language en (no krk_lang; v10 reads no system locale) ({boot[0]})")
with open(os.path.join(UI_DIR, "i18n", "kn", "en.json"), encoding="utf-8") as fh:
    KN = [k for k in json.load(fh) if k.startswith("kn.")]
check(boot[2] == len(EN) + len(KN), f"en table: {boot[2]} keys (ui/i18n/en.json {len(EN)} + kn/en.json {len(KN)})")
for i, code in enumerate(["en"] + LANGS[1:]):
    tag = "boot" if i == 0 else f"s{i}"
    got = locale(log1, f"@{tag}")
    check(got[0] == code, f"{tag}: the Language setting switched to {code} ({got[0]})")
    for label, _, key in MENU:
        p = probe(log1, f"@{tag}-{label}")
        want = fold(t(code, key))
        shown = fold(p["text"])
        if label == "customize":
            shown = re.sub(r" ?brush$", "", shown)  # the button's material icon ligature
        ok = shown == want or (label == "play" and shown.upper() == want.upper())
        check(ok, f"{tag} [{code}] menu {label}: {key} = {want!r} (shown {p['text']!r})")
    hdr = probe(log1, f"@{tag}-sethdr")
    check(fold(hdr["text"]) == fold(t(code, "windows.settings.header")),
          f"{tag} [{code}] Settings header = {t(code, 'windows.settings.header')!r} ({hdr['text']!r})")
    lab = probe(log1, f"@{tag}-langlabel")
    check((lab["text"] or "").startswith(fold(t(code, "settings.local.lang"))),
          f"{tag} [{code}] the Language row's label = {t(code, 'settings.local.lang')!r} ({(lab['text'] or '')[:40]!r})")
    sel = probe(log1, f"@{tag}-langsel")
    check(sel["value"] == code and "English" in (sel["text"] or "") and "日本語" in (sel["text"] or ""),
          f"{tag} [{code}] the Language select shows {code}, its options in their own names ({sel['value']!r})")
    n, missing, first = glyphs(log1, f"@{tag}-page")
    check(n == 1 and missing == 0, f"{tag} [{code}] every rendered character of the page has a glyph (missing {missing}, first {first})")

n, missing, first = glyphs(log1, "@hud-page")
check(n == 1 and missing == 0, f"in game [jp]: every rendered HUD character has a glyph (missing {missing}, first {first})")


def template_re(value):
    """a table value with {n} placeholders as a regex of the text it formats to"""
    return "^" + ".+".join(re.escape(part) for part in re.split(r"\{\d\}", fold(value))) + "$"


for label, code in (("@hud-end-jp", "jp"), ("@hud-end-ru", "ru"), ("@hud-end-jp2", "jp")):
    shown = fold(probe(log1, label)["text"])
    check(re.match(template_re(t(code, "timer.end")), shown) is not None,
          f"end screen [{code}]: timer.end = {t(code, 'timer.end')!r} ({shown!r})")
for label in ("@hud-end-jp-g", "@hud-end-ru-g"):
    n, missing, first = glyphs(log1, label)
    check(n == 1 and missing == 0, f"end screen {label}: no missing glyphs (missing {missing}, first {first})")
check(storage("krk_lang") is not None and "jp" in (storage("krk_lang") or ""), f"krk_lang stored ({storage('krk_lang')!r})")

# ---------------------------------------------------------------- run 2: the choice persists
log2 = run("run2", "0 menu\n500 call showWindow 1\n1500 " + "\n1500 ".join(l.split(" ", 1)[1] for l in probes("again", 1500)) + "\n1600 quit")
again = locale(log2, "@again")
check(again[0] == "jp", f"second start: the menu comes up in jp (krk_lang) ({again[0]})")
for label, _, key in MENU:
    p = probe(log2, f"@again-{label}")
    want = fold(t("jp", key))
    shown = re.sub(r" ?brush$", "", fold(p["text"])) if label == "customize" else fold(p["text"])
    check(shown == want or (label == "play" and shown.upper() == want.upper()),
          f"second start [jp] menu {label} = {want!r} ({p['text']!r})")
n, missing, first = glyphs(log2, "@again-page")
check(n == 1 and missing == 0, f"second start [jp]: no missing glyphs (missing {missing}, first {first})")

# ---------------------------------------------------------------- web build (KN_WEB_DIST): the same C over the browser DOM
def web_part():
    """headless Chromium (DevTools protocol) on a web build's dist dir: English first, the Language select
    to ru (its table fetched over HTTP, then the page re-rendered), to jp, and a reload that comes up in jp."""
    import asyncio
    import shutil as _shutil
    import time
    import urllib.request
    try:
        import websockets
    except ImportError:
        print("SKIP  web: needs python websockets")
        return
    chrome = _shutil.which("chromium-browser") or _shutil.which("chromium") or _shutil.which("google-chrome")
    dist = os.environ.get("KN_WEB_DIST", "")
    if not chrome or not os.path.isfile(os.path.join(dist, "client.wasm")):
        print("SKIP  web: needs chromium and KN_WEB_DIST (a web build's dist dir)")
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from kn_proto import free_ports
    http_port, cdp_port = free_ports(2)
    own = [subprocess.Popen([sys.executable, "-m", "http.server", "--bind", "127.0.0.1", str(http_port)], cwd=dist,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True),
           subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                             "--enable-unsafe-swiftshader", f"--remote-debugging-port={cdp_port}",
                             f"--user-data-dir={os.path.join(WORK, 'chrome')}", "--window-size=1280,720", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)]
    origin = f"http://127.0.0.1:{http_port}"
    texts_js = ("JSON.stringify({lang: document.documentElement.lang, stored: localStorage.getItem('krk_lang'), "
                "host: (document.querySelector('#menuBtnHost .cardLabel') || {}).textContent || null, "
                "join: (document.querySelector('#menuBtnJoin') || {}).textContent || null, "
                "hdr: (document.getElementById('windowHeader') || {}).textContent || null})")
    result = {}

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

            async def snapshot():
                return json.loads(await evaluate(texts_js) or "{}")

            async def pick(code):
                await evaluate("(s => { s.value = %s; s.dispatchEvent(new Event('change')); return 1; })"
                               "(document.getElementById('knLangSelect'))" % json.dumps(code))
                want = json.dumps(fold(t(code, "menu.btn.host")))
                await until("((document.querySelector('#menuBtnHost .cardLabel') || {}).textContent || '').trim() === %s" % want, 30)
                return await snapshot()

            await call("Page.enable")
            await call("Page.navigate", url=origin + "/favicon.png")  # the origin, for localStorage
            await asyncio.sleep(0.5)
            await evaluate("localStorage.clear(), 1")
            await call("Page.navigate", url=origin + "/?offline=1&map=kanji")
            ready = await until("!!(window.windows && document.querySelector('#menuBtnHost .cardLabel'))", 120)
            result["ready"] = ready
            if ready:
                await asyncio.sleep(2.0)
                result["boot"] = await snapshot()
                await evaluate("showWindow(1), 1")
                await until("!!document.getElementById('knLangSelect')", 20)
                result["ru"] = await pick("ru")
                result["jp"] = await pick("jp")
                await call("Page.navigate", url=origin + "/?offline=1&map=kanji")
                await until("!!(window.windows && document.querySelector('#menuBtnHost .cardLabel'))", 120)
                want = json.dumps(fold(t("jp", "menu.btn.host")))
                await until("((document.querySelector('#menuBtnHost .cardLabel') || {}).textContent || '').trim() === %s" % want, 30)
                result["reload"] = await snapshot()
            task.cancel()

    try:
        asyncio.run(asyncio.wait_for(session(), 400))
    except Exception as e:  # noqa: BLE001 - reported as a failed check
        result["error"] = repr(e)
    finally:
        for p in own:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    check(result.get("ready") is True, f"web: the page and the client came up ({result.get('error', '')})")
    if not result.get("ready"):
        return
    boot = result.get("boot", {})
    check(fold(boot.get("host")) == fold(EN["menu.btn.host"]) and boot.get("lang") == "en",
          f"web: first start in English, <html lang=en> ({boot})")
    for code, alias in (("ru", "ru"), ("jp", "ja")):
        snap = result.get(code, {})
        check(fold(snap.get("host")) == fold(t(code, "menu.btn.host")) and fold(snap.get("join")) == fold(t(code, "menu.btn.join"))
              and snap.get("lang") == alias and (snap.get("stored") or "") == code,
              f"web: Language -> {code}: its table fetched, the menu re-rendered, <html lang={alias}>, krk_lang stored ({snap})")
        check(fold(snap.get("hdr")) == fold(t(code, "windows.settings.header")),
              f"web [{code}]: the open Settings window's header translated in place ({snap.get('hdr')!r})")
    rl = result.get("reload", {})
    check(fold(rl.get("host")) == fold(t("jp", "menu.btn.host")) and rl.get("lang") == "ja",
          f"web: a reload comes up in jp (krk_lang, the table fetched at start) ({rl})")


web_part()

if failures:
    print(f"\n{len(failures)} check(s) failed; logs in {WORK}")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("\nall i18n checks passed")
