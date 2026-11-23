#!/usr/bin/env python3
"""The Clans window and the Turf Wars Recruitment board with krunker.io as the active account backend
(client/src/ui/clan_ui.c, client/src/ui/turf_ui.c), end to end.

  python3 -I server/tests/kio_clan_e2e.py BUILD_DIR      (run from the repository root)

While an official krunker.io game is joined and the krunker.io account is signed in, the clan screens are v10.0.7's own
against krunker.io's gapi (the clans client, deob-10.0.7.js:26838-26924), with the account's Bearer, never Box Office's
brain:
  reads   GET /clans/<id>/motd, /clans/<id>/loot-config, /clans/<id>/recruitment (the id is clanData.clan_id, from the
          game socket's "cln" answer, as v10's), GET /clans/recruitment and GET /clans/my-applications (the account has
          no clan)
  writes  PUT /clans/<id>/motd {message}, DELETE /clans/<id>/motd, PUT /clans/<id>/loot-config {the six fields},
          POST /clans/<id>/banner-color {paint_item_id}, DELETE /clans/<id>/logo, POST /clans/<id>/recruitment {post},
          DELETE /clans/<id>/recruitment, POST /clans/<id>/apply (no body), and a staff Take Down: DELETE
          /clans/<other>/recruitment
  upload  POST /clans/<id>/logo as v10's FormData: multipart/form-data, one part `file` with the picked PNG's name, type
          and bytes (deob-10.0.7.js:96094-96120)
and the window's game-socket writes go over the official game socket as v10.0.7 encodes them (deob-10.0.7.js
220405-220642, 223013): ["cln", 1 | 2, tag] create / join, ["cln", 4, 1] leave, ["cln", 5, id] kick, ["cln", 6, id, {r}]
role, ["cln", 7, kr, {wd}] funds, ["cln", 9, link] Discord, ["clnR", id, ok], ["clnRA"], and the ownership transfer's
rpcs ["_0", rpcId, "clan-transfer-initiate" | "-cancel" | "-respond", ...]; each answer ("cln", "clnL", "_0") lands in
the window as v10 applies it. The Social hub's clan view sends Request to Join as applyToClan too.

The krunker.io stubs of krunker_account_e2e.py (its HTTP stub, extended here by a handler subclass with the clan routes:
nothing in that file changes) and its game server (the "cln" answer added through frame_handlers). Two headless
client_bin runs (--offline, no brain):
  A. signed in with the clan KPD (id 9001, the account its owner, one Red Paint): the home tab's loot display and the
     settings tab's MOTD, loot config and recruitment post come from krunker.io; then the MOTD is saved and removed,
     the loot config saved, the Red Paint applied, the logo removed, the recruitment post saved and removed; the
     ownership transfer refused, started and cancelled, a respond; a logo picked (KRUNKNATIVE_PICK_FILE: a 512 x 512
     PNG) and bought with the button it shows; the Discord link set (its popup stays open), a member's role changed, KR deposited, the member kicked, a request accepted and the
     rest declined; the clan left, and a new one created
  B. signed in with no clan as a moderator: the Recruitment board lists krunker.io's posts, my-applications marks one
     Applied, Apply on the other sends applyToClan and marks it Applied, a Take Down removes a post; the Clans window's
     join request; the hub's clan view's Request to Join
Exit 0 when every check passed, 77 when it cannot run.
"""
import copy
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.parse
import zlib

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its stubs; its own WORK dir is removed below)

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-kio-clan-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []

CLAN_ID, CLAN = 9001, "KPD"
RED_PAINT = 7776  # k_paints[0]: the first owned paint is the default pick
NEW_MOTD = "Raid at nine"
ANSWER_BANNER = "#123456"  # the banner-color answer's clan_banner_color (v10 shows the answer's, not the paint's own)
RC_APPLY, RC_APPLIED, RC_TAKEDOWN = 4242, 4343, 4444  # Recruitment board posts: Apply, already applied, taken down
MEMBER_ID, REQ_ID = 555, 666  # a Soldier of KPD; a join request's player
KIO_CLAN = {"clan_name": CLAN, "clan_id": CLAN_ID, "clan_score": 5000, "clan_funds": 50000, "clan_playerid": kae.ACCOUNT_ID,
            "members": [{"p": kae.ACCOUNT_NAME, "i": kae.ACCOUNT_ID, "r": 3}, {"p": "Soldier1", "i": MEMBER_ID, "r": 1}],
            "requests": [{"player_name": "Wannabe", "cr_playerid": REQ_ID}, {"player_name": "Other", "cr_playerid": 667}]}
NEW_CLAN, NEW_CLAN_ID = "NWC", 9002  # the clan created after leaving KPD
HUB_CLAN, HUB_CLAN_ID = "HUB", 4545  # the hub's clan view (run B)
DISCORD = "https://discord.gg/kpdraid"
HEIR = "Heir"  # the transfer's target ("Nobody" is refused)
KR_AFTER, CLAN_FUNDS_AFTER = 4321, 50900  # the deposit's answer: the account's KR and the clan bank
LOGO_NAME = "kpd_logo.png"


def png(w, h):  # a grey w x h PNG (8-bit greyscale)
    def chunk(kind, data):
        return len(data).to_bytes(4, "big") + kind + data + (zlib.crc32(kind + data) & 0xffffffff).to_bytes(4, "big")
    raw = b"".join(b"\x00" + b"\x80" * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes([8, 0, 0, 0, 0])) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


