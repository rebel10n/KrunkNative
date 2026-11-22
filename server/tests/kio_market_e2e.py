#!/usr/bin/env python3
"""The Market with krunker.io as the active account backend (client/src/ui/v10_market.c), end to end.

  python3 -I server/tests/kio_market_e2e.py BUILD_DIR      (run from the repository root)

While an official krunker.io game is joined and the krunker.io account is signed in, the Market's Browse grid lists
krunker.io's listings (v10's market.getListings: gapi GET /market/listings) and its Stats tab shows krunker.io's stats
(v10's market.getStats: GET /market/stats). Before this, both were economy-socket frames that the held socket never
answered, so the Browse grid said "No listings available" and the Stats tab stayed empty.

The account's other screens that read krunker.io the same way (each was a brain request the brain gate refused):
  History            market.getHistory: GET /market/history?sort=date_desc&limit=50 (deob-10.0.7.js:135786)
  Listing Details    items.getView: GET /items/1/view (v10:26957) for its listings and owners, and
                     market.getItemSalesStatistics: GET /market/items/1/sales-statistics?daysback=30 (v10:26895) for Stats
  the item page      items.getSales: GET /items/1/sales?range=3 (v10:26963) for its Sales chart
  Black Market       market.purchaseBlackMarketItem: POST /market/black-market/purchase {item_index} (v10:26934), was
                     refused with "This krunker.io action is not in KrunkNative yet"
  lottery            lottery.getLottery: GET /lottery/3 (v10:26566) and participateInLottery: POST /lottery/3/participate
  Studios            studios.getMyStudios: GET /studios/mine (v10:27371), was never sent (not a krunker.io read)

The krunker.io stubs of krunker_account_e2e.py (GAPI_READS there, in v10's shapes) and one headless client_bin
(--offline, no brain). The client starts with a stored krunker.io session (refreshed by the stub), opens the Market
and checks the Browse grid, the Stats tab, the History tab, Listing Details' Listings / Owners / Stats, the item page's
Sales chart and a Black Market buy, then the lottery (and an entry) and the Studios window.
Exit 0 when every check passed, 77 when it cannot run.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-market-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
NO_LISTINGS = "No listings available"
BM_ITEM = 1642  # Nuke Tamer, a Black Market item of the store table (services/brain/tests/v10_api_market_test.py BM_BUY)
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


def grep(text, label):
    m = re.search(r"\[input_script\] grep t=\d+ %s needle=\S+ n=\d+ hits=(\d+)" % re.escape("@" + label), text)
    return int(m.group(1)) if m else -1


def nodes(text, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def script():
    # the krunker.io account is the stored one (seed_session): signed in at start, its socket carries the refreshed token
    s = ["0 menu", "2500 call closWind",
         '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "14500 call closWind",
          # Browse: the listing's card (waitfor: the clock holds until the grid has a card, or times out)
          "15000 call openExchange 0",
          "15010 waitfor .item-wrapper",
          "16000 nodes @mkcards .item-wrapper",
          "16010 grep @mkprice 1,234 [id=genericPop]",
          "16020 grep @mknone %s [id=genericPop]" % NO_LISTINGS,
          # Stats: the Stats tab (v10mk.mtab 4) shows the stats row's card
          "17000 call v10mk.mtab 4",
          "17010 waitfor .stat-card",
          "18000 nodes @mkstat .stat-card",
          "18010 grep @mkstat_circ Circulation [id=genericPop]",
          # History (v10mk.mtab 3): krunker.io's history row, its other party as Who
          "18100 call v10mk.mtab 3",
          # (grep's needle is one word, then the selector; '#' starts a script comment, so ids are [id=...])
          "18110 waitfor tr.clickable",
          "18600 grep @mkhist %s [id=genericPop]" % kae.MARKET_HISTORY_PARTY,
          "18610 grep @mkhist_amt 777 tr.clickable",
          "18620 nodes @mkhist_none .empty-history",
          # Listing Details of item 1: the view's listing, its owners (tab 2) and the Stats chart (tab 1)
          "18700 call v10mk.mtab 0",
          "18800 call v10mk.item 1",
          "18810 waitfor .listing-item",
          "19300 nodes @mklisting .listing-item",
          "19310 grep @mklisting_seller %s .listing-item" % kae.MARKET_SELLER,
          "19400 call v10mk.dtab 2",
          "19410 waitfor .owners-list",
          "19900 grep @mkowner %s .owners-list" % kae.MARKET_OWNER,
          "20000 call v10mk.dtab 1",
          "20010 waitfor .stats-headers",
          "20500 grep @mkdstats Total .stats-headers",
          "20510 nodes @mkdstats_none .empty-stats",
          "20520 nodes @mkdstats_load .stats-loading",
          # the item page (v10mk.citem) and its Sales tab (2): a range read
          "20600 call v10mk.citem 1",
          "20700 call v10mk.ctab 2",
          "20710 waitfor !.items-detail-loading",
          "21200 nodes @mkcsales_none .items-detail-empty",
          "21210 nodes @mkcsales_chart .items-detail-graph-canvas",
          "21300 call v10mk.back",
          # the Black Market buy (v10mk.side 2, bmbuy, the modal's OK)
          "21800 call v10mk.side 2",
          "21900 call v10mk.bmbuy %d" % BM_ITEM,
          "22000 call v10mk.mok",
          "22010 waitfor .bm-notification",
          "22020 grep @mkbm purchased .bm-notification",
          "22030 nodes @mkbm_err .bm-notification.error",
          "22500 call closWind",
          # the lottery: the view, one entry of 100 KR (the stepper's +), then the view again
          "23000 call openLottery 3",
          "23010 waitfor .lottery-title",
          "23500 grep @lottitle Kio .lottery-title",
          "23510 nodes @lotfail .lottery-window-loading",
          "23600 call knMiscLotQty 0 1",
          "23650 nodes @lotsub .submitting-entries",
          "23700 call knMiscLotEnter",
          "23710 waitfor !.submitting-entries",
          "24500 nodes @lotsub_after .submitting-entries",
          "24510 nodes @lottitle_after .lottery-title",
          "24600 call knMiscClose",
          # the Studios window: krunker.io's studios
          "25000 call openStudios",
          "25010 waitfor [id=knStRow31]",
          "25500 nodes @studio [id=knStRow31]",
          "25510 grep @studioname %s [id=knStRow31]" % kae.KIO_STUDIO,
          "26000 quit"]
    return "\n".join(s) + "\n"


def run_client(stub):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script())
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    # a stored krunker.io session (krunker_account.c's kio_refresh_token / kio_username, html_storage files): the client
    # starts signed in and refreshes it against the stub's FRVR auth, so no sign-in form (whose Turnstile challenge needs
    # the WebKitGTK webview, absent from some builds) is needed; krunker_account_e2e.py covers the form itself
    for key, value in (("kio_refresh_token", kae.REFRESH1), ("kio_username", kae.ACCOUNT_NAME)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, "client.log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:9", "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off",
                "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH})
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


def checks(text, stub):
    check("the official game is the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:2])
    lst = stub.find("GET", "/api/market/listings")
    check("the Browse grid asks krunker.io's /market/listings with the account's Bearer", bool(lst) and lst[-1][3].get("authorization", "").startswith("Bearer "),
          [r[3].get("authorization", "")[:10] for r in lst])
    check("the Browse grid lists krunker.io's listing (one card)", nodes(text, "mkcards") == 1, nodes(text, "mkcards"))
    check("... at its price (1,234 KR)", grep(text, "mkprice") > 0, "")
    check("... and no 'No listings available'", grep(text, "mknone") == 0, "")
    st = stub.find("GET", "/api/market/stats")
    check("the Stats tab asks krunker.io's /market/stats", bool(st), [r[1] for r in stub.requests][-6:])
    check("the Stats tab shows krunker.io's stats row (one card, circulation)", nodes(text, "mkstat") == 1 and grep(text, "mkstat_circ") > 0,
          (nodes(text, "mkstat"), grep(text, "mkstat_circ")))
    def bearer(rows):
        return bool(rows) and all(r[3].get("authorization", "").startswith("Bearer ") for r in rows)

    # History
    hist = stub.find("GET", "/api/market/history")
    check("the History tab asks krunker.io's /market/history?sort=date_desc&limit=50 with the Bearer",
          bearer(hist) and hist[-1][2] == {"sort": "date_desc", "limit": "50"}, [(r[2], r[3].get("authorization", "")[:7]) for r in hist])
    check("... and shows its row: the other party as Who, 777 KR, no 'No transaction history found'",
          grep(text, "mkhist") > 0 and grep(text, "mkhist_amt") > 0 and nodes(text, "mkhist_none") == 0,
          (grep(text, "mkhist"), grep(text, "mkhist_amt"), nodes(text, "mkhist_none")))
    # Listing Details: /items/1/view (listings, owners) and the Stats tab's sales-statistics
    view = stub.find("GET", "/api/items/1/view")
    check("Listing Details asks krunker.io's /items/1/view (once for its listings and owners)", bearer(view) and len(view) == 1,
          [r[1] for r in view])
    check("... its Listings tab lists the view's listing (one, the seller)", nodes(text, "mklisting") == 1 and grep(text, "mklisting_seller") > 0,
          (nodes(text, "mklisting"), grep(text, "mklisting_seller")))
    check("... its Owners tab lists the view's owner", grep(text, "mkowner") > 0, grep(text, "mkowner"))
    stats = stub.find("GET", "/api/market/items/1/sales-statistics")
    check("the Stats tab asks /market/items/1/sales-statistics?daysback=30", bearer(stats) and stats[0][2] == {"daysback": "30"},
          [r[2] for r in stats])
    check("... and draws the chart (Total Sold; not empty, not loading)",
          grep(text, "mkdstats") > 0 and nodes(text, "mkdstats_none") == 0 and nodes(text, "mkdstats_load") == 0,
          (grep(text, "mkdstats"), nodes(text, "mkdstats_none"), nodes(text, "mkdstats_load")))
    sales = stub.find("GET", "/api/items/1/sales")
    check("the item page's Sales tab asks /items/1/sales?range=3 (its range index)", bearer(sales) and sales[0][2] == {"range": "3"},
          [r[2] for r in sales])
    check("... and draws it (no 'No sales in this window')", nodes(text, "mkcsales_none") == 0 and nodes(text, "mkcsales_chart") == 1,
          (nodes(text, "mkcsales_none"), nodes(text, "mkcsales_chart")))
    # the Black Market buy
    bm = stub.find("POST", "/api/market/black-market/purchase")
    check("the Black Market buy POSTs krunker.io's market/black-market/purchase {item_index} with the Bearer",
          bearer(bm) and len(bm) == 1 and bm[0][4] == {"item_index": BM_ITEM}, [(r[4], r[3].get("authorization", "")[:7]) for r in bm])
    check("... and says 'Item purchased successfully!' (no error note)", grep(text, "mkbm") > 0 and nodes(text, "mkbm_err") == 0,
          (grep(text, "mkbm"), nodes(text, "mkbm_err")))
    check("... not refused as an action that is not in KrunkNative yet", "not sent to krunker.io yet" not in text,
          re.findall(r"\[kio-store\].*not sent.*", text)[:2])
    # the lottery
    lot = stub.find("GET", "/api/lottery/3")
    part = stub.find("POST", "/api/lottery/3/participate")
    check("the lottery asks krunker.io's /lottery/3 with the Bearer", bearer(lot), [r[3].get("authorization", "")[:7] for r in lot])
    check("... and shows krunker.io's lottery (its title, not the load failure)", grep(text, "lottitle") > 0 and nodes(text, "lotfail") == 0,
          (grep(text, "lottitle"), nodes(text, "lotfail")))
    check("an entry of 100 KR (the stepper) POSTs /lottery/3/participate {cost_type, cost_ref, cost_quantity} with the Bearer",
          nodes(text, "lotsub") == 1 and bearer(part) and len(part) == 1 and
          part[0][4] == {"cost_type": "currency", "cost_ref": "1", "cost_quantity": 100}, (nodes(text, "lotsub"), [r[4] for r in part]))
    check("... then the entries reset and the lottery is asked again", nodes(text, "lotsub_after") == 0 and nodes(text, "lottitle_after") == 1
          and len(lot) >= 2, (nodes(text, "lotsub_after"), nodes(text, "lottitle_after"), len(lot)))
    # Studios
    mine = stub.find("GET", "/api/studios/mine")
    check("the Studios window asks krunker.io's /studios/mine with the Bearer", bearer(mine), [r[1] for r in mine])
    check("... and lists krunker.io's studio", nodes(text, "studio") == 1 and grep(text, "studioname") > 0,
          (nodes(text, "studio"), grep(text, "studioname")))
    posts = [(r[0], r[1]) for r in stub.requests if r[0] != "GET" and not r[1].startswith("/api/auth/") and not r[1].startswith("/frvr/")]
    check("nothing but the sign-in, the Black Market buy and the lottery entry was POSTed to krunker.io",
          posts == [("POST", "/api/market/black-market/purchase"), ("POST", "/api/lottery/3/participate")], posts)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_market_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    # 5000 KR: the lottery's KR cost can be entered (each sign-in's "a" carries it)
    stub.account_data = [{"player_clan": kae.TURF_CLAN, "player_funds": 5000}] * 2
    try:
        stub.start()
        text = run_client(stub)
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
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
