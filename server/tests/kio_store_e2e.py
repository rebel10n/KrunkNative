#!/usr/bin/env python3
"""The Store with krunker.io as the active account backend (client/include/kio_store.h), end to end.

  python3 -I server/tests/kio_store_e2e.py BUILD_DIR      (run from the repository root)

While an official krunker.io game is joined, the Store shows krunker.io's own catalogue, loaded the way
Krunker v10.0.2 loads it (its API client: shop/displays?tag=limited, ranked/shop, with the krunker.io
account's token), and nothing that would spend the account's KR / RP is sent to krunker.io.

A real kn_brain (scratch sqlite database: the account service is enabled, as for a player), the
krunker.io stubs of krunker_account_e2e.py (API with shop/displays and ranked/shop in v10's shapes,
FRVR auth, matchmaker, game server) and one headless client_bin (--offline):
  1. Box Office: the Eterno Shop tab is Box Office's (its empty state, no krunker.io request)
  2. an official game as a krunker.io guest: the Eterno Shop lists the stub's krunker.io products
     (fetched without a token); a guest's BUY opens v10's login modal
  3. signed in to krunker.io: the same catalogue, its SOLD OUT card, the Global limit, the dynamic
     (quantity) card and the section; BUY on a card, BUY in the item view, a spin, a voucher, Purchase
     KR and the ranked shop's Buy (the ranked shop lists the account's krunker.io items, fetched with
     its Bearer token) all show the read-only notice and send nothing to krunker.io
  4. leaving the game: the Box Office Eterno Shop again, exactly as in 1, and no further krunker.io
     shop request
Exit 0 when every check passed, 77 when it cannot run.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_kiostore_%d" % os.getpid()
SECRET = "kn-kiostore-%d" % os.getpid()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-store-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
# one word of each krunker.io product / mark the Eterno Shop shows (grep probes: one word each)
WORDS = ["Mug", "Selakuku", "Company", "Junk", "Season", "Paint", "Weekend", "Hovi", "$2.99", "2,000", "SOLD", "37/1000", "Global"]
SIGNED = "signedInHeaderBar signedOutHeaderBar knV10Login"
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


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def vis(text, label):
    m = re.search(r"\[input_script\] vis t=\d+ @%s (.*)" % re.escape(label), text)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


def grep(text, label):
    m = re.search(r"\[input_script\] grep t=\d+ %s needle=\S+ n=\d+ hits=(\d+)" % re.escape("@" + label), text)
    return int(m.group(1)) if m else -1


def words(text, label, ws):
    """the WORDS of ws the window @label shows (grep probes @label_<word>)"""
    return [w for w in ws if grep(text, "%s_%s" % (label, w)) > 0]


def log_mark(text, label):
    """the log from the probe @label on"""
    i = text.find("@%s " % label)
    return text[i:] if i >= 0 else ""


def eterno(t, label):
    """the Store's Eterno Shop tab: its text, the shop front and its cards"""
    return [f"{t} call showWindow 14",
            f"{t + 200} call windows.13.switchTab 5",
            f"{t + 1800} text @{label} [id=menuWindow]",
            f"{t + 1810} nodes @{label}front .shop-front",
            f"{t + 1820} nodes @{label}cards .shop-front-grid .shop-item-wrapper",
            f"{t + 1830} nodes @{label}empty .error-container",
            f"{t + 1835} text @{label}err .error-text"] + \
        [f"{t + 1840 + i} grep @{label}_{w} {w} [id=menuWindow]" for i, w in enumerate(WORDS)]