LOGO = png(512, 512)
STATE = {"clan": None}  # the stub game server's clan for the account (create / leave change it)
WS = []  # the clan frames the client sent, decoded ([name, args...]; an rpc as ["_0", id, name, args...])
ORDER = []  # ("ws", frame) and ("http", method, path) in arrival order
MOTD = {"message": "Welcome to KPD", "set_by_name": "KioCaptain", "set_at": 1763632800000}
LOOT = {"distribution_mode": "top_weighted", "clan_pct": 10, "top_player_count": 5, "top_player_pct": 70, "remainder_pct": 30,
        "min_games_required": 3, "updated_by_name": "KioCaptain", "updated_at": "2026-11-20T10:00:00.000Z"}
POST = {"recruitment_id": 77, "clan_id": CLAN_ID, "description": "We raid nightly", "looking_for": "snipers", "min_level": 5,
        "min_kd": 1.5, "region": "EU", "language": "en", "is_active": True}


def board_post(cid, tag, desc):
    return {"recruitment_id": cid, "clan_id": cid, "clan_name": tag, "description": desc, "region": "EU", "language": "en",
            "min_level": 0, "min_kd": 0}


READS = {  # GET path -> data
    "/api/clans/%d/motd" % CLAN_ID: MOTD,
    "/api/clans/%d/loot-config" % CLAN_ID: LOOT,
    "/api/clans/%d/recruitment" % CLAN_ID: POST,
    "/api/clans/recruitment": {"posts": [board_post(RC_APPLY, "APX", "Apply here"), board_post(RC_APPLIED, "APD", "Applied before"),
                                         board_post(RC_TAKEDOWN, "TKD", "Spam post")], "total": 3, "page": 1, "limit": 20},
    "/api/clans/my-applications": {"clan_ids": [RC_APPLIED]},
    "/api/clans/by-name/" + HUB_CLAN: {"clan_id": HUB_CLAN_ID, "clan_name": HUB_CLAN, "creator_name": "HubBoss", "clan_score": 900,
                                       "members": [{"p": "HubBoss", "i": 1, "r": 3}]},
}


def write_answer(method, path, body):  # (status, JSON | None for no body) of a clan write, as krunker.io answers it
    body = body if isinstance(body, dict) else {}
    base = "/api/clans/%d" % CLAN_ID
    if (method, path) == ("PUT", base + "/motd"):
        return 200, {"data": {"message": body.get("message"), "set_by_name": kae.ACCOUNT_NAME, "set_at": 1763856000000}, "error": None}
    if (method, path) == ("DELETE", base + "/motd"):
        return 204, None  # v10's wrapper: 204 is {data: null, error: null}, a success
    if (method, path) == ("PUT", base + "/loot-config"):
        return 200, {"data": dict(body, updated_by_name=kae.ACCOUNT_NAME, updated_at="2026-11-23T09:00:00.000Z"), "error": None}
    if (method, path) == ("POST", base + "/banner-color"):
        return 200, {"data": {"clan_banner_color": ANSWER_BANNER}, "error": None}
    if (method, path) == ("DELETE", base + "/logo"):
        return 200, {"data": {"success": True}, "error": None}
    if (method, path) == ("POST", base + "/recruitment"):
        return 200, {"data": dict(body, recruitment_id=77, clan_id=CLAN_ID, is_active=True), "error": None}
    if (method, path) == ("DELETE", base + "/recruitment"):
        return 200, {"data": None, "error": None}
    if (method, path) == ("POST", "/api/clans/%d/apply" % RC_APPLY):
        return 200, {"data": {"message": "Application sent!"}, "error": None}
    if (method, path) == ("DELETE", "/api/clans/%d/recruitment" % RC_TAKEDOWN):
        return 200, {"data": None, "error": None}
    if (method, path) == ("POST", "/api/clans/%d/apply" % HUB_CLAN_ID):
        return 200, {"data": {"message": "Application sent!"}, "error": None}
    if (method, path) == ("POST", base + "/logo"):
        return 200, {"data": {"message": "Logo uploaded and pending review"}, "error": None}
    return None


STUB = []  # the running kae.Stub (its tokens)


class ClanServer(kae.ThreadingHTTPServer):
    """krunker_account_e2e's HTTP stub with krunker.io's clan routes on top (its handler class subclassed)."""

    def __init__(self, addr, handler):
        def authed(h):
            stub = STUB[0]
            return h.headers.get("Authorization", "") in ("Bearer " + stub.access1, "Bearer " + stub.access2)

        class H(handler):
            def do_GET(self):
                path = urllib.parse.urlsplit(self.path).path
                if path not in READS:
                    return super().do_GET()
                self.record("GET")
                ORDER.append(("http", "GET", path))
                if authed(self):
                    self.reply(200, {"data": READS[path], "error": None})
                else:
                    self.reply(401, {"data": None, "error": {"code": "LOGIN_REQUIRED", "message": "Must be logged in"}})

            def clan_write(self, method):
                path = urllib.parse.urlsplit(self.path).path
                if not path.startswith("/api/clans/"):
                    return False
                if method == "POST" and path.endswith("/logo"):  # FormData: the raw bytes are the body
                    u = urllib.parse.urlsplit(self.path)
                    n = int(self.headers.get("Content-Length") or 0)
                    body = self.rfile.read(n) if n else b""
                    STUB[0].requests.append((method, u.path, dict(urllib.parse.parse_qsl(u.query)),
                                             {k.lower(): v for k, v in self.headers.items()}, body))
                else:
                    _, _, body = self.record(method)
                ORDER.append(("http", method, path))
                ans = write_answer(method, path, body)
                if not authed(self):
                    self.reply(401, {"data": None, "error": {"code": "LOGIN_REQUIRED", "message": "Must be logged in"}})
                elif ans is None:
                    self.reply(404, {"data": None, "error": "not found"})
                elif ans[1] is None:
                    self.send_response(ans[0])
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                else:
                    self.reply(*ans)
                return True

            def do_POST(self):
                if not self.clan_write("POST"):
                    super().do_POST()

            def do_PUT(self):
                if not self.clan_write("PUT"):
                    self.record("PUT")
                    self.reply(404, {"error": "not found"})

            def do_DELETE(self):
                if not self.clan_write("DELETE"):
                    self.record("DELETE")
                    self.reply(404, {"error": "not found"})

        super().__init__(addr, H)


