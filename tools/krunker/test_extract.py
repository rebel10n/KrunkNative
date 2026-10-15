#!/usr/bin/env python3
"""Regression test of tools/krunker/extract_build.py.

1. Cross-build check (the main one): every Krunker bundle found locally must extract, with the
   right AHKEY / versions / salt for its build, and the one-line obfuscated variants
   (game.debug.js, game.min.js = game.manifest.json's "source", the manifest itself) must give
   exactly the values of the readable game.deob.js. Bundles that are not on this machine (CI)
   are skipped; KRUNKER_BUNDLES_REQUIRED=1 makes a missing one a failure.
2. Synthetic bundles (always run, no Krunker files): a readable fixture, the same fixture
   string-array obfuscated into one line, and one fixture per anchor with that anchor broken,
   which must fail naming the anchor and write nothing.
3. The committed shared/include/krunker_build.h matches tools/krunker/build.json and compiles
   as C11 with -Wall -Wextra -Werror (when a C compiler is installed); the current bundle (the
   one build.json names by sha256: the URL cache or a local copy) regenerates it identically
   and yields the known per-player layouts (ADD_PLAYERS 53/player, UPDATE_PLAYERS 13, ...).

usage: python3 -I tools/krunker/test_extract.py [-v]
"""

import hashlib
import io
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract_build as E  # noqa: E402

ROOT = E.ROOT
HOME = os.path.expanduser("~")
OFFICIAL = os.path.join(HOME, ".cache", "kn-official")
V10 = os.path.join(HOME, ".cache", "kn-v10", "v10.js")

# The live build (2026-10-14, sha256 e21ede6a... of game.deob.js) and the older v10.js copy.
LIVE = {"ahkey": 0x17153, "client_version": "10.0.2r1", "build_version": "Def6phpLKsJTIw2XpAeEicZWURGypSce",
        "salt": "KRUNK3R_AC", "c2s": 138, "s2c": 262}
OLD_V10 = {"ahkey": 1597, "client_version": "10.0.1r1", "build_version": "FArgZqd2pNs7O8lWk4TblHSdDWfMUfD2",
           "salt": "KRUNK3R_AC", "c2s": 138, "s2c": 262}

# Per-player layouts and argument positions (krunker_link.c relies on them). The same in the
# 10.0.1 (v10.js), 10.0.2 (game.deob.js) and 10.0.3 (live, 2026-10-14) builds. attachIndex is
# [22], not the [23] packet-audit.md section 1 lists: players.add takes a[i + 0x16] as its
# attachIndex parameter and the server's getPlayerAddData packs attachIndex at [22], pcStatIndex
# at [23].
KNOWN_ADD = {"id": 0, "sid": 1, "x": 2, "y": 3, "z": 4, "name": 5, "classIndex": 6, "health": 7, "maxHealth": 8,
             "team": 9, "level": 10, "clan": 11, "secIndex": 15, "xDire": 18, "attachIndex": 22, "pcStatIndex": 23,
             "alias": 28, "busy": 45}
KNOWN_K = {"sid": 0, "x": 1, "y": 2, "z": 3, "xDire": 4, "yDire": 5, "stepVal": 6, "onGround": 7, "crouchVal": 8,
           "weapon": 9, "aimVal": 10, "grapple": 11, "ping": 12}
KNOWN_SYNC = {n: i for i, n in enumerate(E.SYNC_REQUIRED)}  # inputSN 0 .. canSlideT 0x18, _protected 0x19
KNOWN_ARGS = {
    "kill_player": {"victim": 0, "deaths": 1, "killer": 2, "stats": 3, "info": 4, "hideFeed": 5},
    "do_damage": {"target": 0, "damage": 1, "crit": 2, "kill": 3, "ai": 4, "silent": 5},
    "change_health": {"health": 0, "sid": 1, "srcX": 2, "srcZ": 3, "noHitMarker": 4, "resetMax": 5},
    "get_score": {"delta": 0, "relay": 1, "absolute": 2, "classIndex": 3},
}
KNOWN_LAYOUTS = {"add_stride": 53, "add": KNOWN_ADD, "k_stride": 13, "k": KNOWN_K, "send_vals": 0x1a,
                 "sync": KNOWN_SYNC, "args": KNOWN_ARGS}

VERBOSE = "-v" in sys.argv
failures = []
passed = [0]


def check(cond, what):
    if cond:
        passed[0] += 1
        if VERBOSE:
            print("  ok   " + what)
    else:
        failures.append(what)
        print("  FAIL " + what)


def run_extract(text):
    try:
        return E.extract(text), None
    except E.ExtractError as e:
        return None, str(e)


def check_values(label, doc, exp):
    check(doc["ahkey"] == exp["ahkey"], "%s: AHKEY %s == %s" % (label, doc["ahkey_hex"], hex(exp["ahkey"])))
    check(doc["ahnum_step"] == exp["ahkey"] & 0xFF, "%s: AHNUM step" % label)
    check(doc["client_version"] == exp["client_version"], "%s: clientVersion %s" % (label, doc["client_version"]))
    check(doc["build_version"] == exp["build_version"], "%s: buildVersion %s" % (label, doc["build_version"]))
    check(doc["cheat"]["salt"] == exp["salt"], "%s: cheat salt %r" % (label, doc["cheat"]["salt"]))
    check(len(doc["packets_c2s"]) == exp["c2s"], "%s: %d c2s packets" % (label, len(doc["packets_c2s"])))
    check(len(doc["packets_s2c"]) == exp["s2c"], "%s: %d s2c packets" % (label, len(doc["packets_s2c"])))
    c2s = {e["name"]: e["wire"] for e in doc["packets_c2s"]}
    s2c = {e["name"]: e["wire"] for e in doc["packets_s2c"]}
    if "en" in exp.get("anchors", ("en",)):
        check(c2s.get("ENTER_GAME") == "en" and c2s.get("CHEAT_CHECK_RESPONSE") == "ccr" and c2s.get("cptR") == "cptR",
              "%s: c2s anchors" % label)
        check(s2c.get("IO_INIT") == "io-init" and s2c.get("CHEAT_CHECK") == "cc", "%s: s2c anchors" % label)
    check(doc["cheat"]["fnv_multiply"] == "double", "%s: FNV step in doubles" % label)


