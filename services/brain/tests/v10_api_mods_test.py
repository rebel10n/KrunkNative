#!/usr/bin/env python3
"""The v10 moderation, mods and KPD routes on kn_brain's root paths (services/brain/v10_mods.c;
route inventory doc/v10/api-inventory.md rows 158-160, 173-178, 190-193).

  v10_api_mods_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root; a build dir means BUILD/services/brain/kn_brain.)

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
Five accounts register over POST /auth/register (a developer, a moderator, a plain player who owns
the mods, another player, and the target of the moderation calls). Staff flags and the rows the
routes read (published_mods, kpd_demos, kpd_calls) are seeded through the scratch database.

One or more checks per route. Each names the method and path, the v10.0.3 consumer line (the
decoded bundle's gapi wrapper, line 5161 for these routes, or the moderation wrapper on the same
line) and the v10.js line of the v10.0.2 caller the inventory cites, then the status and the JSON
fields the caller reads:

  200  the data v10 reads (the field names of the caller's own code)
  403  FORBIDDEN: a staff-only write from a player without the staff flag (code is never NOT_IMPLEMENTED)
  403  NOT_IMPLEMENTED: a refusal, only for a write KrunkNative cannot do (mod upload)
  401  LOGIN_REQUIRED: a write or a per-caller read without a session
  404  NOT_FOUND: an unknown resource, or a demo the caller may not open (the caller shows its message)
  400  BAD_REQUEST: a value outside the caller's contract

Exit 0 when every check passes, 1 otherwise, 77 when kn_brain or the sqlite scratch database is
unavailable (CTest's skip).
"""
import json
import os
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ARG = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BRAIN = os.path.join(ARG, "services", "brain", "kn_brain") if os.path.isdir(ARG) else ARG
NAME = "kn_test_v10mods_%d" % os.getpid()
SECRET = "kn-v10-mods-test-secret-%d" % os.getpid()
PASS = "hunter22"
RESULTS = []

# the five players: who, the username, the staff flags seeded for them
WHO = {
    "dev": ("v10mods_dev", {"player_developer": 1}),
    "mod": ("v10mods_mod", {"player_moderator": 1}),
    "plain": ("v10mods_plain", {}),
    "other": ("v10mods_other", {}),
    "target": ("v10mods_target", {}),
}

# the v10.0.3 consumer line of the gapi wrappers these routes call (decoded bundle)
W = "v10.0.3 decoded 5161"