kae.ThreadingHTTPServer = ClanServer  # Stub.start builds its server from this module global


def check(name, ok, detail=""):
    results.append(bool(ok))
    detail = str(detail)
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail[:500]) if show else ''}", flush=True)


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


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) .*?text=(.*)" % re.escape(label), text)
    return m.group(2).strip() if m else ""


def value_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=\d+ checked=\d+ value=(.*?) text=" % re.escape(label), text)
    return m.group(1) if m else None


def sign_in(t0):  # an official game as a guest, then v10's login modal (as kio_turf_e2e does)
    s = ["0 menu", "2500 call closWind", '2600 call krunkerJoin "%s"' % kae.GAME_PICK,
         "%d call loginOrRegister" % t0, "%d clickid knV10LoginMode" % (t0 + 500),
         "%d clickid accName" % (t0 + 700), "%d type %s" % (t0 + 750, kae.LOGIN),
         "%d clickid accPass" % (t0 + 900), "%d type %s" % (t0 + 950, kae.PASSWORD),
         "%d clickid knV10LoginSubmit" % (t0 + 1100)]
    t = t0 + 5000
    for i, d in enumerate(kae.CODE):
        s += ["%d clickid knV10Code%d" % (t, i), "%d type %s" % (t + 60, d)]
        t += 150
    return s, t + 1500