def check_layouts(label, doc, exp):
    lay = doc["layouts"]
    offs = lambda k: {f["name"]: f["offset"] for f in lay[k]["fields"]}  # noqa: E731
    add, k, sync = offs("add_players"), offs("update_players"), offs("sync_player")
    check(lay["add_players"]["stride"] == exp["add_stride"], "%s: ADD_PLAYERS stride %s" % (label, lay["add_players"]["stride"]))
    check(lay["add_players"]["server_stride"] in (None, exp["add_stride"]), "%s: getPlayerAddData packs the stride" % label)
    bad = {n: add.get(n) for n, o in exp["add"].items() if add.get(n) != o}
    check(not bad, "%s: ADD_PLAYERS offsets (wrong: %r)" % (label, bad))
    check(lay["update_players"]["stride"] == exp["k_stride"], "%s: UPDATE_PLAYERS stride %s" % (label, lay["update_players"]["stride"]))
    check(k == exp["k"], "%s: UPDATE_PLAYERS offsets %r" % (label, k))
    check(lay["sync_player"]["send_vals"] == exp["send_vals"], "%s: SYNC_PLAYER sendVals at %s" % (label, lay["sync_player"]["send_vals"]))
    check(sync == exp["sync"], "%s: SYNC_PLAYER layout (wrong: %r)" % (label, {n: o for n, o in sync.items() if exp["sync"].get(n) != o}))
    for key_, args in exp["args"].items():
        check(lay[key_]["args"] == args and lay[key_]["argc"] == len(args), "%s: %s args %r" % (label, key_, lay[key_]))


def current_bundle():
    """A local copy of the bundle build.json was generated from (the URL cache first), or None."""
    with open(E.OUT_JSON, encoding="utf8") as f:
        src = json.load(f)["source"]
    want, url = src["sha256"], src.get("url") or E.DEFAULT_URL
    cands = [os.path.join(E.cache_dir(), hashlib.sha256(url.encode()).hexdigest()[:16] + ".js")]
    if os.path.isdir(OFFICIAL):
        cands += [os.path.join(OFFICIAL, n) for n in sorted(os.listdir(OFFICIAL)) if n.endswith(".js")]
    for c in cands:
        if os.path.isfile(c):
            with open(c, "rb") as f:
                if hashlib.sha256(f.read()).hexdigest() == want:
                    return c
    return None


def comparable(doc):
    """The values that must not depend on the bundle's form (deob vs debug vs min)."""
    d = dict(doc)
    for k in ("deobfuscation", "cheat_keystream_js", "cheat_salt_js", "cheat_keystream_standalone_js",
              "cheat_handler_js"):
        d.pop(k, None)
    return d


# ------------------------------------------------------------------ 1. real bundles

def check_maps(label, doc):
    """The official map table anchors (tools/krunker/maps.py) of a 10.0.2+ bundle: the API, and the raid
    preset naming map 43 Bastion."""
    mp = doc.get("maps")
    check(mp is not None, "%s: map anchors extract" % label)
    if not mp:
        return
    check(mp["api_url"] == "https://gapi.svc.krunker.io" and mp["list_path"] == "/maps" and mp["map_path"] == "/maps/",
          "%s: maps API gapi /maps, /maps/<i>" % label)
    raid = [p for p in mp["presets"] if 43 in p["indices"]]
    check(raid and raid[0]["names"][raid[0]["indices"].index(43)] == "bastion", "%s: preset map 43 is bastion" % label)
    check((mp.get("ftue") or {}).get("fileName") == "bot_ftue_v2", "%s: FTUE map file" % label)


