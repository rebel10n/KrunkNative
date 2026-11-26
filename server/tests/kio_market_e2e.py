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
  the item page      items.getSales: GET /items/1/sales?range=3 (v10:26963) for its Sales chart; a minted, wearable
                     item's Mints and Wear tabs, items.getInstances: GET /items/7791/instances?sort=mint|wear
                     (deob-10.0.7.js:27078-27083), the wear read failing ("Failed to load wear leaderboard"), and an
                     item whose view read fails ("Failed to load item details", no Mints tab)
  Black Market       getBlackMarketItems / getBlackMarketRank on the tab's mount: GET /market/black-market (owned) and
                     /market/black-market/rank (peak_mmr, the rank item) (deob-10.0.7.js:109167-109178);
                     market.purchaseBlackMarketItem: POST /market/black-market/purchase {item_index} (v10:26934), was
                     refused with "This krunker.io action is not in KrunkNative yet"
  lottery            lottery.getLottery: GET /lottery/3 (v10:26566) and participateInLottery: POST /lottery/3/participate
  Studios            studios.getMyStudios: GET /studios/mine (v10:27371), was never sent (not a krunker.io read)
  My Inventory       market.getInventory: GET /market/inventory (deob-10.0.7.js:26949), was the brain's empty page
  the popup's writes market.purchaseItem / listItem / quickSellItem / unlistItem (POST /market/purchase, list, quick-sell,
                     unlist) and trades.acceptTrade / cancelTrade / declineAll / createTrade (POST /trades/<id>/accept,
                     /trades/<id>/cancel, /trades/decline-all, /trades/create), with My Sales (GET /market/sales), Trading
                     (GET /trades/history) and Create Trade's reads (GET /trades/player-search, /trades/inventory/<id>)
  paint and style    items.setPaint / setStyle: POST /items/<instance>/paint {item_id, paint_item_id}, /style {item_id,
                     style} (deob-10.0.7.js:27093-27106, :108616-108665, :108817-108850); the paint taken off the account
  locks              items.getLocks / setLock: GET /items/locks on the inventory's mount, POST /items/locks {skin_id,
                     locked} from the inventory's lock toggle and {item_id | skin_id, locked} from the Wallet (:109026-109063)
  bulk and multiple  market.listBulk POST /market/list/bulk {items: [{item_id, skin_id, price, quantity}]} (:132187),
                     bulkQuickSell POST /market/quick-sell/bulk {action, items: [{skin_id, item_id}]} (:147136),
                     getBuyLadder GET /market/items/<i>/buy-ladder and buyMultiple POST /market/buy-multiple {item_id,
                     quantity, max_total} (:132963, :133029); a listed row (marketid) is not bulk-selectable (:135063)