def script_a():
    s, t = sign_in(7000)
    s += ["%d call closWind" % t,
          # home tab: the loot display (getLootConfig)
          "%d call showWindow 13" % (t + 500),
          "%d waitfor .bigMenTabs" % (t + 510),
          "%d text @home [id=clan-loot-mode-display]" % (t + 3000),
          # settings tab: MOTD, loot config, recruitment post
          "%d call windows.12.switchTab 4" % (t + 3200),
          "%d waitfor [id=clanMotdInput]" % (t + 3210),
          "%d waitfor .loot-updated-info" % (t + 3220),
          "%d waitfor [id=recruit-desc]" % (t + 3230),
          "%d text @set .loot-updated-info" % (t + 5000),
          "%d text @motdmeta0 .motd-meta" % (t + 5005),
          "%d text @motd0 [id=clanMotdInput]" % (t + 5010),
          "%d text @rdesc0 [id=recruit-desc]" % (t + 5020),
          # the MOTD: typed over and saved (setMotd), then removed (deleteMotd: a 204)
          "%d clickid clanMotdInput" % (t + 5200),
          "%d down CTRL+A" % (t + 5300), "%d up CTRL+A" % (t + 5350),
          "%d type %s" % (t + 5400, NEW_MOTD),
          "%d call windows.12.motdSave" % (t + 5800),
          "%d waitfor .motd-saved" % (t + 5810),
          "%d text @motd1 [id=clanMotdInput]" % (t + 5820),
          "%d text @motdmeta .motd-meta" % (t + 5830),
          "%d call windows.12.motdRemove" % (t + 6500),
          "%d text @motd2 [id=clanMotdInput]" % (t + 8000),
          "%d nodes @motdrm .motd-error" % (t + 8010),
          # the loot config (updateLootConfig)
          "%d call windows.12.saveLoot" % (t + 8200),
          "%d waitfor .loot-config-container .success-message" % (t + 8210),
          "%d text @lootok .loot-config-container .success-message" % (t + 8220),
          "%d text @lootby .loot-updated-info" % (t + 8230),
          # the banner colour (setBannerColor with the default pick, the Red Paint)
          "%d nodes @swatch0 .swatch-grid .swatch" % (t + 8990),
          "%d call windows.12.bannerApply" % (t + 9000),
          "%d text @banner .banner-color-current" % (t + 11000),
          "%d nodes @paintsleft .banner-color-empty" % (t + 11010),
          # the logo removal (removeLogo; v10 then asks for the clan again: sendReq(3))
          "%d call windows.12.logoRemoveConfirmed" % (t + 11200),
          # the recruitment post: saved (createOrUpdateRecruitment), then removed (removeRecruitment)
          "%d call windows.12.recruitSave" % (t + 13000),
          "%d waitfor .recruitment-manage .success-message" % (t + 13010),
          "%d text @rsaved .recruitment-manage .success-message" % (t + 13020),
          "%d call windows.12.recruitRemoveConfirmed" % (t + 14000),
          "%d text @rremoved .recruitment-manage .success-message" % (t + 16000),
          "%d text @rdesc2 [id=recruit-desc]" % (t + 16010),
          ]
    u = t + 16200
    s += [  # the ownership transfer (the socket's rpcs): refused, started, cancelled; then a respond
          # (the transfer is the settings tab's last section: scrolled into view for the clicks)
          "%d setscroll menuWindow 100000" % (u - 100), "%d clickid newOwnerName" % u, "%d type Nobody" % (u + 100),
          "%d call windows.12.transferStart" % (u + 300),
          "%d text @terr .transfer-ownership-container .error-message" % (u + 1500),
          "%d setscroll menuWindow 100000" % (u + 1550), "%d clickid newOwnerName" % (u + 1600), "%d down CTRL+A" % (u + 1700), "%d up CTRL+A" % (u + 1750),
          "%d type %s" % (u + 1800, HEIR),
          "%d call windows.12.transferStart" % (u + 2000),
          "%d text @tpend .highlight-name" % (u + 3500),
          "%d call windows.12.transferCancel" % (u + 3600),
          "%d nodes @tpend2 .highlight-name" % (u + 5000), "%d nodes @tinput [id=newOwnerName]" % (u + 5010),
          "%d call windows.12.transferRespond 1" % (u + 5200),
          # the logo upload (v10's FormData), the file being KRUNKNATIVE_PICK_FILE
          "%d call windows.12.logoPick" % (u + 6600),
          # the picked file shows the buy button (iÏïiíïï's BUY 20,000 KR), which uploads it
          "%d waitfor .upload-button-container .main-button" % (u + 6700),
          "%d nodes @logobuy .upload-button-container .buy-button" % (u + 6800),
          "%d call windows.12.logoUpload" % (u + 7200),
          "%d grep @logoerr logo .error-message" % (u + 8500),
          # Discord (owner): ["cln", 9, link]
          "%d call windows.12.discordPop" % (u + 8600),
          "%d clickid clanDiscordLink" % (u + 8800), "%d type %s" % (u + 8900, DISCORD),
          "%d call windows.12.updateDis" % (u + 9500),
          "%d nodes @dispop .contPop" % (u + 10500),
          # Members: a role (["cln", 6, id, {r}]), answered with the new rank
          "%d call windows.12.switchTab 1" % (u + 10600),
          "%d waitfor [id=clanRolD%d]" % (u + 10700, MEMBER_ID),
          "%d nodes @rol0 [id=clanRolD%d]" % (u + 11000, MEMBER_ID),
          "%d call windows.12.updRol %d" % (u + 11100, MEMBER_ID),
          "%d grep @rolok Success [id=genericPop]" % (u + 12500),
          "%d text @rolsel [id=clanRolD%d]" % (u + 12510, MEMBER_ID),
          # funds: a deposit (["cln", 7, kr, {wd: 0}]), answered with the account's KR and the clan bank
          "%d call windows.12.mangFunds" % (u + 12600),
          "%d clickid clanFundsIn" % (u + 12800), "%d type 1000" % (u + 12900),
          "%d call windows.12.chngFunds 0" % (u + 13200),
          "%d grep @fundok Success [id=genericPop]" % (u + 14500),
          "%d text @kr [id=menuKRCount]" % (u + 14510),
          "%d call windows.12.mangFunds" % (u + 14600),
          "%d grep @cfunds 50,900 [id=genericPop]" % (u + 14800),
          "%d call clearPops" % (u + 14900),
          # kick (["cln", 5, id]): the member leaves the list
          '%d call windows.12.kickReq "Soldier1" %d' % (u + 15000, MEMBER_ID),
          "%d call kickClanConfirmed %d" % (u + 15200, MEMBER_ID),
          "%d nodes @rol1 [id=clanRolD%d]" % (u + 15500, MEMBER_ID),
          # requests: one accepted (["clnR", id, 1]), the rest declined (["clnRA"])
          "%d call windows.12.switchTab 3" % (u + 15600),
          "%d call windows.12.updReq 0 %d 1" % (u + 15800, REQ_ID),
          "%d call windows.12.declinePop" % (u + 16000), "%d call windows.12.decAll" % (u + 16100),
          "%d grep @reqs Other .setBodH" % (u + 16300),
          # leave (["cln", 4, 1] -> "clnL"), then create (["cln", 1, tag])
          "%d call windows.12.delReq" % (u + 16600),
          "%d call delClanConfirmed" % (u + 16800),
          "%d call showWindow 13" % (u + 18500),
          "%d waitfor [id=clanInp1]" % (u + 18600),
          "%d nodes @form [id=clanInp1]" % (u + 19000),
          "%d clickid clanInp1" % (u + 19100), "%d type %s" % (u + 19200, NEW_CLAN),
          "%d call windows.12.sendReq 1" % (u + 19500),
          # v10 keeps windows[12].tabIndex over del() and the new clan (Requests, the owner's): then Home
          "%d text @newtab .bigMenTabA" % (u + 21400),
          "%d call windows.12.switchTab 0" % (u + 21450),
          "%d grep @nwc %s .setHed" % (u + 21500, NEW_CLAN),
          "%d quit" % (u + 22000)]
    return "\n".join(s) + "\n"


def script_b():
    s, t = sign_in(7000)
    s += ["%d call closWind" % t,
          "%d call openTurfWars" % (t + 500),
          "%d call knTwTab 3" % (t + 1000),
          "%d waitfor .post-card" % (t + 1010),
          "%d grep @rc0_apx APX [id=genericPop]" % (t + 3000), "%d grep @rc0_apd APD [id=genericPop]" % (t + 3001),
          "%d grep @rc0_tkd TKD [id=genericPop]" % (t + 3002),
          "%d nodes @rcapplied0 .main-button-disabled" % (t + 3010),
          "%d nodes @rctake .takedown-btn" % (t + 3020),
          # Apply (applyToClan)
          "%d call knTwRcApply %d" % (t + 3200, RC_APPLY),
          "%d text @rc1 [id=genericPop]" % (t + 5500),
          "%d nodes @rcapplied1 .main-button-disabled" % (t + 5510),
          # Take Down (removeRecruitment of another clan's post): the confirm, then Take Down
          '%d call knTwRcTakedown %d "TKD"' % (t + 5700, RC_TAKEDOWN),
          "%d call knViewPop 1" % (t + 6200),
          "%d grep @rc2_tkd TKD [id=genericPop]" % (t + 8500),
          "%d nodes @rccards2 .post-card" % (t + 8510),
          # the Clans window's join request (["cln", 2, tag], answered with a message)
          "%d call closWind" % (t + 8600), "%d call clearPops" % (t + 8650),
          "%d call showWindow 13" % (t + 8800),
          "%d waitfor [id=clanInp2]" % (t + 8810),
          "%d clickid clanInp2" % (t + 9000), "%d type APX" % (t + 9100),
          "%d call windows.12.sendReq 2" % (t + 9400),
          "%d text @joinmsg [id=clanErr]" % (t + 11000),
          # the hub's clan view: Request to Join (applyToClan)
          "%d call closWind" % (t + 11100),
          '%d call openClanView "%s"' % (t + 11300, HUB_CLAN),
          "%d waitfor [id=v10sClanJoin]" % (t + 11310),
          "%d text @hubjoin0 [id=v10sClanJoin]" % (t + 13000),
          '%d call v10s "apply"' % (t + 13100),
          "%d text @hubjoin1 [id=v10sClanJoin]" % (t + 15000),
          "%d quit" % (t + 15500)]
    return "\n".join(s) + "\n"