def test_real_bundles(tmp):
    required = os.environ.get("KRUNKER_BUNDLES_REQUIRED") == "1"
    deob_path = os.path.join(OFFICIAL, "game.deob.js")
    variants = [("game.deob.js", deob_path), ("game.debug.js", os.path.join(OFFICIAL, "game.debug.js")),
                ("game.manifest.json", os.path.join(OFFICIAL, "game.manifest.json"))]
    man = variants[2][1]
    if os.path.exists(man):
        # game.min.js as served = the manifest's "source" (game.debug.js minus its 106-byte prelude)
        with open(man, encoding="utf8") as f:
            src = json.load(f)["source"]
        minp = os.path.join(tmp, "game.min.js")
        with open(minp, "w", encoding="utf8") as f:
            f.write(src)
        variants.append(("game.min.js", minp))
    ref = None
    for label, path in variants:
        if not os.path.exists(path):
            print("  SKIP %s (not on this machine)" % label)
            check(not required, "%s present" % label)
            continue
        text, prov = E.load_source(path)
        doc, err = run_extract(text)
        check(err is None, "%s extracts (%s)" % (label, err))
        if not doc:
            continue
        check_values(label, doc, LIVE)
        check_layouts(label, doc, KNOWN_LAYOUTS)
        check_maps(label, doc)
        if label == "game.deob.js":
            ref = doc
            check(doc["deobfuscation"] is None, "game.deob.js needs no string-array decoding")
        else:
            check(doc["deobfuscation"] is not None, "%s was string-array decoded" % label)
            if ref:
                same = comparable(doc) == comparable(ref)
                check(same, "%s: every value identical to game.deob.js" % label)
                check(E.canonical_js(doc["cheat_keystream_js"], {}) == E.canonical_js(ref["cheat_keystream_js"], {}),
                      "%s: same keystream function" % label)
        print("  %-19s %s" % (label, E.summary(E.build_doc(doc, prov)).split("\n")[0]))
    if os.path.exists(V10):
        text, prov = E.load_source(V10)
        doc, err = run_extract(text)
        check(err is None, "v10.js extracts (%s)" % err)
        if doc:
            check_values("v10.js", doc, OLD_V10)
            check_layouts("v10.js", doc, KNOWN_LAYOUTS)
            check_maps("v10.js", doc)
            print("  %-19s %s" % ("v10.js", E.summary(E.build_doc(doc, prov)).split("\n")[0]))
    else:
        print("  SKIP v10.js (not on this machine)")
        check(not required, "v10.js present")
    cur = current_bundle()
    if cur:
        text, prov = E.load_source(cur)
        doc, err = run_extract(text)
        check(err is None, "current bundle %s extracts (%s)" % (os.path.basename(cur), err))
        if doc:
            check_layouts("current bundle", doc, KNOWN_LAYOUTS)
            check_maps("current bundle", doc)
            mm = E.maps_module()
            check(mm.consistency(doc["maps"] or {}, mm.read_lock()) == [],
                  "current bundle: map anchors agree with maps.lock.json %r" % mm.consistency(doc["maps"] or {}, mm.read_lock()))
            print("  %-19s %s" % ("current bundle", E.summary(E.build_doc(doc, prov)).split("\n")[1].strip()))
    else:
        print("  SKIP current bundle (build.json's sha256 is not on this machine)")
        check(not required, "current bundle present")


# --------------------------------------------------------------- 2. synthetic bundles

def fixture_tables():
    c2s = [("PING", "po"), ("ENTER_GAME", "en"), ("INPUT", "q"), ("CHAT_MESSAGE", "ct"), ("cptR", "cptR"),
           ("CHEAT_CHECK_RESPONSE", "ccr")] + [("C2S_X%d" % i, "x%d" % i) for i in range(20)]
    s2c = [("RPC_SUCCESS", "_0"), ("cpt", "cpt"), ("cptR", "cptR"), ("IO_INIT", "io-init"), ("ADD_PLAYERS", "0"),
           ("CHEAT_CHECK", "cc"), ("ENTER_GAME", "frs"), ("ERROR", "error"), ("UPDATE_PLAYERS", "k"),
           ("SYNC_PLAYER", "l"), ("KILL_PLAYER", "3"), ("DO_DAMAGE", "4"), ("CHANGE_HEALTH", "h"), ("GET_SCORE", "5")] + [("S2C_Y%d" % i, "y%d" % i) for i in range(20)]
    return c2s, s2c


def table_js(var, entries):
    return "  var %s = {\n%s\n  };\n" % (var, ",\n".join("    '%s': '%s'" % kv for kv in entries))