def script():
    s = ["0 menu"]
    s += eterno(500, "bo1")  # 1. Box Office (its brain display loads later: see check 4)
    s += ["2500 call closWind",
          '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
          "7000 vis @g " + SIGNED]
    s += eterno(7100, "g")  # 2. the krunker.io guest
    s += ["9100 clicksel .shop-front-grid .buy-button",
          "9600 vis @gbuy knV10Login",
          "9700 call knV10LoginClose",
          # 3. sign in to krunker.io from v10's login modal
          "10000 call loginOrRegister",
          "10500 clickid knV10LoginMode",
          "10700 clickid accName", "10750 type " + kae.LOGIN,
          "10900 clickid accPass", "10950 type " + kae.PASSWORD,
          "11100 clickid knV10LoginSubmit"]  # the Security Challenge: the challenge webview passes by itself
    t = 15000
    for i, d in enumerate(kae.CODE):
        s += ["%d clickid knV10Code%d" % (t, i), "%d type %s" % (t + 60, d)]
        t += 150
    s += ["17500 vis @kio " + SIGNED]
    s += eterno(18000, "k")
    s += ["20000 nodes @ksold .shop-front-grid .sideways-overlay",
          "20010 nodes @kdyn .dynamic-shop-item",
          "20020 nodes @ksection .shop-section .section-title",
          "20030 nodes @kglobal .global-limit-container",
          "20040 nodes @kbadge .badge-img",
          # BUY on a card (the last in the front grid)
          "20100 clicksel .shop-front-grid .buy-button",
          "20600 text @kro .error-popup-content",
          "20700 call knViewPop 0",
          # the item view (the Eterno Mug's card, display 5) and its BUY
          "21000 call knKioShopItem 5",
          "21500 nodes @kview .item-showcase-container",
          "21510 text @kviewtext .item-showcase-container",
          "21600 clicksel .purchase-section .buy-button",
          "22100 nodes @kro2 .error-popup-container",
          "22200 call knViewPop 0", "22300 call knViewPop 0",
          # a voucher, Purchase KR
          "22500 call voucherPopup",
          "22800 nodes @kvouch .error-popup-container",
          "22900 call knViewPop 0",
          "23000 call showPurchase 0",
          "23300 call knViewPop 0",
          # a spin (Prize Wheels: the first wheel)
          "23500 call windows.13.switchTab 1",
          "23800 call prizeWheel 0",
          "24500 call knSpinButton",
          "25000 nodes @kspin .error-popup-container",
          "25100 call knViewPop 0",
          "25200 call knSpinUI",
          "25500 call closWind",
          # the ranked shop
          "26000 call openRankedMenu",
          "26300 call knRkTab 4",
          "28500 text @rk [id=genericPop]",
          "28510 nodes @rkitems .shop-grid .shop-item-wrapper",
          "28520 grep @rk_rhino Rhino [id=genericPop]", "28530 grep @rk_clan Clan [id=genericPop]",
          "28600 call knRkBuy 501",
          "29000 nodes @rkro .error-popup-container",
          "29100 call knViewPop 0",
          "29300 call clearPops",
          # 4. leave: Box Office again
          "29500 con disconnect",
          "29700 down ESC", "29750 up ESC"]
    s += eterno(30500, "bo2")
    s += ["33000 quit"]
    return "\n".join(s) + "\n"