def run_client(stub, name, script):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    log = os.path.join(WORK, name + ".log")
    base = "http://127.0.0.1:%d" % stub.http_port
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                # no brain: the official game must not need one (a refused port answers nothing)
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:9", "KRUNKNATIVE_FINDER_URL": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
                "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS,
                "KRUNKNATIVE_ASSET_FETCH": "off",
                "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH,
                "KRUNKNATIVE_PICK_FILE": os.path.join(WORK, LOGO_NAME)})
    p = subprocess.Popen(["xvfb-run", "-a", "stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(240)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check(name + ": client finished its script", False, "timed out; log " + log)
    return read_log(log)


def bearer(r):
    return r[3].get("authorization", "").startswith("Bearer ")


def one(stub, method, path):
    rs = stub.find(method, path)
    return rs[-1] if rs else None


def checks_a(text, stub, cln_before_logo):
    check("A. the official game is the active backend", "[account] active account backend: krunker.io" in text,
          re.findall(r"\[account\] active.*", text)[:2])
    check("A. the Clans window asks the game socket for the clan (\"cln\" 3)", "cln" in stub.ws_frames, stub.ws_frames[-12:])
    base = "/api/clans/%d" % CLAN_ID
    for path, what in ((base + "/loot-config", "getLootConfig"), (base + "/motd", "getMotd"), (base + "/recruitment", "getRecruitment")):
        r = one(stub, "GET", path)
        check("A. %s: GET %s with the Bearer (clanData.clan_id)" % (what, path[4:]), r is not None and bearer(r),
              [(x[0], x[1]) for x in stub.requests if x[1].startswith("/api/clans/")][:10])
    home = text_of(text, "home")
    check("A. the home tab's loot display is krunker.io's (Top Weighted, 10% cut, 5 get 70%, min 3 games)",
          all(s in home for s in ("Top Weighted", "10%", "5 get 70%")), home[-300:])
    st = text_of(text, "set")
    check("A. the settings tab shows krunker.io's loot config (Last updated by KioCaptain)", "Last updated by KioCaptain" in st, st[:300])
    check("A. ... and its MOTD (in the textarea, set by KioCaptain)",
          value_of(text, "motd0") == MOTD["message"] and "Set by KioCaptain" in text_of(text, "motdmeta0"),
          (value_of(text, "motd0"), text_of(text, "motdmeta0")))
    check("A. ... and its recruitment post", value_of(text, "rdesc0") == POST["description"], value_of(text, "rdesc0"))

    r = one(stub, "PUT", base + "/motd")
    check("A. setMotd: PUT /clans/9001/motd {message} with the Bearer", r is not None and bearer(r) and r[4] == {"message": NEW_MOTD},
          r and (r[4], bearer(r)))
    check("A. ... and the window shows the saved MOTD (Saved!, set by the account)",
          value_of(text, "motd1") == NEW_MOTD and ("Set by " + kae.ACCOUNT_NAME) in text_of(text, "motdmeta"),
          (value_of(text, "motd1"), text_of(text, "motdmeta")))
    r = one(stub, "DELETE", base + "/motd")
    check("A. deleteMotd: DELETE /clans/9001/motd with the Bearer, no body", r is not None and bearer(r) and r[4] is None, r and r[4])
    check("A. ... its 204 is a success (v10's wrapper): the MOTD is cleared, no error", value_of(text, "motd2") == "" and nodes(text, "motdrm") == 0,
          (value_of(text, "motd2"), nodes(text, "motdrm")))

    r = one(stub, "PUT", base + "/loot-config")
    want = {k: LOOT[k] for k in ("distribution_mode", "clan_pct", "top_player_count", "top_player_pct", "remainder_pct", "min_games_required")}
    check("A. updateLootConfig: PUT /clans/9001/loot-config with v10's six fields and the Bearer", r is not None and bearer(r) and r[4] == want,
          r and r[4])
    check("A. ... and the window says saved, updated by the account", "Config saved successfully!" in text_of(text, "lootok")
          and ("Last updated by " + kae.ACCOUNT_NAME) in text_of(text, "lootby"), (text_of(text, "lootok"), text_of(text, "lootby")))

    r = one(stub, "POST", base + "/banner-color")
    check("A. setBannerColor: POST /clans/9001/banner-color {paint_item_id: 7776} with the Bearer",
          r is not None and bearer(r) and r[4] == {"paint_item_id": RED_PAINT}, r and r[4])
    check("A. ... and the current colour is the answer's", ANSWER_BANNER in text_of(text, "banner"), text_of(text, "banner"))
    check("A. ... and the spent Red Paint is taken off the account (its one unit: no paint left to pick)",
          nodes(text, "swatch0") == 1 and nodes(text, "paintsleft") == 1, (nodes(text, "swatch0"), nodes(text, "paintsleft")))

    r = one(stub, "DELETE", base + "/logo")
    check("A. removeLogo: DELETE /clans/9001/logo with the Bearer", r is not None and bearer(r), r and r[1])
    check("A. ... and the clan is asked for again (sendReq(3))", stub.ws_frames.count("cln") > cln_before_logo[0] if cln_before_logo else False,
          (stub.ws_frames.count("cln"), cln_before_logo))

    r = one(stub, "POST", base + "/recruitment")
    want = {k: POST[k] for k in ("description", "looking_for", "min_level", "min_kd", "region", "language")}
    check("A. createOrUpdateRecruitment: POST /clans/9001/recruitment with the form's post and the Bearer",
          r is not None and bearer(r) and r[4] == want, r and r[4])
    check("A. ... and the window says updated (the post was active)", "Recruitment post updated!" in text_of(text, "rsaved"), text_of(text, "rsaved"))
    r = one(stub, "DELETE", base + "/recruitment")
    check("A. removeRecruitment: DELETE /clans/9001/recruitment with the Bearer", r is not None and bearer(r), r and r[1])
    check("A. ... and the form is cleared (Recruitment post removed.)", "Recruitment post removed." in text_of(text, "rremoved")
          and value_of(text, "rdesc2") == "", (text_of(text, "rremoved"), value_of(text, "rdesc2")))

    checks_a_socket(text, stub)

    writes = [(r[0], r[1]) for r in stub.requests if r[0] != "GET" and r[1].startswith("/api/clans/")]
    allowed = {("PUT", base + "/motd"), ("DELETE", base + "/motd"), ("PUT", base + "/loot-config"), ("POST", base + "/banner-color"),
               ("DELETE", base + "/logo"), ("POST", base + "/recruitment"), ("DELETE", base + "/recruitment"), ("POST", base + "/logo")}
    check("A. no other clan write reached krunker.io (the transfer is the socket's rpcs in v10, not gapi)",
          set(writes) <= allowed and not stub.find("POST", base + "/transfer") and not stub.find("DELETE", base + "/transfer"), writes)
    check("A. each write went out once", len(writes) == len(set(writes)), writes)


def frames(name, *head):  # the clan frames sent, [name, args...], whose first args are head
    return [f for f in WS if f[0] == name and list(f[1:1 + len(head)]) == list(head)]


def rpcs(name):  # the rpcs sent of that name: ["_0", id, name, args...]
    return [f for f in WS if f[0] == "_0" and len(f) > 2 and f[2] == name]


def asked_after(event):  # a ["cln", 3, ...] went out after that event (v10's sendReq(3))
    i = ORDER.index(event) if event in ORDER else len(ORDER)
    return any(e[0] == "ws" and e[1][:2] == ["cln", 3] for e in ORDER[i + 1:])


def multipart(r):  # (boundary, [(headers dict, bytes)]) of a FormData request, or None
    ctype = r[3].get("content-type", "")
    m = re.match(r"multipart/form-data; boundary=(\S+)$", ctype)
    if not m or not isinstance(r[4], bytes):
        return None
    b = m.group(1).encode()
    body = r[4]
    if not body.startswith(b"--" + b + b"\r\n") or not body.endswith(b"\r\n--" + b + b"--\r\n"):
        return m.group(1), None
    parts = []
    for chunk in body[len(b) + 4:-(len(b) + 8)].split(b"\r\n--" + b + b"\r\n"):
        head, _, data = chunk.partition(b"\r\n\r\n")
        hs = dict(line.split(": ", 1) for line in head.decode().split("\r\n") if ": " in line)
        parts.append(({k.lower(): v for k, v in hs.items()}, data))
    return m.group(1), parts


def checks_a_socket(text, stub):
    # the ownership transfer
    r = rpcs("clan-transfer-initiate")
    check("A. transferOwnership: the socket's rpc [\"_0\", rpcId, \"clan-transfer-initiate\", name], once per Transfer",
          [f[3:] for f in r] == [["Nobody"], [HEIR]] and all(isinstance(f[1], int) for f in r), r)
    check("A. ... its res.error is thrown: \"Error: Player not found\"", "Error: Player not found" in text_of(text, "terr"), text_of(text, "terr"))
    check("A. ... and res.data is the ownership_transfer: pending to Heir", text_of(text, "tpend") == HEIR, text_of(text, "tpend"))
    r = rpcs("clan-transfer-cancel")
    check("A. cancelTransfer: [\"_0\", rpcId, \"clan-transfer-cancel\"] (no args)", len(r) == 1 and len(r[0]) == 3, r)
    check("A. ... the transfer is gone: the name input is back", nodes(text, "tpend2") == 0 and nodes(text, "tinput") == 1,
          (nodes(text, "tpend2"), nodes(text, "tinput")))
    r = rpcs("clan-transfer-respond")
    check("A. respondToTransfer(true): [\"_0\", rpcId, \"clan-transfer-respond\", true]", len(r) == 1 and r[0][3:] == [True], r)
    ids = [f[1] for f in WS if f[0] == "_0"]
    check("A. ... each rpc its own id (v10's rpcCounter)", len(ids) == len(set(ids)) == 4, ids)
    check("A. ... and its success asks for the clan again (sendReq(3))", asked_after(("ws", r[0])) if r else False)

    # the logo upload
    r = one(stub, "POST", "/api/clans/%d/logo" % CLAN_ID)
    mp = multipart(r) if r else None
    check("A. logoPick: the picked file shows the buy button", nodes(text, "logobuy") == 1, nodes(text, "logobuy"))
    check("A. uploadLogo: POST /clans/9001/logo with the Bearer and an X-Idempotency-Key", r is not None and bearer(r) and
          r[3].get("x-idempotency-key"), r and {k: v for k, v in r[3].items() if k != "authorization"})
    check("A. ... as multipart/form-data with Chromium's boundary (----WebKitFormBoundary + 16)",
          mp is not None and re.match(r"----WebKitFormBoundary[A-Za-z0-9]{16}$", mp[0]) is not None, r and r[3].get("content-type"))
    part = mp[1][0] if mp and mp[1] and len(mp[1]) == 1 else None
    check("A. ... one part: name=\"file\", the file's name, Content-Type image/png",
          part is not None and part[0].get("content-disposition") == 'form-data; name="file"; filename="%s"' % LOGO_NAME and
          part[0].get("content-type") == "image/png", mp and mp[1] and [p[0] for p in mp[1]])
    check("A. ... holding the PNG's bytes as they are", part is not None and part[1] == LOGO, part and (len(part[1]), len(LOGO)))
    check("A. ... no error, and the clan is asked for again (sendReq(3))", grep(text, "logoerr") == 0 and
          asked_after(("http", "POST", "/api/clans/%d/logo" % CLAN_ID)), grep(text, "logoerr"))

    # the game socket's writes
    check("A. updateDis: [\"cln\", 9, the link] (the owner)", frames("cln", 9) == [["cln", 9, DISCORD]], frames("cln", 9))
    check("A. ... its ok leaves the Discord popup open (v10's resp(9) takes the generic path: no msg, no data)",
          nodes(text, "dispop") == 1, nodes(text, "dispop"))
    check("A. updRol: [\"cln\", 6, id, {r}] (the select's rank)", frames("cln", 6) == [["cln", 6, MEMBER_ID, {"r": 1}]], frames("cln", 6))
    check("A. ... the answer's rank is the member's: Success, the select reads Captain",
          grep(text, "rolok") > 0 and nodes(text, "rol0") == 1 and "Captain" in text_of(text, "rolsel"), (grep(text, "rolok"), text_of(text, "rolsel")))
    check("A. chngFunds(0): [\"cln\", 7, 1000, {wd: 0}]", frames("cln", 7) == [["cln", 7, 1000, {"wd": 0}]], frames("cln", 7))
    check("A. ... the answer's pf is the krunker.io account's KR, cf the clan bank",
          grep(text, "fundok") > 0 and text_of(text, "kr") == "4,321" and grep(text, "cfunds") > 0,
          (grep(text, "fundok"), text_of(text, "kr"), grep(text, "cfunds")))
    check("A. kick: [\"cln\", 5, id], and the member leaves the list", frames("cln", 5) == [["cln", 5, MEMBER_ID]] and nodes(text, "rol1") == 0,
          (frames("cln", 5), nodes(text, "rol1")))
    check("A. updReq: [\"clnR\", id, 1]", frames("clnR") == [["clnR", REQ_ID, 1]], frames("clnR"))
    check("A. decAll: [\"clnRA\"], and the requests are gone", frames("clnRA") == [["clnRA"]] and grep(text, "reqs") == 0,
          (frames("clnRA"), grep(text, "reqs")))
    check("A. delClanConfirmed: [\"cln\", 4, 1]", frames("cln", 4) == [["cln", 4, 1]], frames("cln", 4))
    check("A. ... \"clnL\" ends the clan: acc.clan = null, the create / join form", nodes(text, "form") == 1, nodes(text, "form"))
    check("A. sendReq(1): [\"cln\", 1, the tag]", frames("cln", 1) == [["cln", 1, NEW_CLAN]], frames("cln", 1))
    check("A. ... the answer has no clan_id: the clan is asked for (sendReq(3)), and the window is the new clan's",
          asked_after(("ws", ["cln", 1, NEW_CLAN])) and grep(text, "nwc") > 0, grep(text, "nwc"))
    check("A. ... on the tab the window was left on (v10 keeps tabIndex: Requests)", text_of(text, "newtab") == "Requests",
          text_of(text, "newtab"))
    bad = [f for f in WS if f[0] == "cln" and (len(f) < 2 or f[1] not in (1, 3, 4, 5, 6, 7, 9))]
    check("A. no other clan frame went out", not bad, bad)


def checks_b(text, stub):
    r = [x for x in stub.requests if x[0] == "GET" and x[1] == "/api/clans/recruitment"]
    check("B. the Recruitment board: GET /clans/recruitment?page=1&limit=20 from krunker.io",
          bool(r) and r[-1][2].get("page") == "1" and r[-1][2].get("limit") == "20", [x[2] for x in r][:3])
    r = one(stub, "GET", "/api/clans/my-applications")
    check("B. getMyApplications: GET /clans/my-applications with the Bearer, no token", r is not None and bearer(r) and "token" not in r[2],
          r and (r[2], bearer(r)))
    check("B. the board lists krunker.io's posts", all(grep(text, "rc0_" + s) > 0 for s in ("apx", "apd", "tkd")),
          [grep(text, "rc0_" + s) for s in ("apx", "apd", "tkd")])
    check("B. ... the applied clan reads Applied (one disabled button), and a moderator sees Take Down",
          nodes(text, "rcapplied0") == 1 and nodes(text, "rctake") == 3, (nodes(text, "rcapplied0"), nodes(text, "rctake")))
    r = one(stub, "POST", "/api/clans/%d/apply" % RC_APPLY)
    check("B. applyToClan: POST /clans/4242/apply with the Bearer and no body", r is not None and bearer(r) and r[4] is None, r and r[4])
    check("B. ... and that clan reads Applied too (v10 adds it to the applied set)", nodes(text, "rcapplied1") == 2, nodes(text, "rcapplied1"))
    r = one(stub, "DELETE", "/api/clans/%d/recruitment" % RC_TAKEDOWN)
    check("B. Take Down: DELETE /clans/4444/recruitment with the Bearer", r is not None and bearer(r), r and r[1])
    check("B. ... and the post leaves the board", grep(text, "rc2_tkd") == 0 and nodes(text, "rccards2") == 2,
          (nodes(text, "rccards2"), grep(text, "rc2_tkd")))
    check("B. the Clans window's join: [\"cln\", 2, the tag], its answer's message in #clanErr",
          frames("cln", 2) == [["cln", 2, "APX"]] and text_of(text, "joinmsg") == "Request sent to APX!", (frames("cln", 2), text_of(text, "joinmsg")))
    check("B. the hub's clan view shows Request to Join to the krunker.io account (no clan)", text_of(text, "hubjoin0") == "group_add Request to Join",
          text_of(text, "hubjoin0"))
    r = one(stub, "POST", "/api/clans/%d/apply" % HUB_CLAN_ID)
    check("B. ... and sends applyToClan: POST /clans/4545/apply with the Bearer, no body", r is not None and bearer(r) and r[4] is None, r and r[4])
    check("B. ... then reads Request Sent", text_of(text, "hubjoin1") == "check Request Sent", text_of(text, "hubjoin1"))
    writes = [(x[0], x[1]) for x in stub.requests if x[0] != "GET" and x[1].startswith("/api/clans/")]
    check("B. no other clan write reached krunker.io", sorted(set(writes)) == sorted({("POST", "/api/clans/%d/apply" % RC_APPLY),
                                                                                    ("DELETE", "/api/clans/%d/recruitment" % RC_TAKEDOWN),
                                                                                    ("POST", "/api/clans/%d/apply" % HUB_CLAN_ID)}), writes)


async def answer_cln(send, msg):  # windows[12]'s "cln" frames, answered as krunker.io's game server ("cln" [msg, data, ok, action])
    f = kae.frame_args(msg)
    WS.append(f)
    ORDER.append(("ws", f))
    action = f[1] if len(f) > 1 else None
    if action == 3:  # the clan page
        await send("cln", "", STATE["clan"], 1, 3)
    elif action == 1:  # create: the new clan, without its id (v10 then asks: clan_id || sendReq(3))
        STATE["clan"] = {"clan_name": f[2], "clan_id": NEW_CLAN_ID, "clan_score": 0, "clan_funds": 0, "clan_playerid": kae.ACCOUNT_ID,
                         "members": [{"p": kae.ACCOUNT_NAME, "i": kae.ACCOUNT_ID, "r": 3}], "requests": []}
        await send("cln", "", {k: v for k, v in STATE["clan"].items() if k != "clan_id"}, 1, 1)
    elif action == 2:  # join: a request sent
        await send("cln", "Request sent to %s!" % f[2], None, 1, 2)
    elif action == 4:  # leave / delete
        STATE["clan"] = None
        await send("clnL")
    elif action == 6:  # role: the member's new rank
        await send("cln", "", {"i": f[2], "r": 2}, 1, 6)
    elif action == 7:  # funds: the account's KR and the clan bank
        await send("cln", "", {"pf": KR_AFTER, "cf": CLAN_FUNDS_AFTER}, 1, 7)
    elif action == 9:  # Discord
        await send("cln", "", None, 1, 9)


async def record_frame(send, msg):  # "clnR" / "clnRA": no answer
    f = kae.frame_args(msg)
    WS.append(f)
    ORDER.append(("ws", f))


async def answer_rpc(send, msg):  # ["_0", rpcId, name, args...]: the transfer's rpcs answered ["_0", rpcId, {data, error}]
    f = kae.frame_args(msg)
    if len(f) < 3 or not str(f[2]).startswith("clan-transfer-"):
        return
    WS.append(f)
    ORDER.append(("ws", f))
    if f[2] == "clan-transfer-initiate" and f[3:] == ["Nobody"]:
        await send("_0", f[1], {"data": None, "error": "Player not found"})
    elif f[2] == "clan-transfer-initiate":
        await send("_0", f[1], {"data": {"to_player_name": f[3], "to_player_id": MEMBER_ID, "from_player_id": kae.ACCOUNT_ID}, "error": None})
    else:
        await send("_0", f[1], {"data": None, "error": None})


HANDLERS = {"cln": answer_cln, "clnR": record_frame, "clnRA": record_frame, "_0": answer_rpc}


def run_stub(account, handlers, fn):
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    STUB[:] = [stub]
    stub.account_data = [account]
    stub.frame_handlers = dict(handlers)
    WS.clear()
    ORDER.clear()
    try:
        stub.start()
        fn(stub)
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


def main():
    if kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("kio_clan_e2e: no client_bin in %s: skipped" % BUILD)
        return 77

    cln_before_logo = []

    def run_a(stub):
        orig = stub.frame_handlers["cln"]

        async def counted(send, msg):  # the "cln" count when the logo removal goes out (its answer asks once more)
            if stub.find("DELETE", "/api/clans/%d/logo" % CLAN_ID) and not cln_before_logo:
                cln_before_logo.append(stub.ws_frames.count("cln") - 1)
            await orig(send, msg)

        stub.frame_handlers["cln"] = counted
        text = run_client(stub, "a", script_a())
        if not cln_before_logo:
            cln_before_logo.append(stub.ws_frames.count("cln"))
        checks_a(text, stub, cln_before_logo)

    with open(os.path.join(WORK, LOGO_NAME), "wb") as f:
        f.write(LOGO)
    STATE["clan"] = copy.deepcopy(KIO_CLAN)
    run_stub({"player_clan": CLAN, "skins": [{"ind": RED_PAINT, "cnt": 1}]}, HANDLERS, run_a)
    STATE["clan"] = None
    run_stub({"player_role": 2}, HANDLERS, lambda stub: checks_b(run_client(stub, "b", script_b()), stub))
    print(f"{sum(results)}/{len(results)} passed")
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())