# A readable bundle with the same structures as Krunker's (written for this test, not copied):
# the identifiers are arbitrary on purpose; only strings and numbers are anchors.
FIXTURE = r"""!function () {
  'use strict';
  function qq1(a1, a2) {
    return a1 + a2 & 0xff;
  }
  function qq2(a3) {
    return [a3 >> 0x4 & 0xf, 0xf & a3];
  }
  var vv1 = '9.8.7';
  var vv2 = vv1 + 'r3';
  var vv3 = process["env"]['CUSTOM_VERSION'];
  zz1 = process['env']["MATCHMAKER_URI"] || "https://mm.example.test", zz2 = process["env"]["API_URI"] || "https://api.example.test", zz3 = process["env"]["GAPI_URI"] || "https://gapi.example.test";
  var mapi = {
    'getMaps': function () {
      return rq1(this, '/maps', {
        'method': "GET"
      });
    },
    'getMap': function (m1) {
      return rq1(this, "/maps/"["concat"](m1), {
        'method': "GET",
        'cache': "no-cache"
      });
    }
  };
  var mlist = [];
  api1["maps"]["getMaps"]()["then"](function (r1) {
    r1["data"]["forEach"](function (r2) {
      mlist["push"](r2);
    });
  });
  function onInit1(i1, i2, i3, i4, i5, i6, i7, i8, i9, c9) {
    mlist[i1]["didLoad"] || c9 && c9["data"] ? go1() : api1['maps']["getMap"](i1)["then"](function (r3) {
      mlist[i1] = r3["data"];
    });
  }
  var hosts1 = [{
    'title': "Raid Server Setup",
    'allMaps': ['tortuga', "bastion"],
    'overDat': {
      'maps': [0x12],
      'modes': [0x17]
    },
    'all': {
      'maps': [0x12, 0x2b]
    }
  }];
  var vv4 = zz9 ? "0x4AAAAAAAtestSiteKey000" : "1x00000000000000000000BB";
  zz9 ? (vv5 = ['de-fra', "us-nj"], zz8 || vv5['push']("sss")) : vv5 = ["local"];
  var vv6 = ['us-wa'];
  var cfg = {
    'buildVersion': "AbCdEfGhIjKlMnOpQrStUvWxYz012345",
    'clientRevision': '7',
    'clientVersion': vv2,
    'customBuildVersion': vv3,
    'dedicatedOnlyRegions': vv6,
    'turnstileSiteKey': vv4
  };
@@C2S@@@@S2C@@  var sock = {
    'ahNum': 0x0,
    'send': function (p1) {
      if (this["socket"] && this["socket"]["readyState"] === WebSocket['OPEN']) {
        p1 != tc2s['cptR'] && (this["ahNum"] = qq1(this["ahNum"], @@AHKEY@@));
        var p2 = qq9(p1, this["ahNum"]);
        this["socket"]["send"](p2);
      }
    }
  };
  var salt1 = [@@SALT@@];
  function ks1(n1, n2) {
    for (var k1 = salt1, k2 = new Uint8Array(n2), k3 = 0x0; k3 < n2; k3++) {
      k2[k3] = (n1['charCodeAt'](k3 % n1['length']) ^ k1[k3 % k1["length"]]) + k3 & 0xff;
    }
    return k2;
  }
  function cc1(h1, h2, h3) {
    try {
      var h4;
      var h5 = function (b1, b2) {
        var b3;
        b3 = "undefined" != typeof Buffer ? new Uint8Array(Buffer['from'](b1, "base64")) : new Uint8Array(atob(b1)["split"]('')["map"](function (b4) {
          return b4["charCodeAt"](0x0);
        }));
        for (var b5 = ks1(b2, b3["length"]), b6 = new Uint8Array(b3['length']), b7 = 0x0; b7 < b3["length"]; b7++) {
          b6[b7] = b3[b7] ^ b5[b7];
        }
        var b8 = new TextDecoder()["decode"](b6);
        return JSON["parse"](b8);
      }(h3, h1);
      try {
        h4 = (0x0, eval)(h5);
      } catch (e1) {
        h4 = {
          'r': null,
          'e': {
            '_eval': e1["message"] || "eval_error"
          }
        };
      }
      var h6 = function (c1, c2) {
        for (var c3 = JSON["stringify"](c1), c4 = new TextEncoder()["encode"](c3), c5 = ks1(c2, c4["length"]), c6 = new Uint8Array(c4["length"]), c7 = 0x0; c7 < c4["length"]; c7++) {
          c6[c7] = c4[c7] ^ c5[c7];
        }
        return "undefined" != typeof Buffer ? Buffer["from"](c6)["toString"]("base64") : btoa(String["fromCharCode"]["apply"](String, qq8(c6)));
      }(h4, h1);
      var h7 = function (d1, d2) {
        for (var d3 = d2 + JSON["stringify"](d1), d4 = 0x811c9dc5, d5 = 0x0; d5 < d3['length']; d5++) {
          d4 = 0x1000193 * (d4 ^= d3["charCodeAt"](d5)) | 0x0;
        }
        return (d4 >>> 0x0)["toString"](0x10)['padStart'](0x8, '0');
      }(h4, h1);
      sock["send"](tc2s["CHEAT_CHECK_RESPONSE"], h1, h6, h7);
    } catch (e2) {}
  }
  var Plr = function (c0, c1) {
    this["id"] = c0;
    this["init"] = function (n0, n1, n2, n3) {
      this["z"] = n0;
      this["x"] = n1;
      this["y"] = n2;
      this["name"] = n3 || "unknown";
    };
    this["setClass"] = function (s0, s1, s2) {
      this["classIndex"] = s1;
    };
  };
  function PlayerMgr() {
    this["add"] = function (a0, a1, a2, a3, a4, a5, a6, a7, a8) {
      for (var f1, f2 = 0x0; f2 < this["list"]["length"]; ++f2) {
        if (this["list"][f2]["id"] == a0) {
          f1 = this["list"][f2];
          break;
        }
      }
      f1 || ((f1 = new Plr(a0, this))["sid"] = a1 || ++this["currentSid"], this["list"]["push"](f1));
      a5 || (a5 = "Guest_" + f1["sid"]);
      f1["init"](a4, a2, a3, a5);
      f1["attachIndex"] = a8;
      f1["secIndex"] = a7;
      f1["setClass"](gm, a6, a7);
      return f1;
    };
  }
  function hAdd(l1, l2) {
    for (var i1 = 0x0; i1 < l1["length"];) {
      var me = l1[i1] == conn["socketId"];
      (pl = gm["players"]["add"](l1[i1], l1[i1 + 0x1], l1[i1 + 0x2], l1[i1 + 0x3], l1[i1 + 0x4], l1[i1 + 0x5], l1[i1 + 0x6], l1[i1 + 0xb], l1[i1 + 0xc]))["health"] = null == l1[i1 + 0x7] ? pl["maxHealth"] : l1[i1 + 0x7];
      pl["maxHealth"] = l1[i1 + 0x8];
      pl["team"] = l1[i1 + 0x9];
      pl["xDire"] = l1[i1 + 0xa];
      pl["flagIndex"] = parseInt(l1[i1 + 0xd]);
      gm["setBusy"](pl, l1[i1 + 0xe]);
      i1 += 0x10;
    }
  }
  function hK(k1, k2, k3) {
    if (k1) {
      for (var j1 = 0x0; j1 < k1["length"];) {
        if ((pk = gm["players"]["findBySid"](k1[j1])) && pk != you) {
          pk["x"] = k1[j1 + 0x1];
          pk["y"] = k1[j1 + 0x2];
          pk["z"] = k1[j1 + 0x3];
          pk["xDire"] = rad(k1[j1 + 0x4]);
          pk["yDire"] = rad(k1[j1 + 0x5]);
          pk["onGround"] = k1[j1 + 0x6];
          pk["crouchVal"] = k1[j1 + 0x7];
          pk["aimVal"] = k1[j1 + 0x9];
          pk["ping"] = k1[j1 + 0xb];
          var gr = k1[j1 + 0xa];
          gr ? pk["grapple"] = {"x": gr[0x0]} : pk["grapple"] = null;
          gm["players"]["swapWeapon"](pk, 0x0, !0x1, k1[j1 + 0x8]);
        }
        j1 += 0xc;
      }
    }
  }
  function hL(s1) {
    conn["send"](tc2s["PONG"]);
    if (you && s1) {
      you["x"] = s1[0x2];
      you["y"] = s1[0x3];
      you["z"] = s1[0x4];
      you["onGround"] = s1[0x9];
      you["didJump"] = s1[0xa];
      you["didJumpW"] = s1[0xb];
      you["wallJumpC"] = s1[0xc];
      you["onWall"] = s1[0xd];
      you["onLadder"] = s1[0xe];
      you["aimVal"] = s1[0xf];
      you["crouchVal"] = s1[0x10];
      you["velocity"]["set"](s1[0x6], s1[0x5], s1[0x7]);
      you["xVelC"] = s1[0x15];
      you["zVelC"] = s1[0x16];
      you["xDire"] = s1[0x8];
      gm["players"]["swapWeapon"](you, 0x0, !0x1, s1[0x11], void 0x0, !0x0);
      you["slideTimer"] = s1[0x12];
      you["canSlide"] = s1[0x13];
      you["onTerrain"] = s1[0x14];
      you["grapple"] = s1[0x17] || null;
      you["canSlideT"] = s1[0x18];
      var pr = s1[0x19];
      if (pr && you["_protected"]) {
        you["_protected"][pr[0x0]] = pr[0x1];
      }
      var ms = gm["movement"][0x0];
      for (var m1 = 0x0; m1 < ms["sendVals"]["length"]; ++m1) {
        you[ms["sendVals"][m1]] = s1[0x1a + m1];
      }
      for (var t1 = 0x0; t1 < cam["tmpInpts"]["length"];) {
        cam["tmpInpts"][t1][0x0] <= s1[0x0] ? cam["tmpInpts"]["splice"](t1, 0x1) : t1++;
      }
    }
    if (you && s1 && s1[0x1]) {
      pg = s1[0x1];
      pingText["textContent"] = pg;
    }
  }
  function hKill(v1, v2, v3, v4, v5, v6) {
    vp = gm["players"]["findBySid"](v1);
    kp = gm["players"]["findBySid"](v3);
    if (vp) {
      kp && vp != kp && gm["players"]["countSkinKill"](kp, v5);
      gm["players"]["kill"](vp);
      vp["isYou"] && (deathsVal["textContent"] = v2 || 0x0, function (w1, w2, w3) {
        w3 = w3 || [];
      }(vp, kp, v4, !0x1));
      if (!v6 && opts["tmp"]["showKills"]) {
        feed(kp, v5);
      }
    }
  }
  function hDmg(d1, d2, d3, d4, d5, d6) {
    you && (d5 ? d4 && stat(you, "aikills") : d2 > 0x0 && stat(you, "hits"));
    d6 || snd["play"](d3 ? "crit_0" : "hit_0", 2.1);
    d2 = Math["ceil"](d2);
    d2 && null != d1 && (d5 ? tg = gm["AI"]["findBySid"](d1) : tg = gm["players"]["findBySid"](d1));
  }
  function hHp(e1, e2, e3, e4, e5, e6) {
    if (you && (hp = null == e2 ? you : gm["players"]["findBySid"](e2))) {
      hp["health"] = e1;
      (hp["health"] > hp["maxHealth"] || e6) && (hp["maxHealth"] = hp["health"]);
      e1 < lastHp && !e5 && (null != e3 && null != e3 && function (u1, u2) {
        hits["x"] = u1;
        hits["z"] = u2;
      }(e3, e4));
    }
  }
  function hScore(g1, g2, g3, g4) {
    g1 = g1 || 0x0;
    if (!g2 && you && (you["score"] = g1 + (g3 ? 0x0 : you["score"]), null != g4)) {
      you["classScores"][g4 || 0x0] += g1;
    }
  }
  function df(o1, o2, o3) {
    o1[o2] = o3;
    return o1;
  }
  var hmap = df(df(df(df(df(df(df({}, ts2c["ADD_PLAYERS"], hAdd), ts2c["UPDATE_PLAYERS"], hK), ts2c["SYNC_PLAYER"], hL), ts2c["KILL_PLAYER"], hKill), ts2c["DO_DAMAGE"], hDmg), ts2c["CHANGE_HEALTH"], hHp), ts2c["GET_SCORE"], hScore);
}();
"""