def check(name, ok, detail=""):
    RESULTS.append(bool(ok))
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not detail else "  -- " + detail), flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def http(port, method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def err_of(reply):
    err = reply.get("error") if isinstance(reply, dict) else None
    return err if isinstance(err, dict) else {}


def data_of(reply):
    return reply.get("data") if isinstance(reply, dict) else None


def brief(status, reply):
    return "HTTP %d %s" % (status, json.dumps(reply)[:180])


def refused(status, reply, code):
    """a refusal in the consumer's shape: status, error.code, error.message, no data"""
    err = err_of(reply)
    return status == 403 and err.get("code") == code and bool(err.get("message")) and "data" not in reply


def error_code(status, reply, code, want):
    err = err_of(reply)
    return status == want and err.get("code") == code and bool(err.get("message")) and "data" not in reply


def db_q(path, sql, args=()):
    con = sqlite3.connect(path, timeout=10)
    try:
        return con.execute(sql, args).fetchall()
    finally:
        con.close()


def db_x(path, sql, args=()):
    con = sqlite3.connect(path, timeout=10)
    try:
        con.execute(sql, args)
        con.commit()
    finally:
        con.close()


def seed(db):
    """the staff flags and the rows the routes read (kpd_demos, published_mods, kpd_calls)"""
    path = db["path"]
    for who, (name, flags) in WHO.items():
        for col, val in flags.items():
            db_x(path, "UPDATE players SET %s = ? WHERE player_id = (SELECT id FROM accounts WHERE username = ?)" % col,
                 (val, name))
    ids = {who: db_q(path, "SELECT id FROM accounts WHERE username = ?", (name,))[0][0] for who, (name, _) in WHO.items()}
    # two mods of the plain player: TestMod stays, DoomedMod is deleted by the test
    db_x(path, "INSERT INTO published_mods (mod_name, creator_id, mod_info) VALUES ('TestMod', ?, '{}')", (ids["plain"],))
    db_x(path, "INSERT INTO published_mods (mod_name, creator_id, mod_info) VALUES ('DoomedMod', ?, '{}')", (ids["plain"],))
    now = "strftime('%Y-%m-%d %H:%M:%f', 'now')"
    demos = [
        # token, ct, suspect id, suspect name, group, typ, map, mode, banned, opened
        ("tokA", "ctA", ids["target"], WHO["target"][0], 0, 1, "Burg", "Team", 1, 1),
        ("tokB", "ctB", ids["target"], WHO["target"][0], 0, 1, "Burg", "Team", 1, 1),
        ("tokC", "ctC", None, "guestname", 7, 0, "Sandstorm", "FFA", 0, 1),
        ("tokD", "ctD", ids["other"], WHO["other"][0], 0, 0, "Kanji", "Team", 0, 1),
        ("tokE", "ctE", None, "closedguest", 8, 0, "Burg", "FFA", 0, 0),
    ]
    for token, ct, sid, sname, grp, typ, mp, mode, banned, opened in demos:
        db_x(path, "INSERT INTO kpd_demos (token, ct, suspect_id, suspect_name, suspect_group, typ, map, mode_name,"
                   " duration_ms, banned, opened) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 61000, ?, ?)",
             (token, ct, sid, sname, grp, typ, mp, mode, banned, opened))
    # the officers' handled calls: mod 2 (the last 30 days), dev 1
    for officer, count in (("mod", 2), ("dev", 1)):
        for _ in range(count):
            db_x(path, "INSERT INTO kpd_calls (reporter_id, reporter_name, suspect_name, status, handled_by, handled_at)"
                       " VALUES (?, 'v10mods_plain', 'v10mods_target', 1, ?, %s)" % now, (ids["plain"], ids[officer]))
    return ids


def scenario(port, db):
    path = db["path"]
    tok = {}
    for who, (name, _) in WHO.items():
        status, body = http(port, "POST", "/auth/register", {"username": name, "password": PASS})
        t = body.get("token") if isinstance(body, dict) else None
        check("a Box Office account registers: %s" % name, status == 200 and bool(t), brief(status, body))
        tok[who] = t
    if not all(tok.values()):
        return
    seed(db)
    ids = {who: db_q(path, "SELECT id FROM accounts WHERE username = ?", (name,))[0][0] for who, (name, _) in WHO.items()}
    mod_id = db_q(path, "SELECT mod_id FROM published_mods WHERE mod_name = 'TestMod'")[0][0]
    doomed_id = db_q(path, "SELECT mod_id FROM published_mods WHERE mod_name = 'DoomedMod'")[0][0]
    target = WHO["target"][0]

    # ---- 158 GET /moderation/chat/{name}: the chat ban state (staff; others see it as not disclosed)
    s, b = http(port, "GET", "/moderation/chat/" + target, token=tok["mod"])
    check("158 GET /moderation/chat/{name} as staff: data.chat_banned false before any ban (%s, v10.js 27087 read at 110003, 217008)" % W,
          s == 200 and data_of(b) == {"chat_banned": False}, brief(s, b))
    s, b = http(port, "GET", "/moderation/chat/" + target, token=tok["plain"])
    check("158 GET /moderation/chat/{name} as a player without staff: data.chat_banned null, not disclosed",
          s == 200 and data_of(b) == {"chat_banned": None}, brief(s, b))
    s, b = http(port, "GET", "/moderation/chat/v10mods_nobody", token=tok["mod"])
    check("158 GET /moderation/chat/{name} unknown player: 404 NOT_FOUND", error_code(s, b, "NOT_FOUND", 404), brief(s, b))

    # ---- 159 POST /moderation/chat: a chat ban (feature_bans 'chat', moderation_log)
    s, b = http(port, "POST", "/moderation/chat", {"player_name": target, "ban": True}, token=tok["mod"])
    d = data_of(b)
    check("159 POST /moderation/chat {player_name, ban:true} as staff: data truthy, data.chat_banned true (%s, v10.js 27092 read at 109549)" % W,
          s == 200 and isinstance(d, dict) and d.get("chat_banned") is True, brief(s, b))
    rows = db_q(path, "SELECT count(*) FROM feature_bans WHERE account_id = ? AND feature = 'chat' AND revoked_at IS NULL",
                (ids["target"],))
    check("159 the ban is a feature_bans row ('chat', not revoked)", rows and rows[0][0] == 1, str(rows))
    logs = db_q(path, "SELECT count(*) FROM moderation_log WHERE action = 'chat_ban' AND actor_id = ? AND target_id = ?",
                (ids["mod"], ids["target"]))
    check("159 moderation_log keeps the ban (chat_ban by the moderator)", logs and logs[0][0] == 1, str(logs))
    s, b = http(port, "GET", "/moderation/chat/" + target, token=tok["mod"])
    check("158 after the ban, GET reports data.chat_banned true", s == 200 and data_of(b) == {"chat_banned": True}, brief(s, b))
    s, b = http(port, "POST", "/moderation/chat", {"player_name": target, "ban": True}, token=tok["plain"])
    check("159 POST /moderation/chat as a player without staff: 403 FORBIDDEN, a reason, no data (not NOT_IMPLEMENTED)",
          refused(s, b, "FORBIDDEN"), brief(s, b))
    s, b = http(port, "POST", "/moderation/chat", {"player_name": "v10mods_nobody", "ban": True}, token=tok["mod"])
    check("159 POST /moderation/chat unknown player: 404 NOT_FOUND", error_code(s, b, "NOT_FOUND", 404), brief(s, b))
    s, b = http(port, "POST", "/moderation/chat", {"player_name": target, "ban": True})
    check("159 POST /moderation/chat without a session: 401 LOGIN_REQUIRED", error_code(s, b, "LOGIN_REQUIRED", 401), brief(s, b))
    s, b = http(port, "POST", "/moderation/chat", {"player_name": target, "ban": False}, token=tok["mod"])
    check("159 POST /moderation/chat {ban:false} as staff: data.chat_banned false (unban)",
          s == 200 and data_of(b) == {"player_name": target, "chat_banned": False}, brief(s, b))
    rows = db_q(path, "SELECT count(*) FROM feature_bans WHERE account_id = ? AND feature = 'chat' AND revoked_at IS NULL",
                (ids["target"],))
    revoked = db_q(path, "SELECT count(*) FROM feature_bans WHERE account_id = ? AND feature = 'chat' AND revoked_at IS NOT NULL",
                   (ids["target"],))
    check("159 the unban revokes the feature ban (the row stays, revoked)", rows and rows[0][0] == 0
          and revoked and revoked[0][0] == 1, str(rows) + " " + str(revoked))

    # ---- 160 POST /moderation/flag: the staff hacker flag (players.player_hack, moderation_log)
    s, b = http(port, "POST", "/moderation/flag", {"player_name": target, "flag": True}, token=tok["mod"])
    check("160 POST /moderation/flag {player_name, flag:true} as staff: data truthy (%s, v10.js 27098 read at 109570)" % W,
          s == 200 and isinstance(data_of(b), dict), brief(s, b))
    hack = db_q(path, "SELECT player_hack FROM players WHERE player_id = ?", (ids["target"],))
    check("160 the flag sets players.player_hack to 1", hack and hack[0][0] == 1, str(hack))
    logs = db_q(path, "SELECT count(*) FROM moderation_log WHERE action = 'flag' AND actor_id = ? AND target_id = ?",
                (ids["mod"], ids["target"]))
    check("160 moderation_log keeps the flag", logs and logs[0][0] == 1, str(logs))
    s, b = http(port, "POST", "/moderation/flag", {"player_name": target, "flag": True}, token=tok["plain"])
    check("160 POST /moderation/flag as a player without staff: 403 FORBIDDEN (not NOT_IMPLEMENTED)",
          refused(s, b, "FORBIDDEN"), brief(s, b))
    s, b = http(port, "POST", "/moderation/flag", {"player_name": target, "flag": False}, token=tok["mod"])
    hack = db_q(path, "SELECT player_hack FROM players WHERE player_id = ?", (ids["target"],))
    check("160 POST /moderation/flag {flag:false} clears player_hack", s == 200 and hack and hack[0][0] == 0,
          brief(s, b) + " " + str(hack))

    # ---- 173 POST /mods/publish/key: the upload key (KrunkNative has no mod file storage: a refusal)
    s, b = http(port, "POST", "/mods/publish/key", {"name": "TestMod", "assetSizes": {"mod": 1}}, token=tok["dev"])
    check("173 POST /mods/publish/key: 403 NOT_IMPLEMENTED refusal with a reason (%s, v10.js 27195 read at 213947)" % W,
          refused(s, b, "NOT_IMPLEMENTED"), brief(s, b))

    # ---- 174 GET /mods/by-name/{name}: the published mod (published_mods)
    s, b = http(port, "GET", "/mods/by-name/TestMod")
    d = data_of(b) or {}
    check("174 GET /mods/by-name/{name}: data.mod_id, data.name, data.creator_name, data.featured, data.version (%s, v10.js 27201 read at 107663)" % W,
          s == 200 and d.get("mod_id") == mod_id and d.get("name") == "TestMod" and d.get("creator_name") == WHO["plain"][0]
          and d.get("featured") in (0, False) and d.get("version") is None, brief(s, b))
    s, b = http(port, "GET", "/mods/by-name/NoSuchMod")
    check("174 GET /mods/by-name/{name} unknown name: 404 NOT_FOUND", error_code(s, b, "NOT_FOUND", 404), brief(s, b))

    # ---- 175 POST /mods/{id}/vote: creation_votes (-1, 0, 1), the net votes on the mod
    s, b = http(port, "POST", "/mods/%d/vote" % mod_id, {"vote": 1}, token=tok["other"])
    check("175 POST /mods/{id}/vote {vote:1}: data.id, data.vote, data.votes (%s, v10.js 27203)" % W,
          s == 200 and data_of(b) == {"id": mod_id, "vote": 1, "votes": 1}, brief(s, b))
    s, b = http(port, "POST", "/mods/%d/vote" % mod_id, {"vote": -1}, token=tok["mod"])
    check("175 a second player's vote -1 nets to 0 votes", s == 200 and data_of(b) == {"id": mod_id, "vote": -1, "votes": 0}, brief(s, b))
    s, b = http(port, "POST", "/mods/%d/vote" % mod_id, {"vote": 0}, token=tok["other"])
    check("175 vote 0 withdraws the vote: votes -1", s == 200 and data_of(b) == {"id": mod_id, "vote": 0, "votes": -1}, brief(s, b))
    mv = db_q(path, "SELECT mod_votes FROM published_mods WHERE mod_id = ?", (mod_id,))
    check("175 published_mods.mod_votes follows the creation_votes sum", mv and mv[0][0] == -1, str(mv))
    s, b = http(port, "POST", "/mods/%d/vote" % mod_id, {"vote": 1})
    check("175 POST /mods/{id}/vote without a session: 401 LOGIN_REQUIRED", error_code(s, b, "LOGIN_REQUIRED", 401), brief(s, b))
    s, b = http(port, "POST", "/mods/%d/vote" % mod_id, {"vote": 5}, token=tok["other"])
    check("175 POST /mods/{id}/vote a value outside -1..1: 400", error_code(s, b, "BAD_REQUEST", 400), brief(s, b))
    s, b = http(port, "POST", "/mods/999999/vote", {"vote": 1}, token=tok["other"])
    check("175 POST /mods/{id}/vote unknown mod: 404 NOT_FOUND", error_code(s, b, "NOT_FOUND", 404), brief(s, b))

    # ---- 176 POST /mods/{id}/feature: developers (the creation feature rule, ftC)
    s, b = http(port, "POST", "/mods/%d/feature" % mod_id, {"featured": True}, token=tok["dev"])
    check("176 POST /mods/{id}/feature {featured:true} as a developer: data.featured true (%s, v10.js 27212 read at 107604)" % W,
          s == 200 and data_of(b) == {"mod_id": mod_id, "featured": True}, brief(s, b))
    feat = db_q(path, "SELECT mod_featured FROM published_mods WHERE mod_id = ?", (mod_id,))
    check("176 published_mods.mod_featured is set", feat and feat[0][0] == 1, str(feat))
    s, b = http(port, "POST", "/mods/%d/feature" % mod_id, {"featured": True}, token=tok["mod"])
    check("176 POST /mods/{id}/feature as a moderator (not a developer): 403 FORBIDDEN (not NOT_IMPLEMENTED)",
          refused(s, b, "FORBIDDEN"), brief(s, b))
    s, b = http(port, "POST", "/mods/999999/feature", {"featured": True}, token=tok["dev"])
    check("176 POST /mods/{id}/feature unknown mod: 404 NOT_FOUND", error_code(s, b, "NOT_FOUND", 404), brief(s, b))
    s, b = http(port, "GET", "/mods/by-name/TestMod")
    check("174 the featured mod reads back featured true", s == 200 and (data_of(b) or {}).get("featured") in (1, True), brief(s, b))

    # ---- 177 DELETE /mods/{id}: the creator's delete (creations.c delC: creator, developers, mod makers)
    s, b = http(port, "DELETE", "/mods/%d" % doomed_id, token=tok["other"])
    check("177 DELETE /mods/{id} by another player: 403 FORBIDDEN (not NOT_IMPLEMENTED) (%s, v10.js 27220 read at 107616)" % W,
          refused(s, b, "FORBIDDEN"), brief(s, b))
    s, b = http(port, "DELETE", "/mods/%d" % doomed_id)
    check("177 DELETE /mods/{id} without a session: 401 LOGIN_REQUIRED", error_code(s, b, "LOGIN_REQUIRED", 401), brief(s, b))
    s, b = http(port, "DELETE", "/mods/%d" % doomed_id, token=tok["plain"])
    check("177 DELETE /mods/{id} by the creator: data.deleted true", s == 200 and data_of(b) == {"mod_id": doomed_id, "deleted": True}, brief(s, b))
    gone = db_q(path, "SELECT deleted_at IS NOT NULL FROM published_mods WHERE mod_id = ?", (doomed_id,))
    check("177 published_mods.deleted_at is set", gone and gone[0][0] == 1, str(gone))
    s, b = http(port, "DELETE", "/mods/%d" % doomed_id, token=tok["plain"])
    check("177 DELETE /mods/{id} of a deleted mod: 404 NOT_FOUND", error_code(s, b, "NOT_FOUND", 404), brief(s, b))
    s, b = http(port, "GET", "/mods/by-name/DoomedMod")
    check("174 a deleted mod is not found by name", error_code(s, b, "NOT_FOUND", 404), brief(s, b))

    # ---- 178 POST /mods/publish: the multipart upload (no mod file storage here: a refusal)
    s, b = http(port, "POST", "/mods/publish", {}, token=tok["dev"])
    check("178 POST /mods/publish: 403 NOT_IMPLEMENTED refusal with a reason (%s, v10.js 27229)" % W,
          refused(s, b, "NOT_IMPLEMENTED"), brief(s, b))

    # ---- 190 GET /kpd/demos: the demos grouped by suspect (kpd_demos, db_kpd.c P_KPD_DEMOS)
    s, b = http(port, "GET", "/kpd/demos?off=0")
    d = data_of(b) or {}
    rows = d.get("rows") if isinstance(d, dict) else None
    check("190 GET /kpd/demos: data.rows of suspect groups and data.more (%s, v10.js 27329 read at 216006)" % W,
          s == 200 and isinstance(rows, list) and len(rows) == 4 and d.get("more") is False, brief(s, b))
    tg = next((g for g in rows or [] if isinstance(g, dict) and g.get("name") == WHO["target"][0]), None)
    check("190 a banned suspect's group: name shown, ban 1, gst 0, demos[] with tok, ct, typ, map, mod, dat, dur",
          tg is not None and tg.get("name") == WHO["target"][0] and tg.get("ban") == 1 and tg.get("gst") == 0
          and isinstance(tg.get("demos"), list) and len(tg["demos"]) == 2
          and all(k in tg["demos"][0] for k in ("tok", "ct", "typ", "map", "mod", "dat", "dur")), brief(s, b))
    ug = [g for g in rows or [] if isinstance(g, dict) and g.get("grp") == 7]
    check("190 an unbanned guest group: name hidden (''), ban 0, gst 1",
          len(ug) == 1 and ug[0].get("name") == "" and ug[0].get("ban") == 0 and ug[0].get("gst") == 1, brief(s, b))
    s, b = http(port, "GET", "/kpd/demos?q=" + WHO["target"][0])
    rows = (data_of(b) or {}).get("rows") or []
    check("190 GET /kpd/demos?q=<name>: that suspect's group only", s == 200 and len(rows) == 1
          and len(rows[0].get("demos", [])) == 2, brief(s, b))

    # ---- 191 GET /kpd/demos/{id}: an officer's view of a demo (officer = player_moderator)
    s, b = http(port, "GET", "/kpd/demos/tokA?ct=ctA", token=tok["mod"])
    d = data_of(b) or {}
    check("191 GET /kpd/demos/{id}?ct=: data.tok, ct, typ, ban, name, gst, map, mod, dat, dur for an officer (%s, v10.js 27335 read at 222638)" % W,
          s == 200 and d.get("tok") == "tokA" and d.get("ct") == "ctA" and d.get("typ") == 1 and d.get("ban") == 1
          and d.get("name") == WHO["target"][0] and d.get("gst") == 0 and d.get("map") == "Burg" and d.get("mod") == "Team"
          and isinstance(d.get("dat"), (int, float)) and d.get("dur") == 61000, brief(s, b))
    s, b = http(port, "GET", "/kpd/demos/tokC?ct=ctC", token=tok["plain"])
    check("191 GET /kpd/demos/{id} as a player who is not an officer: 404 NOT_FOUND ('not authorized')",
          error_code(s, b, "NOT_FOUND", 404), brief(s, b))
    s, b = http(port, "GET", "/kpd/demos/nosuch", token=tok["mod"])
    check("191 GET /kpd/demos/{id} unknown demo: 404 NOT_FOUND", error_code(s, b, "NOT_FOUND", 404), brief(s, b))
    s, b = http(port, "GET", "/kpd/demos/tokA?ct=wrong", token=tok["mod"])
    check("191 GET /kpd/demos/{id} with another call token: 404 NOT_FOUND", error_code(s, b, "NOT_FOUND", 404), brief(s, b))
    s, b = http(port, "GET", "/kpd/demos/tokA?ct=ctA")
    check("191 GET /kpd/demos/{id} without a session: 401 LOGIN_REQUIRED", error_code(s, b, "LOGIN_REQUIRED", 401), brief(s, b))

    # ---- 192 POST /kpd/demos/{id}/vote: one verdict per officer (kpd_demo_votes), the counts follow
    s, b = http(port, "POST", "/kpd/demos/tokC/vote", {"ct": "ctC", "verdict": 1}, token=tok["mod"])
    check("192 POST /kpd/demos/{id}/vote {ct, verdict:1}: data.yes, no, myv, banned, resolved (%s, v10.js 27345 read at 222454)" % W,
          s == 200 and data_of(b) == {"yes": 1, "no": 0, "myv": 1, "banned": False, "resolved": False}, brief(s, b))
    s, b = http(port, "POST", "/kpd/demos/tokC/vote", {"ct": "ctC", "verdict": 0}, token=tok["dev"])
    check("192 a second officer's verdict counts: yes 1, no 1, myv 0 for that officer",
          s == 200 and data_of(b) == {"yes": 1, "no": 1, "myv": 0, "banned": False, "resolved": False}, brief(s, b))
    s, b = http(port, "POST", "/kpd/demos/tokC/vote", {"ct": "ctC", "verdict": 0}, token=tok["mod"])
    check("192 an officer changing a verdict replaces it: yes 0, no 2",
          s == 200 and data_of(b) == {"yes": 0, "no": 2, "myv": 0, "banned": False, "resolved": False}, brief(s, b))
    row = db_q(path, "SELECT yes_votes, no_votes FROM kpd_demos WHERE token = 'tokC'")
    check("192 kpd_demos counts match the verdicts (0 yes, 2 no)", row and tuple(row[0]) == (0, 2), str(row))
    s, b = http(port, "POST", "/kpd/demos/tokC/vote", {"ct": "ctC", "verdict": 1}, token=tok["plain"])
    check("192 a player who is not an officer: 404 NOT_FOUND ('not authorized')", error_code(s, b, "NOT_FOUND", 404), brief(s, b))
    s, b = http(port, "POST", "/kpd/demos/tokC/vote", {"ct": "ctC", "verdict": 2}, token=tok["mod"])
    check("192 a verdict other than 0 or 1: 400", error_code(s, b, "BAD_REQUEST", 400), brief(s, b))
    s, b = http(port, "POST", "/kpd/demos/tokE/vote", {"ct": "ctE", "verdict": 1}, token=tok["mod"])
    check("192 a closed case takes no votes: 400", error_code(s, b, "VOTE_CLOSED", 400), brief(s, b))
    s, b = http(port, "POST", "/kpd/demos/tokC/vote", {"ct": "ctC", "verdict": 1})
    check("192 a vote without a session: 401 LOGIN_REQUIRED", error_code(s, b, "LOGIN_REQUIRED", 401), brief(s, b))

    s, b = http(port, "GET", "/kpd/demos?off=0", token=tok["mod"])
    mine = [dm for g in (data_of(b) or {}).get("rows", []) for dm in g.get("demos", []) if dm.get("tok") == "tokC"]
    check("190 GET /kpd/demos as an officer: each demo carries the caller's own verdict (myv)",
          s == 200 and len(mine) == 1 and mine[0].get("myv") == 0, brief(s, b))

    # ---- 193 GET /kpd/reviewers: the officers who handled the most calls (kpd_calls status 1)
    s, b = http(port, "GET", "/kpd/reviewers")
    d = data_of(b) or {}
    all_rows = d.get("all") if isinstance(d, dict) else None
    month = d.get("month") if isinstance(d, dict) else None
    check("193 GET /kpd/reviewers: data.month and data.all, rows with name and cnt (%s, v10.js 27354 read at 216013)" % W,
          s == 200 and isinstance(month, list) and isinstance(all_rows, list), brief(s, b))
    check("193 all-time reviewers, most calls first: %s 2, %s 1" % (WHO["mod"][0], WHO["dev"][0]),
          isinstance(all_rows, list) and len(all_rows) >= 2 and all_rows[0].get("name") == WHO["mod"][0]
          and all_rows[0].get("cnt") == 2 and all_rows[1].get("name") == WHO["dev"][0] and all_rows[1].get("cnt") == 1,
          brief(s, b))
    check("193 the last-30-days list has the same officers", isinstance(month, list) and len(month) == 2
          and month[0].get("cnt") == 2, brief(s, b))

    # ---- 190 paging: a page is 30 suspect groups, the rest follow (the Prev / Next steps)
    for i in range(31):
        db_x(path, "INSERT INTO kpd_demos (token, ct, suspect_name, suspect_group, typ, map, mode_name, duration_ms)"
                   " VALUES (?, 'pgct', 'guest%d', ?, 0, 'Burg', 'FFA', 1000)" % i, ("pg%d" % i, 100 + i))
    s, b = http(port, "GET", "/kpd/demos?off=0")
    d = data_of(b) or {}
    check("190 a full page is 30 suspect groups with more true (the Next step)",
          s == 200 and len(d.get("rows") or []) == 30 and d.get("more") is True, brief(s, b)[:120])
    s, b = http(port, "GET", "/kpd/demos?off=30")
    d = data_of(b) or {}
    check("190 the next page has the rest (5 groups) and more false",
          s == 200 and len(d.get("rows") or []) == 5 and d.get("more") is False, brief(s, b)[:120])


def stop(proc):
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except OSError:
            proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    if not os.access(BRAIN, os.X_OK):
        print("v10_api_mods_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("v10_api_mods_test: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    work = tempfile.mkdtemp(prefix="kn-v10-mods-test.")
    log = os.path.join(work, "brain.log")
    procs = []
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=v10-mods-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        try:
            scenario(int(m.group(1)), db)
        except Exception as e:  # noqa: BLE001 - a crashed probe is a failed run, not a crash of the test
            check("the probes complete", False, repr(e)[:200])
    finally:
        for p in procs:
            stop(p)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    ok = bool(RESULTS) and all(RESULTS)
    print("v10_api_mods_test: %d PASS, %d FAIL" % (sum(RESULTS), len(RESULTS) - sum(RESULTS)))
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("logs kept in " + work)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