The krunker.io stubs of krunker_account_e2e.py (GAPI_READS there, in v10's shapes) and one headless client_bin
(--offline, no brain). The client starts with a stored krunker.io session (refreshed by the stub), opens the Market
and checks the Browse grid, the Stats tab, the History tab, Listing Details' Listings / Owners / Stats, the item page's
Sales chart and a Black Market buy, then the lottery (and an entry) and the Studios window, then each of the popup's
writes: the listing's buy, My Inventory and a List, two Quick Sells, an Unlist, the trades' accept / decline / cancel (and a
refused accept), Decline All and a Create Trade, each checked for its body, the Bearer and its note; then a paint and a
style applied from My Inventory, a lock toggle, the select mode (the listed copy left out), a bulk List and a Buy
Multiple, and the Wallet's two locks and a bulk Quick Sell.
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
    """the most hits of any grep with this label (-1: none ran); a label may look more than once"""
    hits = re.findall(r"\[input_script\] grep t=\d+ %s needle=\S+ n=\d+ hits=(\d+)" % re.escape("@" + label), text)
    return max(int(h) for h in hits) if hits else -1


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
          # the item page of a minted, wearable item (K-35 B): its Mints and Wear tabs, the mint list, the wear read's failure
          "21310 call v10mk.citem %d" % kae.PAINT_ITEM,
          "21320 waitfor !.items-detail-loading",
          "21330 grep @cmints_tab Mints .simple-tabs",
          "21340 grep @cwear_tab Wear .simple-tabs",
          "21350 call v10mk.ctab 5",
          "21360 waitfor .items-detail-instance-table",
          "21370 grep @cmint_owner %s .items-detail-instance-table" % kae.INSTANCE_OWNERS[0],
          "21380 grep @cmint_serial 1,234 .items-detail-instance-table",
          "21390 grep @cmint_wear 0.0500 .items-detail-instance-table",
          "21400 nodes @cmint_rank .ii-col-rank",
          "21410 nodes @cmint_float .ii-col-float",
          "21420 call v10mk.ctab 6",
          "21430 waitfor .items-detail-error",
          "21440 grep @cwear_err leaderboard .items-detail-error",
          "21450 call v10mk.back",
          # an item page whose view read fails (KrunkCup Jerseys): v10's "Failed to load item details", no Mints tab
          "21460 call v10mk.citem %d" % kae.STYLE_ITEM,
          "21470 waitfor .items-detail-error",
          "21480 grep @cview_err details .items-detail-error",
          "21490 grep @cview_mints Mints .simple-tabs",
          "21500 call v10mk.back",
          # the Black Market buy (v10mk.side 2, bmbuy, the modal's OK)
          "21800 call v10mk.side 2",
          # the tab's mount: krunker.io's owned list and peak rank, then the grid
          "21810 waitfor .bm-grid",
          "21850 nodes @bm_owned .bm-card.owned",
          "21860 nodes @bm_rank [id=v10mkBm4531]",
          "21870 nodes @bm_loading .bm-loading",
          "21900 call v10mk.bmbuy %d" % BM_ITEM,
          "22000 call v10mk.mok",
          "22010 waitfor .bm-notification",
          "22020 grep @mkbm purchased .bm-notification",
          "22030 nodes @mkbm_err .bm-notification.error",
          "22040 nodes @bm_owned_after .bm-card.owned",
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
          "25600 call closWind",
          # the popup's writes. Each waits for its answer (the note), then for the note to go, so a check reads its own.
          # Buy the Browse listing (555, item 1, 1234 KR)
          "26000 call openExchange 0",
          "26010 waitfor .item-wrapper",
          "26100 call v10mk.buy 555 1 1234",
          "26200 call v10mk.mok",
          "26210 waitfor .notification-item",
          "26300 grep @wbuy purchased .notification-list",
          "26310 nodes @wbuy_err .notification-item.error",
          "26320 waitfor !.notification-item",
          # My Inventory (v10mk.mtab 1): krunker.io's rows, the emptied stack left out
          "26400 call v10mk.mtab 1",
          "26410 waitfor .quick-sell-icon",
          "26500 nodes @inv .item-wrapper",
          "26510 grep @inv_x3 x3 [id=v10mkGrid]",
          "26520 nodes @inv_none .empty-message",
          "26530 nodes @inv_list [id=v10mkList%d]" % kae.INV_STACK,
          # list the stack (its unit: the item index) for 500 KR
          "26600 call v10mk.list %d %d" % (kae.INV_STACK, kae.INV_STACK),
          '26610 call v10mk.price "500"',
          "26700 call v10mk.mok",
          "26710 waitfor .notification-item",
          "26800 grep @wlist listed .notification-list",
          "26810 nodes @wlist_err .notification-item.error",
          "26820 waitfor !.notification-item",
          # quick-sell the copy (its local id 1000000, the first copy of the answer) for KR: "1 Item"
          "26900 call v10mk.qs %d 1000000" % kae.INV_COPY,
          "26910 call v10mk.qsgo 0",
          "26920 waitfor .notification-item",
          "27000 grep @wqs Reticle .notification-list",
          "27010 nodes @wqs_err .notification-item.error",
          "27020 waitfor !.notification-item",
          # quick-sell all of the stack for Junk: "All" (amount 2), no skin_id
          "27100 call v10mk.qs %d %d" % (kae.INV_STACK, kae.INV_STACK),
          '27110 call v10mk.qsmode "2"',
          "27120 call v10mk.qsgo 1",
          "27130 waitfor .notification-item",
          "27200 grep @wqsj Junk .notification-list",
          "27210 waitfor !.notification-item",
          # My Sales (v10mk.mtab 2): unlist krunker.io's listing
          "27300 call v10mk.mtab 2",
          "27310 waitfor [id=v10mkUnlist%d]" % kae.SALE_MARKET_ID,
          "27400 call v10mk.unlist %d %d" % (kae.SALE_MARKET_ID, kae.SALE_ITEM),
          "27500 call v10mk.mok",
          "27510 waitfor .notification-item",
          "27600 grep @wunlist unlisted .notification-list",
          "27610 nodes @wunlist_err .notification-item.error",
          # Trading (v10mk.side 1): krunker.io's open trades
          "27700 call v10mk.side 1",
          "27710 waitfor [id=v10mkAccept41]",
          "27800 nodes @tin [id^=v10mkAccept]",
          "27810 nodes @tout [id^=v10mkCancel]",
          "27900 call v10mk.taccept 41",
          "27910 waitfor .trading-notification",
          "28000 grep @tacc accepted .trading-notification",
          "28010 waitfor !.trading-notification",
          "28100 call v10mk.tdecline 43",
          "28110 waitfor .trading-notification",
          "28200 grep @tdec declined .trading-notification",
          "28210 waitfor !.trading-notification",
          "28300 call v10mk.tcancel 42",
          "28310 waitfor .trading-notification",
          "28400 grep @tcan cancelled .trading-notification",
          "28410 waitfor !.trading-notification",
          # trade 44's accept is refused with a code: v10's text for it (errors.exchange.tradeItemNotOwned)
          "28500 call v10mk.taccept 44",
          "28510 waitfor .trading-notification",
          "28600 grep @taccerr inventory .trading-notification",
          "28610 nodes @taccerr_cls .trading-notification.error",
          "28620 waitfor !.trading-notification",
          "28700 call v10mk.tdeclineall",
          "28710 call v10mk.mok",
          "28720 waitfor .trading-notification",
          "28800 grep @tdall All .trading-notification",
          "28810 waitfor !.trading-notification",
          # Create Trade: the player search, the two inventories, the stack's unit and their copy, Submit, the confirm
          "28900 call v10mk.tcreate",
          '28910 call v10mk.tname "%s"' % kae.MARKET_FRIEND,
          "28920 waitfor .trade-sides",
          "29000 call v10mk.cadd 0 %d 0" % kae.INV_STACK,
          "29010 call v10mk.cadd 1 %d %d" % (kae.INV_COPY, kae.THEIR_COPY_KID),
          "29100 call v10mk.csubmit",
          "29110 call v10mk.mok",
          "29120 waitfor .trading-notification",
          "29200 grep @tsent sent .trading-notification",
          "29210 nodes @tsent_open .trade-sides",
          # My Inventory again: the lock toggles (the locked style copy's is on), the paint icon of the K-35 B copy
          "29300 call v10mk.side 0",
          "29310 call v10mk.mtab 1",
          "29320 waitfor [id=v10mkLock1000001]",
          "29400 nodes @locks .lock-toggle",
          "29410 nodes @locks_on .lock-toggle.is-locked",
          "29420 nodes @inv_all .item-wrapper",
          "29430 nodes @paintbtn [id=v10mkPaint1000001]",
          # paint the K-35 B copy (local id 1000001, instance 5552) with the first owned paint, Red Paint (x2)
          "29500 call v10mk.paint %d 1000001" % kae.PAINT_ITEM,
          "29510 waitfor .paint-container",
          "29600 grep @paint_x2 2 .swatch-count",  # v10 draws the bare count (deob-10.0.7.js:108718)
          "29610 call paintApply",
          "29620 waitfor !.paint-container",
          # as Styled below: the toast comes with the POST's reply, so look every 200 ms for a second
          "29700 grep @wpaint Painted [id=knNotify]",
          "29900 grep @wpaint Painted [id=knNotify]",
          "30100 grep @wpaint Painted [id=knNotify]",
          "30300 grep @wpaint Painted [id=knNotify]",
          "30500 grep @wpaint Painted [id=knNotify]",
          # the paint was spent: the picker lists one Red Paint now
          "30800 call v10mk.paint %d 1000001" % kae.PAINT_ITEM,
          "30810 waitfor .paint-container",
          "30900 grep @paint_x1 1 .swatch-count",
          "30910 call knViewPop 0",
          "30920 waitfor !.paint-container",
          # style the KrunkCup Jerseys copy (1000002, instance 5553): style 3
          "31000 call v10mk.style %d 1000002" % kae.STYLE_ITEM,
          "31010 waitfor .style-container",
          "31100 call stylePick %d" % kae.STYLE_PICK,
          "31110 call styleApply",
          "31120 waitfor !.style-container",
          # the toast comes with the POST's reply, which a loaded box can take more than 90 ms for: look
          # again every 200 ms for a second (it stays up 2 s, ui_notify.c NOTIFY_DURATION_MS)
          "31200 grep @wstyle Styled [id=knNotify]",
          "31400 grep @wstyle Styled [id=knNotify]",
          "31600 grep @wstyle Styled [id=knNotify]",
          "31800 grep @wstyle Styled [id=knNotify]",
          "32000 grep @wstyle Styled [id=knNotify]",
          # lock the K-35 B copy from its toggle
          "32300 call v10mk.lock %d 1000001" % kae.PAINT_ITEM,
          "32310 waitfor .notification-item",
          "32400 grep @wlock Locked .notification-list",
          "32410 waitfor !.notification-item",
          # select mode: the listed copy (1000003) is not picked, Select all leaves it and the locked copy out
          "32500 call v10mk.bsel",
          "32510 waitfor .bulk-check-btn",
          "32600 call v10mk.bpick %d 1000003" % kae.LISTED_ITEM,
          "32610 nodes @bpick_listed .item-wrapper.bulk-selected",
          "32620 nodes @bdim .item-wrapper.bulk-dim",
          "32630 call v10mk.ball",
          "32700 grep @ball 5 .bulk-count",
          "32710 call v10mk.bclr",
          # List selected: the stack (3 units) at 5,000 KR each
          "32720 call v10mk.bpick %d %d" % (kae.INV_STACK, kae.INV_STACK),
          "32730 call v10mk.blist",
          "32740 waitfor [id=knV10BulkConfirm]",
          "32800 call knV10BulkPrice 0 5000",
          "32810 call knV10BulkConfirm",
          "32820 waitfor .notification-item",
          "32900 grep @wblist Listed .notification-list",
          "32910 waitfor !.notification-item",
          "32920 call v10mk.bsel",
          # Buy multiple from Listing Details of item 1: its ladder (1,234 and 1,300 KR), two of them
          "33000 call v10mk.item 1",
          "33010 waitfor .buy-multiple-container",
          "33100 call v10mk.bbuy",
          "33110 waitfor .bm-chip",
          "33200 call knV10BulkBuyQty 2",
          "33210 call knV10BulkConfirm",
          "33220 waitfor .notification-item",
          "33300 grep @wbuym Bought .notification-list",
          "33310 waitfor !.notification-item",
          "33400 call closWind",
          # the Wallet: lock the stack (group 0) and an Acid Breath copy (group 6), then a bulk Quick Sell for KR of the
          # Acid Breath copy it picks (group 6, copy 0: instance 5555)
          "33500 call knV10Wallet",
          "33510 waitfor .wallet-lock",
          "33600 call knV10WalletLock 0 -1",
          "33700 call knV10WalletLock 6 0",
          "33800 call knV10WalletSelect",
          "33810 waitfor .wallet-pick",
          "33900 call knV10WalletPick 6 0",
          "34000 call knV10WalletBulkSell 0",
          "34010 waitfor [id=knV10BulkConfirm]",
          "34100 call knV10BulkConfirm",
          "34110 waitfor .notification-item",
          "34200 grep @wbqs Sold [id=knV10WalletNotes]",
          "34500 quit"]
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
    # (counted up to the lottery: the writes' Market, opened again later, reloads the item's listings, as a buy does)
    lottery = next((k for k, r in enumerate(stub.requests) if r[0] == "GET" and r[1] == "/api/lottery/3"), len(stub.requests))
    view = [r for r in stub.requests[:lottery] if r[0] == "GET" and r[1] == "/api/items/1/view"]
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
    # the item page's Mints / Wear tabs (deob-10.0.7.js:109725-109768)
    inst = stub.find("GET", "/api/items/%d/instances" % kae.PAINT_ITEM)
    check("a minted item's page (stackable false, wearable) has the Mints and Wear tabs",
          grep(text, "cmints_tab") > 0 and grep(text, "cwear_tab") > 0, (grep(text, "cmints_tab"), grep(text, "cwear_tab")))
    check("... each tab asks items.getInstances once with the Bearer: /items/7791/instances?sort=mint, then ?sort=wear",
          bearer(inst) and [r[2] for r in inst] == [{"sort": "mint"}, {"sort": "wear"}], [r[2] for r in inst])
    check("... Mints lists the instances: rank, owner, Float (0.0500), Mint (#1,234; a dash without a serial)",
          grep(text, "cmint_owner") > 0 and grep(text, "cmint_serial") > 0 and grep(text, "cmint_wear") > 0 and
          nodes(text, "cmint_rank") == 3 and nodes(text, "cmint_float") == 3,
          (grep(text, "cmint_owner"), grep(text, "cmint_serial"), grep(text, "cmint_wear"), nodes(text, "cmint_rank"), nodes(text, "cmint_float")))
    check("... a failed wear read shows 'Failed to load wear leaderboard'", grep(text, "cwear_err") > 0, grep(text, "cwear_err"))
    check("a failed view read shows 'Failed to load item details' and no Mints tab",
          grep(text, "cview_err") > 0 and grep(text, "cview_mints") == 0, (grep(text, "cview_err"), grep(text, "cview_mints")))
    # the Black Market tab: v10's two reads on mount (deob-10.0.7.js:137741-137751)
    bmi, bmr = stub.find("GET", "/api/market/black-market"), stub.find("GET", "/api/market/black-market/rank")
    check("the Black Market tab asks krunker.io's /market/black-market and /market/black-market/rank with the Bearer",
          bearer(bmi) and bearer(bmr), ([r[1] for r in bmi], [r[1] for r in bmr]))
    check("... shows the item krunker.io lists as bought (the 1 Year Cape) as owned, and only it",
          nodes(text, "bm_owned") == 1 and nodes(text, "bm_loading") == 0, (nodes(text, "bm_owned"), nodes(text, "bm_loading")))
    check("... and offers Emperor's Blade (rank 4700) to a peak rank of 4800", nodes(text, "bm_rank") == 1, nodes(text, "bm_rank"))
    check("... and a bought item joins the owned ones (ownedItems + item.index)", nodes(text, "bm_owned_after") == 2,
          nodes(text, "bm_owned_after"))
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
    # the popup's writes: v10.0.7's method, path and body, the account's Bearer, and the answer applied
    def one(path):
        rows = stub.find("POST", path)
        return rows[0][4] if bearer(rows) and len(rows) == 1 else ("not one with the Bearer", [(r[4], r[3].get("authorization", "")[:7]) for r in rows])

    check("the listing's buy POSTs /market/purchase {market_id} (deob-10.0.7.js:134157)", one("/api/market/purchase") == {"market_id": 555},
          one("/api/market/purchase"))
    check("... and says 'Item purchased successfully!'", grep(text, "wbuy") > 0 and nodes(text, "wbuy_err") == 0,
          (grep(text, "wbuy"), nodes(text, "wbuy_err")))
    inv = stub.find("GET", "/api/market/inventory")
    check("My Inventory asks krunker.io's /market/inventory with the Bearer (deob-10.0.7.js:26949)", bearer(inv), [r[1] for r in inv])
    check("... and shows its rows: the stack (x3, List), the copies (2, the paint, style and listed ones, two Acid Breath), "
          "not the emptied stack (cnt 0)",
          nodes(text, "inv") == 7 and grep(text, "inv_x3") > 0 and nodes(text, "inv_none") == 0 and nodes(text, "inv_list") == 1,
          (nodes(text, "inv"), grep(text, "inv_x3"), nodes(text, "inv_none"), nodes(text, "inv_list")))
    check("List POSTs /market/list {item_id, price, skin_id: the stack's sid} (deob-10.0.7.js:109104-109108)",
          one("/api/market/list") == {"item_id": kae.INV_STACK, "price": 500, "skin_id": kae.INV_STACK_KID}, one("/api/market/list"))
    check("... and says 'Item listed successfully'", grep(text, "wlist") > 0 and nodes(text, "wlist_err") == 0,
          (grep(text, "wlist"), nodes(text, "wlist_err")))
    qs = stub.find("POST", "/api/market/quick-sell")
    check("Quick Sell POSTs /market/quick-sell with the Bearer: the copy's {item_id, action 0, amount 0, skin_id: its instance}, "
          "then the stack's All for Junk {action 1, amount 2} with no skin_id (deob-10.0.7.js:132698-132706)",
          bearer(qs) and [r[4] for r in qs] == [{"item_id": kae.INV_COPY, "action": 0, "amount": 0, "skin_id": kae.INV_COPY_KID},
                                                {"item_id": kae.INV_STACK, "action": 1, "amount": 2}], [r[4] for r in qs])
    check("... and says 'Sold Reticle Blaze for KR', then 'Sold Moon Dragon for Junk'",
          grep(text, "wqs") > 0 and nodes(text, "wqs_err") == 0 and grep(text, "wqsj") > 0, (grep(text, "wqs"), nodes(text, "wqs_err"), grep(text, "wqsj")))
    sales = stub.find("GET", "/api/market/sales")
    check("My Sales asks krunker.io's /market/sales with the Bearer", bearer(sales), [r[1] for r in sales])
    check("Unlist POSTs /market/unlist {market_id} (deob-10.0.7.js:134319)", one("/api/market/unlist") == {"market_id": kae.SALE_MARKET_ID},
          one("/api/market/unlist"))
    check("... and says 'Item unlisted successfully'", grep(text, "wunlist") > 0 and nodes(text, "wunlist_err") == 0,
          (grep(text, "wunlist"), nodes(text, "wunlist_err")))
    th = stub.find("GET", "/api/trades/history")
    check("Trading asks krunker.io's /trades/history?offset=0&limit=30 and lists its offers (3 incoming, 1 outgoing)",
          bearer(th) and th[0][2] == {"offset": "0", "limit": "30"} and nodes(text, "tin") == 3 and nodes(text, "tout") == 1,
          ([r[2] for r in th][:1], nodes(text, "tin"), nodes(text, "tout")))
    for path, label, note in (("/api/trades/41/accept", "tacc", "Trade accepted!"), ("/api/trades/43/cancel", "tdec", "Trade declined"),
                              ("/api/trades/42/cancel", "tcan", "Trade cancelled")):
        rows = stub.find("POST", path)
        check("%s POSTs %s with the Bearer and no body (deob-10.0.7.js:27165-27180)" % (note.split()[1].capitalize(), path[4:]),
              bearer(rows) and len(rows) == 1 and rows[0][4] is None, [(r[4], r[3].get("authorization", "")[:7]) for r in rows])
        check("... and says '%s'" % note, grep(text, label) > 0, grep(text, label))
    rows = stub.find("POST", "/api/trades/44/accept")
    check("a refused accept (code TRADE_ITEM_NOT_OWNED) shows v10's text for the code as an error",
          bearer(rows) and grep(text, "taccerr") > 0 and nodes(text, "taccerr_cls") == 1, (len(rows), grep(text, "taccerr"), nodes(text, "taccerr_cls")))
    rows = stub.find("POST", "/api/trades/decline-all")
    check("Decline All POSTs /trades/decline-all with the Bearer (deob-10.0.7.js:137269) and says 'All trades declined'",
          bearer(rows) and len(rows) == 1 and rows[0][4] is None and grep(text, "tdall") > 0, (len(rows), grep(text, "tdall")))
    ps, ti = stub.find("GET", "/api/trades/player-search"), stub.find("GET", "/api/trades/inventory/%d" % kae.MARKET_FRIEND_ID)
    check("Create Trade asks /trades/player-search?name= and /trades/inventory/<id> with the Bearer",
          bearer(ps) and ps[0][2] == {"name": kae.MARKET_FRIEND} and bearer(ti), ([r[2] for r in ps], len(ti)))
    check("... and POSTs /trades/create {target_player_id, my_offer: [{i, c}], their_offer: [{i, c, iids}], message} (deob-10.0.7.js:111517)",
          one("/api/trades/create") == {"target_player_id": kae.MARKET_FRIEND_ID, "my_offer": [{"i": kae.INV_STACK, "c": 1}],
                                        "their_offer": [{"i": kae.INV_COPY, "c": 1, "iids": [kae.THEIR_COPY_KID]}], "message": ""},
          one("/api/trades/create"))
    check("... then the window closes and 'Trade offer sent!' shows", grep(text, "tsent") > 0 and nodes(text, "tsent_open") == 0,
          (grep(text, "tsent"), nodes(text, "tsent_open")))
    check("no write was refused as not sent to krunker.io", "is not sent for the krunker.io account" not in text,
          re.findall(r"\[gapi\].*not sent.*", text)[:3])
    # the paint, style, lock, bulk and Buy Multiple writes (v10.0.7's items / market APIs)
    locks = stub.find("GET", "/api/items/locks")
    check("My Inventory asks krunker.io's /items/locks with the Bearer on its mount (deob-10.0.7.js:135001)", bearer(locks),
          [r[1] for r in locks])
    check("... and each owned row with a sid has a lock toggle, the locked copy's on (is-locked)",
          nodes(text, "locks") == 7 and nodes(text, "locks_on") == 1 and nodes(text, "inv_all") == 7,
          (nodes(text, "locks"), nodes(text, "locks_on"), nodes(text, "inv_all")))
    check("the paintable copy has the paint icon on krunker.io (was: no picker for the account)", nodes(text, "paintbtn") == 1,
          nodes(text, "paintbtn"))
    check("Paint POSTs /items/<instance>/paint {item_id, paint_item_id} with the Bearer (deob-10.0.7.js:108623-108626)",
          one("/api/items/%d/paint" % kae.PAINT_COPY_KID) == {"item_id": kae.PAINT_ITEM, "paint_item_id": kae.PAINT},
          one("/api/items/%d/paint" % kae.PAINT_COPY_KID))
    check("... toasts 'Painted K-35 B with Red Paint' and takes the paint off the account (2, then 1: :108632-108648)",
          grep(text, "wpaint") > 0 and grep(text, "paint_x2") > 0 and grep(text, "paint_x1") > 0,
          (grep(text, "wpaint"), grep(text, "paint_x2"), grep(text, "paint_x1")))
    check("Style POSTs /items/<instance>/style {item_id, style} with the Bearer (deob-10.0.7.js:108823-108826) and toasts 'Styled'",
          one("/api/items/%d/style" % kae.STYLE_COPY_KID) == {"item_id": kae.STYLE_ITEM, "style": kae.STYLE_PICK} and grep(text, "wstyle") > 0,
          (one("/api/items/%d/style" % kae.STYLE_COPY_KID), grep(text, "wstyle")))
    lk = [r[4] for r in stub.find("POST", "/api/items/locks")]
    check("the lock toggle POSTs /items/locks {skin_id: the row's sid, locked} (deob-10.0.7.js:134993-134995), the Wallet's "
          "{item_id} for a stack and {skin_id} for a copy (:147536-147539)",
          bearer(stub.find("POST", "/api/items/locks")) and lk[:1] == [{"skin_id": kae.PAINT_COPY_KID, "locked": True}]
          # the Wallet's two toggles are 100 ms apart, each its own request: either may land first
          and sorted(lk[1:], key=json.dumps) == sorted([{"item_id": kae.INV_STACK, "locked": True},
                                                        {"skin_id": kae.BQS_KIDS[0], "locked": True}], key=json.dumps), lk)
    check("... and says 'Locked K-35 B', reading the locks again", grep(text, "wlock") > 0 and len(locks) >= 3, (grep(text, "wlock"), len(locks)))
    check("select mode: the listed copy (marketid) is dimmed and not picked, Select all picks the 5 others that can be "
          "(the locked copy left out too: deob-10.0.7.js:135063-135064)",
          nodes(text, "bpick_listed") == 0 and nodes(text, "bdim") == 2 and grep(text, "ball") > 0,
          (nodes(text, "bpick_listed"), nodes(text, "bdim"), grep(text, "ball")))
    check("List selected POSTs /market/list/bulk {items: [{item_id, skin_id, price, quantity}]} (deob-10.0.7.js:132187-132195) "
          "and says 'Listed 3 items'",
          one("/api/market/list/bulk") == {"items": [{"item_id": kae.INV_STACK, "skin_id": kae.INV_STACK_KID, "price": 5000, "quantity": 3}]}
          and grep(text, "wblist") > 0, (one("/api/market/list/bulk"), grep(text, "wblist")))
    lad = stub.find("GET", "/api/market/items/1/buy-ladder")
    check("Buy multiple asks /market/items/1/buy-ladder (deob-10.0.7.js:133029) and POSTs /market/buy-multiple {item_id, "
          "quantity, max_total} (:132963-132967), then says 'Bought 2 for 2,534 KR'",
          bearer(lad) and one("/api/market/buy-multiple") == {"item_id": 1, "quantity": 2, "max_total": 2534} and grep(text, "wbuym") > 0,
          (len(lad), one("/api/market/buy-multiple"), grep(text, "wbuym")))
    bqs = one("/api/market/quick-sell/bulk")
    check("the Wallet's bulk Quick Sell POSTs /market/quick-sell/bulk {action 0, items: [{skin_id, item_id}]} for one Acid "
          "Breath copy (deob-10.0.7.js:147136-147144) and says 'Sold 1 for ... KR'",
          bqs == {"action": 0, "items": [{"skin_id": kae.BQS_KIDS[0], "item_id": kae.BQS_ITEM}]}
          and grep(text, "wbqs") > 0, (bqs, grep(text, "wbqs")))
    posts = [(r[0], r[1]) for r in stub.requests if r[0] != "GET" and not r[1].startswith("/api/auth/") and not r[1].startswith("/frvr/")]
    check("nothing but the sign-in and these writes was POSTed to krunker.io, in this order",
          posts == [("POST", p) for p in ("/api/market/black-market/purchase", "/api/lottery/3/participate", "/api/market/purchase",
                                          "/api/market/list", "/api/market/quick-sell", "/api/market/quick-sell", "/api/market/unlist",
                                          "/api/trades/41/accept", "/api/trades/43/cancel", "/api/trades/42/cancel",
                                          "/api/trades/44/accept", "/api/trades/decline-all", "/api/trades/create",
                                          "/api/items/%d/paint" % kae.PAINT_COPY_KID, "/api/items/%d/style" % kae.STYLE_COPY_KID,
                                          "/api/items/locks", "/api/market/list/bulk", "/api/market/buy-multiple",
                                          "/api/items/locks", "/api/items/locks", "/api/market/quick-sell/bulk")], posts)


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_market_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    # 5000 KR: the lottery's KR cost can be entered (each sign-in's "a" carries it); level 12: the account can trade
    # its skins: two Red Paints and the instances the paint and style pickers act on (kae.KIO_SKINS)
    stub.account_data = [{"player_clan": kae.TURF_CLAN, "player_funds": 5000, "player_score": 160000, "skins": kae.KIO_SKINS}] * 2
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