FIX_EXP = {"ahkey": 0x2A5F1, "client_version": "9.8.7r3", "build_version": "AbCdEfGhIjKlMnOpQrStUvWxYz012345",
           "salt": "S4LT-x", "c2s": 26, "s2c": 34}
# The fixture's handlers use their own strides / orders, so nothing can pass by being hard-coded.
FIX_LAYOUTS = {
    "add_stride": 16, "k_stride": 12, "send_vals": 0x1a, "sync": KNOWN_SYNC, "args": KNOWN_ARGS,
    "add": {"id": 0, "sid": 1, "x": 2, "y": 3, "z": 4, "name": 5, "classIndex": 6, "health": 7, "maxHealth": 8,
            "team": 9, "xDire": 10, "secIndex": 11, "attachIndex": 12, "flagIndex": 13, "busy": 14},
    "k": {"sid": 0, "x": 1, "y": 2, "z": 3, "xDire": 4, "yDire": 5, "onGround": 6, "crouchVal": 7, "weapon": 8,
          "aimVal": 9, "grapple": 10, "ping": 11},
}


def make_fixture(ahkey="0x2a5f1", salt="S4LT-x"):
    c2s, s2c = fixture_tables()
    return (FIXTURE.replace("@@C2S@@", table_js("tc2s", c2s)).replace("@@S2C@@", table_js("ts2c", s2c))
            .replace("@@AHKEY@@", ahkey).replace("@@SALT@@", ", ".join("0x%x" % ord(c) for c in salt)))