def run_client(brain_port, stub):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script())
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    log = os.path.join(WORK, "client.log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off",
                "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH})  # the challenge webview's page: its own stub
    # a private Xvfb for the challenge webview (WebKitGTK needs a display; the game itself stays headless)
    p = subprocess.Popen(["xvfb-run", "-a", "stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(240)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client finished its script", False, "timed out; log " + log)
    return read_log(log)


def shop_requests(stub, until=None):
    return [r for r in stub.requests[:until] if r[1].startswith("/api/shop/") or r[1] == "/api/ranked/shop"]


def checks(text, stub):
    bo1 = text_of(text, "bo1")
    # Box Office's own catalogue is the brain's "limited" display (seeded with krunker.io's public products, 0354_store_catalog);
    # the krunker.io stub's own product (Weekend Picks) never reaches it, before or after the game (check 4)
    check("1. Box Office: the Eterno Shop never shows the krunker.io stub's product (Weekend Picks)", not words(text, "bo1", ["Weekend"]),
          words(text, "bo1", ["Weekend"]))

    check("2. joining the official game makes krunker.io the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:3])
    disp = stub.find("GET", "/api/shop/displays")
    check("2. the Eterno Shop asks krunker.io for its \"limited\" display (v10 shop.getDisplayByTag)",
          bool(disp) and disp[0][2].get("tag") == "limited", [(r[1], r[2]) for r in disp][:3])
    check("2. ... as a guest: no Authorization header", bool(disp) and "authorization" not in disp[0][3], disp[0][3].get("authorization", "")[:12] if disp else "")
    want = ["Mug", "Selakuku", "Company", "Junk", "Season", "Paint", "Weekend", "Hovi"]
    check("2. the krunker.io guest's Eterno Shop lists the stub's krunker.io items", words(text, "g", want) == want, words(text, "g", WORDS))
    check("2. ... as v10's shop front cards (not Box Office's empty state)", nodes(text, "gfront") == 1 and nodes(text, "gcards") >= 4 and nodes(text, "gempty") == 0,
          (nodes(text, "gfront"), nodes(text, "gcards"), nodes(text, "gempty")))
    check("2. a guest's BUY: v10's login modal", vis(text, "gbuy").get("knV10Login") == "1", vis(text, "gbuy"))

    k = vis(text, "kio")
    check("3. signed in to krunker.io (v10's signed-in bar)", k.get("signedInHeaderBar") == "1", k)
    check("3. the signed-in Eterno Shop lists the krunker.io items with their prices",
          words(text, "k", WORDS) == WORDS, [w for w in WORDS if w not in words(text, "k", WORDS)])
    check("3. the sold-out product is marked SOLD OUT", nodes(text, "ksold") == 1 and grep(text, "k_SOLD") > 0, (nodes(text, "ksold"),))
    check("3. the global purchase limit, the dynamic card, the section and the badge card are drawn",
          nodes(text, "kglobal") == 1 and nodes(text, "kdyn") == 1 and nodes(text, "ksection") == 1 and nodes(text, "kbadge") == 1,
          (nodes(text, "kglobal"), nodes(text, "kdyn"), nodes(text, "ksection"), nodes(text, "kbadge")))
    check("3. BUY on a card: the read-only notice (Box Office)", "Box Office" in text_of(text, "kro"), text_of(text, "kro"))
    check("3. the card opens v10's item view", nodes(text, "kview") == 1 and "Eterno Mug" in text_of(text, "kviewtext"), text_of(text, "kviewtext")[:120])
    check("3. ... and its BUY gives the read-only notice", nodes(text, "kro2") == 1, nodes(text, "kro2"))
    check("3. Redeem Keys gives the read-only notice", nodes(text, "kvouch") == 1, nodes(text, "kvouch"))
    check("3. a spin gives the read-only notice", nodes(text, "kspin") == 1 and "[kio-store] read-only: spin not sent" in text, nodes(text, "kspin"))
    rk = stub.find("GET", "/api/ranked/shop")
    check("3. the ranked shop is krunker.io's ranked.getShop, with the account's Bearer token",
          bool(rk) and rk[-1][3].get("authorization", "").startswith("Bearer ") and rk[-1][3]["authorization"] != "Bearer ",
          [r[3].get("authorization", "")[:10] for r in rk])
    rkt = text_of(text, "rk")
    check("3. ... and lists the krunker.io ranked items", grep(text, "rk_rhino") > 0 and grep(text, "rk_clan") > 0 and nodes(text, "rkitems") == 2,
          (rkt[-200:], nodes(text, "rkitems")))
    check("3. its Buy gives the read-only notice", nodes(text, "rkro") == 1 and "read-only: ranked/purchase" in text, nodes(text, "rkro"))
    ro = re.findall(r"\[kio-store\] read-only: ([^\n]*)", text)
    check("3. every purchase / spin / redeem was refused on the client", len(ro) >= 7, ro)
    posts = [r for r in stub.requests if r[0] != "GET" and not r[1].startswith("/api/auth/") and not r[1].startswith("/frvr/")]
    check("3. nothing but the sign-in was POSTed to krunker.io (no shop/purchase, ranked/purchase, ...)", not posts, [(r[0], r[1]) for r in posts])
    check("3. kn_brain got no spin / voucher for the krunker.io account", not re.search(r'"(spin|fkr|vr)"', read_log(os.path.join(WORK, "brain.log"))), "")

    check("4. leaving switches back to Box Office", "[account] active account backend: krunk.io" in text, re.findall(r"\[account\] active.*", text)[:4])
    bo2 = text_of(text, "bo2")
    check("4. the Eterno Shop is Box Office's again: the brain's products, as before the game", nodes(text, "bo2front") == 1
          and nodes(text, "bo2empty") == 0 and words(text, "bo2", ["Hovi", "Mug"]) == ["Hovi", "Mug"] and not words(text, "bo2", ["Weekend"]),
          (bo1[-120:], bo2[-120:]))
    after = log_mark(text, "bo2")
    check("4. no krunker.io shop request after leaving", "[kio-store] shop/" not in after and "[kio-store] ranked/" not in after,
          re.findall(r"\[kio-store\].*", after)[:3])


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("kio_store_e2e: no client_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("kio_store_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain_log = os.path.join(WORK, "brain.log")
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local", "--brain.id=kiostore-%d" % os.getpid(),
                                  "--accounts.email.required=false", "--smtp.host="],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        stub.start()
        text = run_client(int(m.group(1)), stub)
        checks(text, stub)
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
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