def obfuscate(src, seed=7):
    """One-line string-array form of `src` like Krunker's game.min.js: the string literals that
    are property names / table values move into a rotated array behind a decoder."""
    rnd = random.Random(seed)
    lit = re.compile(E.STR)
    strings = []
    index = {}

    def take(m):
        s = E.js_unquote(m.group(0))
        # keep object keys literal (Krunker's obfuscator does too); everything else goes in the array
        after = src[m.end():m.end() + 3].lstrip()
        if after.startswith(":") or rnd.random() < 0.15:
            return m.group(0)
        if s not in index:
            index[s] = len(strings)
            strings.append(s)
        return "@@D%d@@" % index[s]
    body = lit.sub(take, src)
    offset = 0x1ac
    # checksum strings that parseInt to known numbers
    b, c, d = 2 * rnd.randint(10, 999), 3 * rnd.randint(10, 99), 4 * rnd.randint(10, 9999)
    a = (b // 2) * (c // 3) + rnd.randint(1, 99999)  # keeps the checksum a positive integer
    nums = ["%d%s" % (v, rnd.choice(["abc", "XyZ", "q"])) for v in (a, b, c, d)]
    for n in nums:
        index[n] = len(strings)
        strings.append(n)
    final = list(strings)
    k = rnd.randint(1, len(final) - 1)
    stored = final[-k:] + final[:-k]  # k push(shift) rotations restore `final`
    ids = [offset + index[n] for n in nums]
    expr = ("parseInt(sA(0x%x))/0x1+-parseInt(sA(0x%x))/0x2*(parseInt(sA(0x%x))/0x3)+parseInt(sA(0x%x))/0x4"
            % tuple(ids))
    target = E._Checksum(expr).eval(lambda i: final[i - offset])
    assert target == int(target) and target > 0
    body = re.sub(r"@@D(\d+)@@", lambda m: "dA(0x%x)" % (offset + int(m.group(1))), body)
    body = re.sub(r"\s*\n\s*", "", body)
    head = ("function dA(x1,x2){var x3=gA();return dA=function(x4,x5){x4=x4-0x%x;var x6=x3[x4];return x6;},dA(x1,x2);}"
            "(function(y1,y2){var sA=dA,y3=y1();while(!![]){try{var y4=%s;if(y4===y2)break;else y3['push'](y3['shift']());}"
            "catch(y5){y3['push'](y3['shift']());}}}(gA,0x%x),!(function(){var sB=dA;})());"
            "function gA(){var z1=[%s];gA=function(){return z1;};return gA();}"
            % (offset, expr, int(target), ",".join(E.js_quote(s) for s in stored)))
    # a decoder alias inside a function, like the real thing: function f(){var a=dA; ... a(0x1b0) ...}
    m = re.search(r"function ks1\(n1, n2\) \{.*?return k2;\}", body)
    body = body[:m.start()] + m.group(0).replace("dA(", "al1(").replace("{", "{var al1=dA;", 1) + body[m.end():]
    return head + body


def test_synthetic(tmp):
    fx = make_fixture()
    doc, err = run_extract(fx)
    check(err is None, "fixture extracts (%s)" % err)
    if doc:
        check_values("fixture", doc, dict(FIX_EXP, anchors=("en",)))
        ex = doc["extras"]
        check(ex["matchmaker_url"] == "https://mm.example.test", "fixture: matchmaker URL")
        check(ex["turnstile_site_key"] == "0x4AAAAAAAtestSiteKey000", "fixture: turnstile key")
        check(ex["regions"] == {"regions": ["de-fra", "us-nj"], "conditional": ["sss"]}, "fixture: regions")
        check(ex["dedicated_only_regions"] == ["us-wa"], "fixture: dedicated regions")
        check(ex["client_revision"] == "7", "fixture: clientRevision")
        check(ex["custom_build_version"] == {"value": None, "env": "CUSTOM_VERSION"}, "fixture: customBuildVersion")
        check(doc["warnings"] == [], "fixture: no warnings")
        mp = doc.get("maps") or {}
        check(mp.get("api_url") == "https://gapi.example.test" and mp.get("list_path") == "/maps" and
              mp.get("map_path") == "/maps/", "fixture: maps API (%r)" % ({k: mp.get(k) for k in ("api_url", "list_path", "map_path")},))
        check(mp.get("presets") == [{"title": "Raid Server Setup", "names": ["tortuga", "bastion"], "indices": [18, 43]}],
              "fixture: host preset maps paired by length (%r)" % mp.get("presets"))
        check_layouts("fixture", doc, FIX_LAYOUTS)
    ob = obfuscate(fx)
    check("\n" not in ob and "'buildVersion'" in ob and "dA(0x" in ob, "obfuscated fixture is one line with decoder calls")
    odoc, err = run_extract(ob)
    check(err is None, "obfuscated fixture extracts (%s)" % err)
    if odoc and doc:
        check(comparable(odoc) == comparable(doc), "obfuscated fixture: values identical to the readable one")
        check(odoc["deobfuscation"] and odoc["deobfuscation"][0]["calls"] > 50, "obfuscated fixture: decoder calls replaced")
    # decimal numbers, as v10.js prints them
    dec = re.sub(r"0x([0-9a-f]+)", lambda m: str(int(m.group(1), 16)), make_fixture())
    ddoc, err = run_extract(dec)
    check(err is None and ddoc and ddoc["ahkey"] == FIX_EXP["ahkey"], "decimal fixture extracts (%s)" % err)
    ddoc, err = run_extract(make_fixture(ahkey="94547"))
    check(err is None and ddoc and ddoc["ahkey"] == 94547, "decimal AHKEY accepted (%s)" % err)

    # every anchor broken -> non-zero exit naming it, nothing written
    breaks = [
        ("AHKEY", lambda s: s.replace("'cptR'] && (this", "'cptX'] && (this")),
        ("AHKEY", lambda s: s.replace("return a1 + a2 & 0xff;", "return a1 - a2 & 0xff;")),
        ("AHKEY", lambda s: s.replace("0x2a5f1", "0x2a5f1f")),
        ("ahNum trailer", lambda s: s.replace("[a3 >> 0x4 & 0xf, 0xf & a3]", "[a3 >> 0x3 & 0xf, 0xf & a3]")),
        ("clientVersion", lambda s: s.replace("'clientVersion': vv2", "'clientVersion': vvX")),
        ("clientVersion", lambda s: s.replace("var vv1 = '9.8.7';", "var vv1 = '9.8';")),
        ("buildVersion", lambda s: s.replace("AbCdEfGhIjKlMnOpQrStUvWxYz012345", "AbCdEfGhIjKlMnOpQrStUvWxYz01234")),
        ("packet table c2s", lambda s: s.replace("'ENTER_GAME': 'en'", "'ENTER_GAMEX': 'en'")),
        ("packet table s2c", lambda s: s.replace("'IO_INIT': 'io-init'", "'IO_INITX': 'io-init'")),
        ("packet table s2c", lambda s: s.replace("'CHEAT_CHECK': 'cc'", "'CHEAT_CHECKX': 'cc'")),
        ("cheat check", lambda s: s.replace("0x811c9dc5", "0x811c9dc6")),
        ("cheat salt", lambda s: s.replace("var salt1 = [", "var salt1 = [0x1, ")),
        ("cheat keystream", lambda s: s.replace(") + k3 & 0xff", ") + 0x1 & 0xff")),
        ("cheat handler", lambda s: s.replace("'_eval': e1", "'_evil': e1")),
        ("ADD_PLAYERS layout", lambda s: s.replace("i1 += 0x10;", "i1++;")),
        ("ADD_PLAYERS layout", lambda s: s.replace('f1["attachIndex"] = a8;', "")),
        ("ADD_PLAYERS handler", lambda s: s.replace('ts2c["ADD_PLAYERS"], hAdd)', 'ts2c["ADD_PLAYERS"], hAdd2)')),
        ("UPDATE_PLAYERS layout", lambda s: s.replace('pk["ping"] =', 'pk["pong"] =')),
        ("SYNC_PLAYER layout", lambda s: s.replace('you["onLadder"] = s1[0xe];', 'you["onLadder"] = s1[0x1f];')),
        ("SYNC_PLAYER layout", lambda s: s.replace("s1[0x1a + m1]", "s1[m1]")),
        ("KILL_PLAYER arguments", lambda s: s.replace("!v6 &&", "v6 &&")),
        ("KILL_PLAYER handler", lambda s: s.replace('ts2c["KILL_PLAYER"], hKill)', 'ts2c["KILL_PLAYER"], hKillX)')),
        ("DO_DAMAGE arguments", lambda s: s.replace('"aikills"', '"aikils"')),
        ("CHANGE_HEALTH arguments", lambda s: s.replace("|| e6)", "|| e7)")),
        ("GET_SCORE arguments", lambda s: s.replace("[g4 || 0x0]", "[g4]")),
        ("maps", lambda s: s.replace('"/maps/"["concat"](m1)', '"/maps/"')),
        ("maps", lambda s: s.replace('mlist["push"](r2)', 'mlist["push"](r1)')),
        ("maps", lambda s: s.replace('c9 && c9["data"]', 'c9')),
    ]
    for anchor, brk in breaks:
        broken = brk(fx)
        check(broken != fx, "break of %s applies" % anchor)
        path = os.path.join(tmp, "broken.js")
        with open(path, "w", encoding="utf8") as f:
            f.write(broken)
        hdr, js = os.path.join(tmp, "out.h"), os.path.join(tmp, "out.json")
        err = io.StringIO()
        with redirect_stderr(err), redirect_stdout(io.StringIO()):
            rc = E.main([path, "--header", hdr, "--json", js])
        msg = err.getvalue()
        check(rc != 0 and ("FAILED: " + anchor) in msg and not os.path.exists(hdr) and not os.path.exists(js),
              "broken %-17s -> rc %d, %s" % (anchor, rc, msg.strip().split("\n")[0][:110]))


# ------------------------------------------------------- 3. committed header, CLI, C compile

def test_committed(tmp):
    with open(E.OUT_JSON, encoding="utf8") as f:
        doc = json.load(f)
    with open(E.OUT_HEADER, encoding="utf8") as f:
        hdr = f.read()
    check(E.render_header(doc) == hdr, "krunker_build.h == render(build.json)")
    check(doc["generator_rev"] == E.GENERATOR_REV, "build.json generator rev current")
    check(re.fullmatch(r"[0-9a-f]{64}", doc["source"]["sha256"]) is not None, "build.json records the source sha256")
    check("KRUNKER_BUILD_SOURCE_SHA256 \"%s\"" % doc["source"]["sha256"] in hdr, "header records the source sha256")
    deob = current_bundle()
    if not deob:
        print("  SKIP --check / regenerate (build.json's bundle is not on this machine)")
    else:
        # the CLI, as `make krunker-build-check` runs it, offline on the local copy
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "extract_build.py"), deob, "--check"],
                           capture_output=True, text=True, timeout=300)
        check(r.returncode == 0 and "regenerated header identical" in r.stderr,
              "--check regenerates the committed header identically: rc %d %s" % (r.returncode, r.stderr.strip()[:300]))
        # --check catches a header that no longer matches what the extractor generates
        stale_h, stale_j = os.path.join(tmp, "stale.h"), os.path.join(tmp, "stale.json")
        with open(stale_h, "w", encoding="utf8") as f:
            f.write(hdr.replace("#define KRUNKER_ADD_STRIDE %d" % doc["layouts"]["add_players"]["stride"],
                                "#define KRUNKER_ADD_STRIDE 52"))
        stale_doc = json.loads(json.dumps(doc))
        stale_doc["layouts"]["add_players"]["stride"] = 52
        with open(stale_j, "w", encoding="utf8") as f:
            f.write(E.render_json(stale_doc))
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "extract_build.py"), deob, "--check",
                            "--header", stale_h, "--json", stale_j], capture_output=True, text=True, timeout=300)
        check(r.returncode == 1 and "KRUNKER_ADD_STRIDE 53" in r.stderr,
              "--check fails on a drifted stride: rc %d %s" % (r.returncode, r.stderr.strip()[:200]))
        # regenerate into a temp dir: identical except provenance (url vs local file)
        hdr2, js2 = os.path.join(tmp, "k.h"), os.path.join(tmp, "k.json")
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "extract_build.py"), deob,
                            "--header", hdr2, "--json", js2], capture_output=True, text=True, timeout=300)
        check(r.returncode == 0, "CLI regenerate rc %d" % r.returncode)
        if r.returncode == 0:
            with open(js2, encoding="utf8") as f:
                d2 = json.load(f)
            d1 = dict(doc)
            d1.pop("source")
            d2.pop("source")
            check(d1 == d2, "regenerated build.json values == committed")
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        print("  SKIP C compile (no compiler)")
        return
    c = os.path.join(tmp, "use_header.c")
    with open(c, "w") as f:
        f.write(r'''#include "krunker_build.h"
#include <string.h>
#if KRUNKER_BUILD_SCHEMA != 1
#error schema
#endif
#define WIRE(id, wire, name) wire,
static const char *const c2s[KRUNKER_C2S_COUNT] = { KRUNKER_C2S_PACKETS(WIRE) };
static const char *const s2c[KRUNKER_S2C_COUNT] = { KRUNKER_S2C_PACKETS(WIRE) };
#define OFF(c, off, name) off,
static const int add_offs[] = { KRUNKER_ADD_FIELDS(OFF) };
static const int k_offs[] = { KRUNKER_K_FIELDS(OFF) };
static const int sync_offs[] = { KRUNKER_SYNC_FIELDS(OFF) };
_Static_assert(KRUNKER_ADD_ATTACH_INDEX < KRUNKER_ADD_STRIDE && KRUNKER_K_PING < KRUNKER_K_STRIDE, "layout");
_Static_assert(KRUNKER_SYNC_PROTECTED < KRUNKER_SYNC_SEND_VALS && KRUNKER_KILL_ARG_INFO < KRUNKER_KILL_ARGC &&
               KRUNKER_DAMAGE_ARG_AI < KRUNKER_DAMAGE_ARGC && KRUNKER_HEALTH_ARG_RESET_MAX < KRUNKER_HEALTH_ARGC &&
               KRUNKER_SCORE_ARG_CLASS_INDEX < KRUNKER_SCORE_ARGC, "args");
int main(void) {
    if (add_offs[1] != KRUNKER_ADD_SID || k_offs[0] != KRUNKER_K_SID || sync_offs[0] != KRUNKER_SYNC_INPUT_SN) return 2;
    unsigned ah = 0;
    ah = (ah + KRUNKER_AHKEY) & 0xffu;
    return !(ah == KRUNKER_AHNUM_STEP && !strcmp(c2s[KRUNKER_C2S_ENTER_GAME], "en") &&
             !strcmp(s2c[KRUNKER_S2C_IO_INIT], "io-init") && !strcmp(c2s[KRUNKER_C2S_cptR], "cptR") &&
             sizeof KRUNKER_CHEAT_SALT - 1 == KRUNKER_CHEAT_SALT_LEN && KRUNKER_CHEAT_FNV_OFFSET == 0x811c9dc5u);
}
''')
    exe = os.path.join(tmp, "use_header")
    r = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I",
                        os.path.join(ROOT, "shared", "include"), c, "-o", exe], capture_output=True, text=True, timeout=120)
    check(r.returncode == 0, "header compiles as C11 -Werror with %s %s" % (os.path.basename(cc), r.stderr[:300]))
    if r.returncode == 0:
        check(subprocess.run([exe], timeout=30).returncode == 0, "header values usable from C")


def main():
    tmp = tempfile.mkdtemp(prefix="kn-krunker-extract-test-")
    try:
        for name, fn in (("real bundles", test_real_bundles), ("synthetic bundles", test_synthetic),
                         ("committed header", test_committed)):
            print("%s:" % name)
            fn(tmp)
    finally:
        shutil.rmtree(tmp)
    print("%d checks passed, %d failed" % (passed[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
