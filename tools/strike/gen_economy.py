#!/usr/bin/env python3
"""Krunker Strike economy generator: tools/strike/strike_data.json (+ assets/skins.json) ->
shared/src/rules/strike/strike_economy_data.c and services/db/migrations/<dialect>/0320_strike_economy.sql

strike_data.json is extract_strike.py's text-only extraction of FRVR's Krunker Strike 1.37.2 bundle.
What comes from it:
  items.item_types, economy.weapon_types / item_rarities     the catalogue's enums
  items.rarities                                           rarity names, colours, i18n keys, sell KR
  items.crate_tiers                                        the wheels' rarity odds (Starter, Elite, Heroic)
  items.packages (KR Package)                              KR amounts of the KR packages
  economy.currencies                                       the wallet currencies
  economy.xp_curve / golden_spin_meter_default / past_max_level_reward   the progression rules
  economy.mission_keys, i18n chall.name.*                  the mission keys and their texts
  classes (strike_rules)                                   class rarities and unlock origins
What FRVR serves from its gateway and never shipped in the client, so KrunkNative chooses (each
choice is marked KN below and in the generated files):
  the catalogue's items       FRVR item definitions name Krunker store skins (`id`, `weapon`,
                              `rarity`): the official Krunker store skins (no creator, not limited,
                              not bundle / battle pass / ranked, sellable) of Strike's six class
                              weapons, melee and hats / backs / sprays of seasons 1-2, plus every
                              official Unobtainable of those (the Heroic wheel's 0.01%)
  wheel prices                Krunker v10's Starter 50 KR / Elite 100 KR (store_spins_table.c)
  store, missions, track      listed below

Usage: python3 -I tools/strike/gen_economy.py            write the files
       python3 -I tools/strike/gen_economy.py --check    exit 1 when a checked-in file is stale
       python3 -I tools/strike/gen_economy.py --json     the catalogue as JSON (tests)
Stdlib only; reads JSON, executes nothing.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tools", "strike", "strike_data.json")
SKINS = os.path.join(ROOT, "assets", "skins.json")
OUT_C = os.path.join(ROOT, "shared", "src", "rules", "strike", "strike_economy_data.c")
MIGRATION = "0320_strike_economy.sql"
OUT_SQL = {d: os.path.join(ROOT, "services", "db", "migrations", d, MIGRATION) for d in ("postgres", "sqlite", "mysql")}

# Strike's class weapons (classes[].loadout, weapon enum = Krunker weapon id - 1): Triggerman AR,
# Hunter Sniper, Run N Gun SMG, Shotgunner Shotgun, Detective Revolver, Rocketeer RL.
COSMETIC_TYPES = (1, 2, 3, 4)       # hat, back, melee, spray
MAX_SEASON = 2                      # KN: Krunker's seasons 1-2 (the classic catalogue)
RARITY_POOLS = ["uncommon", "rare", "epic", "legendary", "relic", "contraband", "unobtainable"]
DEAL_PRICES = [60, 150, 400, 1500, 5000, 15000, 50000, 0]  # KN: daily deal KR by rarity

# KN: the wheels. Odds = the bundle's crate tiers; prices = Krunker v10's wheel prices.
WHEELS = [
    dict(id="starter", name="Starter", key="windows.store.spin.type.starter", type="default", tier="Starter",
         cur="kr", amount=50, color="#b2f252", golden=1),
    dict(id="elite", name="Elite", key="windows.store.spin.type.elite", type="default", tier="Elite",
         cur="kr", amount=100, color="#2196F3", golden=1),
    dict(id="lucky_draw", name="Lucky Draw", key="windows.store.spin.type.luckydraw", type="default", tier="Elite",
         cur="spin_ticket", amount=1, color="#FBC02D", golden=1, heroic=0.02, guaranteed=("epic", 10)),
    dict(id="golden_spin", name="Golden", key="windows.store.spin.type.heroic", type="golden_spin", tier="Heroic",
         cur="spin_golden_ticket", amount=1, color="#F2D833", hide=1),
    dict(id="free", name="Free", key="windows.store.spin.type.free", type="free", tier="Starter",
         cur=None, amount=0, color="#15C5D0", every=4 * 3600, accumulate=1),
]

MISSIONS = [  # KN: the daily mission pool (key, required, reward, max_mode)
    ("k", 15, ("kr", 25), 0), ("k", 40, ("spin_ticket", 1), 0), ("hs", 5, ("kr", 25), 0), ("hs", 15, ("kr", 60), 0),
    ("gp", 3, ("kr", 25), 0), ("w", 1, ("kr", 25), 0), ("w", 3, ("spin_ticket", 1), 0), ("score", 2500, ("kr", 25), 0),
    ("score", 7500, ("kr", 60), 0), ("ast", 5, ("kr", 25), 0), ("mk", 2, ("kr", 30), 0), ("kstk", 5, ("kr", 50), 1),
]
MAX_LEVEL = 30           # KN: the free track's length (maxLevelCap)
MISSIONS_UNLOCK = 3      # KN: the track's "missions" feature level
DAILY_MISSIONS = 3
DEAL_COUNT = 4


def load():
    with open(DATA, encoding="utf-8") as f:
        d = json.load(f)
    with open(SKINS, encoding="utf-8") as f:
        skins = json.load(f)
    return d, skins


def official(x):
    return not (x.get("creator") or x.get("creators") or x.get("limT") or x.get("limited") or x.get("bundle")
                or x.get("battlepass") or x.get("ranked") or x.get("noSpin") or x.get("noSale"))


def build(d, skins):
    eco, items_sec = d["economy"], d["items"]
    rar_enum = sorted(eco["item_rarities"].items(), key=lambda kv: kv[1])
    rarities = []
    for name, i in rar_enum:
        src = next((r for r in items_sec["rarities"] if r["name"].lower() == name.lower()), {})
        rarities.append({"name": src.get("name", name), "color": src.get("color", "#fff"),
                         "i18n": src.get("language", ""), "sell": int(src.get("sell", 0)), "deal": DEAL_PRICES[i]})
    weapon_names = [w["name"] for w in d["weapons"]]
    type_names = [k for k, _ in sorted(items_sec["item_types"].items(), key=lambda kv: kv[1])]
    classes = d["classes"]
    class_weapons = sorted({c["loadout"][0] for c in classes})  # weapon enum
    cur_keys = eco["currencies"]
    currencies = [cur_keys[k] for k in ("EXPERIENCE", "KRUNKIES", "SPIN_TICKET", "SPIN_GOLDEN_TICKET", "NAME_CHANGE",
                                        "DAILY_KR_GAINED", "SEASONAL_EVENT_TOKENS")]

    items = []
    # defaults: the plain look of every class weapon and of melee (getDefaultItem(type))
    for w in class_weapons:
        items.append(dict(id="d0_%d" % w, type=0, weapon=w, name="Default " + weapon_names[w], rarity=-1, skin=-1,
                          class_id=-1, default=1, spin=0, origin=None))
    items.append(dict(id="d3", type=3, weapon=-1, name="Default Knife", rarity=-1, skin=-1, class_id=-1, default=1,
                      spin=0, origin=None))
    # classes (type 15): rarity = the class's; origin = its unlockable
    for c in classes:
        rar = c.get("rarity", 0) or 0
        items.append(dict(id="c%d" % c["id"], type=15, weapon=-1, name=c["name"], rarity=rar, skin=-1, class_id=c["id"],
                          default=0 if c.get("unlockable") else 1, spin=1 if c.get("unlockable") == "spin_tickets" else 0,
                          origin=c.get("unlockable")))
    wanted_weapons = {w + 1 for w in class_weapons}
    picked = []
    for x in skins:
        if not isinstance(x, dict) or not official(x):
            continue
        t, r = x.get("type", 0), x.get("rarity", 0)
        if r > 6 or t not in (0,) + COSMETIC_TYPES:
            continue
        if t == 0 and x.get("weapon") not in wanted_weapons:
            continue
        if x.get("seas", 1) > MAX_SEASON and r != 6:
            continue
        picked.append(x)
    picked.sort(key=lambda x: x["index"])
    for x in picked:
        t = x.get("type", 0)
        items.append(dict(id="s%d" % x["index"], type=t, weapon=x["weapon"] - 1 if t == 0 else -1, name=x["name"],
                          rarity=x["rarity"], skin=x["index"], class_id=-1, default=0, spin=1, origin="spin_tickets"))
    index = {it["id"]: i for i, it in enumerate(items)}

    pools = []
    for r, pid in enumerate(RARITY_POOLS):
        members = [i for i, it in enumerate(items) if it["spin"] and it["rarity"] == r]
        pools.append(dict(id=pid, name=rarities[r]["name"], color=rarities[r]["color"], rarity=r, items=members))

    tiers = {t["name"]: t["rarities"] for t in items_sec["crate_tiers"]}
    wheels = []
    for w in WHEELS:
        odds = tiers[w["tier"]]
        total = sum(odds)
        slices = []
        for r, pct in enumerate(odds):
            if pct <= 0 or not pools[r]["items"]:
                continue
            g = w.get("guaranteed")
            slices.append(dict(pool=RARITY_POOLS[r], weight=round(pct / total, 6),
                               guaranteed=g[1] if g and g[0] == RARITY_POOLS[r] else 0))
        wheels.append(dict(id=w["id"], name=w["name"], key=w["key"], type=w["type"], cur=w["cur"], amount=w["amount"],
                           every=w.get("every", 0), accumulate=w.get("accumulate", 0), heroic=w.get("heroic", 0.0),
                           hide=w.get("hide", 0), golden=w.get("golden", 0), color=w["color"], category="spins",
                           slices=slices, tier=w["tier"]))

    kr_pk = [p["kr"] for p in items_sec["packages"] if p.get("name") == "KR Package" and "kr" in p]

    def cur(c, n):
        return dict(type="currency", currency=c, amount=n)

    def item(i):
        return dict(type="item", item=i)

    hunter = next(c for c in classes if c.get("unlockable") == "bundle")
    sniper_leg = next((it["id"] for it in items if it["type"] == 0 and it["weapon"] == hunter["loadout"][0]
                       and it["rarity"] == 3), None)
    products = [  # KN: the store (virtual currency / free only; KrunkNative takes no real money)
        dict(id="free_daily", section="free", title="KR Package", ptype="free", cur=None, amount=0,
             rewards=[cur("kr", kr_pk[1])], refresh=86400, maxp=1),
        dict(id="tickets_1", section="tickets", title="1 Spin Ticket", ptype="virtual_currency", cur="kr", amount=80,
             rewards=[cur("spin_ticket", 1)], refresh=0, maxp=0),
        dict(id="tickets_10", section="tickets", title="10 Spin Tickets", ptype="virtual_currency", cur="kr", amount=720,
             rewards=[cur("spin_ticket", 10)], refresh=0, maxp=0),
        dict(id="golden_ticket_1", section="tickets", title="Golden Spin Ticket", ptype="virtual_currency", cur="kr",
             amount=500, rewards=[cur("spin_golden_ticket", 1)], refresh=0, maxp=0),
        dict(id="bundle_%s" % hunter["name"].lower().replace(" ", "_"), section="bundles", title=hunter["name"] + " Bundle",
             ptype="virtual_currency", cur="kr", amount=2000,
             rewards=[dict(type="class", item="c%d" % hunter["id"], class_id=hunter["id"])] + ([item(sniper_leg)] if sniper_leg else []),
             refresh=0, maxp=1),
        dict(id="name_change", section="currency", title="Name Change", ptype="virtual_currency", cur="kr", amount=250,
             rewards=[cur("name_change", 1)], refresh=0, maxp=0),
    ]
    for p in products:
        for rw in p["rewards"]:
            assert rw["type"] == "currency" or rw["item"] in index, rw

    i18n = d["i18n"]
    missions = []
    for key, req, (rc, ra), mx in MISSIONS:
        assert key in eco["mission_keys"] or key in ("gp", "w"), key
        missions.append(dict(id="%s_%d" % (key, req), key=key, text="chall.name." + key, required=req, max=mx, weight=1,
                             rewards=[cur(rc, ra)], text_en=i18n.get("chall.name." + key, "")))

    # KN: the free track (accountProgression) levels 1..MAX_LEVEL
    by_class = {c["id"]: c for c in classes}
    track_classes = [c for c in classes if c.get("unlockable") == "rewards_track"]
    track_classes.sort(key=lambda c: c.get("rarity", 0))

    def choice(rarity, n=3):
        out = []
        for w in class_weapons:
            cand = [it["id"] for it in items if it["type"] == 0 and it["weapon"] == w and it["rarity"] == rarity]
            if cand:
                out.append(item(cand[0]))
            if len(out) == n:
                break
        return out

    track = []
    for lvl in range(1, MAX_LEVEL + 1):
        pc = 0
        if lvl == 1:
            rewards, pc = choice(0), 1
        elif lvl == MISSIONS_UNLOCK:
            rewards = [dict(type="feature", feature="missions")]
        elif lvl == 5 and track_classes:
            c = track_classes[0]
            rewards = [dict(type="class", item="c%d" % c["id"], class_id=c["id"])]
        elif lvl == 15 and len(track_classes) > 1:
            c = track_classes[1]
            rewards = [dict(type="class", item="c%d" % c["id"], class_id=c["id"])]
        elif lvl % 10 == 0:
            rewards = [cur("spin_golden_ticket", 1)]
        elif lvl % 10 == 8:
            rewards, pc = choice(min(1 + lvl // 10, 3)), 1
        elif lvl % 2 == 0:
            rewards = [cur("spin_ticket", 1)]
        else:
            rewards = [cur("kr", 50 * (1 + lvl // 10))]
        track.append(dict(level=lvl, slot="%d_00" % lvl, pc=pc, rewards=rewards))
    del by_class

    starter = [c["id"] for c in classes if not c.get("unlockable")]
    cfg = dict(xp_base=eco["xp_curve"]["base"], xp_step=eco["xp_curve"]["step"], xp_divisor=eco["xp_curve"]["divisor"],
               max_level=MAX_LEVEL, past=eco["past_max_level_reward"], golden=eco["golden_spin_meter_default"],
               missions_unlock=MISSIONS_UNLOCK, daily_missions=DAILY_MISSIONS, deals=DEAL_COUNT, packages=kr_pk,
               starter=starter)
    return dict(version=d["meta"]["game_version"], currencies=currencies, rarities=rarities, type_names=type_names,
                weapon_names=weapon_names, items=items, pools=pools, wheels=wheels, products=products,
                missions=missions, track=track, cfg=cfg)


# ------------------------------------------------------------------------------------------- C

def cstr(s):
    if s is None:
        return "NULL"
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def creward(rw):
    return "{%s, %s, %d, %s, %d, %s}" % (cstr(rw["type"]), cstr(rw.get("currency")), rw.get("amount", 0),
                                          cstr(rw.get("item")), rw.get("class_id", -1) if rw["type"] == "class" else -1,
                                          cstr(rw.get("feature")))


def gen_c(m):
    o = ["// GENERATED by tools/strike/gen_economy.py from tools/strike/strike_data.json and assets/skins.json:",
         "// do not edit. Regenerate: python3 -I tools/strike/gen_economy.py",
         "// Krunker Strike %s's economy (shared/include/strike_economy.h). Values marked KN are KrunkNative's" % m["version"],
         "// (FRVR serves them from its gateway); everything else is the bundle's.",
         "#include <strike_economy.h>", ""]
    o.append("static const char *const k_currency_keys[STRIKE_CUR_COUNT] = {%s};" % ", ".join(cstr(c) for c in m["currencies"]))
    o.append("// items.rarities (deal_price: KN)")
    o.append("static const StrikeRarity k_rarities[STRIKE_RARITY_COUNT] = {")
    for r in m["rarities"]:
        o.append("    {%s, %s, %s, %d, %d}," % (cstr(r["name"]), cstr(r["color"]), cstr(r["i18n"]), r["sell"], r["deal"]))
    o.append("};")
    o.append("static const char *const k_type_names[STRIKE_ITEM_TYPE_COUNT] = {%s};" % ", ".join(cstr(t) for t in m["type_names"]))
    o.append("static const char *const k_weapon_names[] = {%s};" % ", ".join(cstr(w) for w in m["weapon_names"]))
    o.append("")
    o.append("// the catalogue (%d items): defaults, classes, then Krunker store skins by store index" % len(m["items"]))
    o.append("static const StrikeItem k_items[] = {")
    for it in m["items"]:
        o.append("    {%s, %d, %d, %s, %d, %d, %d, %d, %d, %s}," % (cstr(it["id"]), it["type"], it["weapon"], cstr(it["name"]),
                                                                  it["rarity"], it["skin"], it["class_id"], it["default"],
                                                                  it["spin"], cstr(it["origin"])))
    o.append("};")
    for p in m["pools"]:
        o.append("static const int k_pool_%s[] = {%s};" % (p["id"], ", ".join(str(i) for i in p["items"]) or "-1"))
    o.append("static const StrikePool k_pools[] = {")
    for p in m["pools"]:
        o.append("    {%s, %s, %s, %d, k_pool_%s, %d}," % (cstr(p["id"]), cstr(p["name"]), cstr(p["color"]), p["rarity"], p["id"],
                                                         len(p["items"])))
    o.append("};")
    o.append("")
    o.append("// KN wheels: odds = items.crate_tiers (as fractions), prices = Krunker v10's")
    for w in m["wheels"]:
        o.append("static const StrikeWheelSlice k_slices_%s[] = { // crate tier %s" % (w["id"], w["tier"]))
        for s in w["slices"]:
            o.append("    {%s, %r, %d}," % (cstr(s["pool"]), s["weight"], s["guaranteed"]))
        o.append("};")
    o.append("static const StrikeWheel k_wheels[] = {")
    for w in m["wheels"]:
        o.append("    {%s, %s, %s, %s, %s, %d, %d, %d, %r, %d, %d, %s, %s, k_slices_%s, %d}," % (
            cstr(w["id"]), cstr(w["name"]), cstr(w["key"]), cstr(w["type"]), cstr(w["cur"]), w["amount"], w["every"],
            w["accumulate"], float(w["heroic"]), w["hide"], w["golden"], cstr(w["color"]), cstr(w["category"]), w["id"],
            len(w["slices"])))
    o.append("};")
    o.append("")
    o.append("// KN store (virtual currency / free only)")
    for p in m["products"]:
        o.append("static const StrikeReward k_prod_%s[] = {%s};" % (p["id"], ", ".join(creward(r) for r in p["rewards"])))
    o.append("static const StrikeProduct k_products[] = {")
    for p in m["products"]:
        o.append("    {%s, %s, %s, %s, %s, %d, k_prod_%s, %d, %d, %d}," % (cstr(p["id"]), cstr(p["section"]), cstr(p["title"]),
                                                                      cstr(p["ptype"]), cstr(p["cur"]), p["amount"], p["id"],
                                                                      len(p["rewards"]), p["refresh"], p["maxp"]))
    o.append("};")
    o.append("")
    o.append("// KN daily missions (keys: the game's missions.update keys; texts: i18n chall.name.*)")
    for ms in m["missions"]:
        o.append("static const StrikeReward k_mis_%s[] = {%s}; // %s" % (ms["id"], ", ".join(creward(r) for r in ms["rewards"]),
                                                                       ms["text_en"].replace("{0}", str(ms["required"])).replace(" {1}", "")))
    o.append("static const StrikeMission k_missions[] = {")
    for ms in m["missions"]:
        o.append("    {%s, %s, %s, %d, %d, %d, k_mis_%s, %d}," % (cstr(ms["id"]), cstr(ms["key"]), cstr(ms["text"]), ms["required"],
                                                              ms["max"], ms["weight"], ms["id"], len(ms["rewards"])))
    o.append("};")
    o.append("")
    o.append("// KN account progression track (accountProgression; the XP curve is the bundle's)")
    for t in m["track"]:
        o.append("static const StrikeReward k_trk_%d[] = {%s};" % (t["level"], ", ".join(creward(r) for r in t["rewards"])))
    o.append("static const StrikeTrackSlot k_track[] = {")
    for t in m["track"]:
        o.append("    {%d, %s, %d, k_trk_%d, %d}," % (t["level"], cstr(t["slot"]), t["pc"], t["level"], len(t["rewards"])))
    o.append("};")
    c = m["cfg"]
    o.append("static const int k_packages_kr[] = {%s}; // items.packages: KR Package by rarity" % ", ".join(str(x) for x in c["packages"]))
    o.append("static const int k_starter_classes[] = {%s}; // classes without an unlockable" % ", ".join(str(x) for x in c["starter"]))
    o.append("")
    o.append("static const StrikeEconomy k_economy = {")
    o.append("    .game_version = %s," % cstr(m["version"]))
    o.append("    .currency_keys = k_currency_keys,")
    o.append("    .rarities = k_rarities,")
    o.append("    .item_type_names = k_type_names,")
    o.append("    .weapon_names = k_weapon_names, .weapon_count = %d," % len(m["weapon_names"]))
    o.append("    .items = k_items, .item_count = %d," % len(m["items"]))
    o.append("    .pools = k_pools, .pool_count = %d," % len(m["pools"]))
    o.append("    .wheels = k_wheels, .wheel_count = %d," % len(m["wheels"]))
    o.append("    .products = k_products, .product_count = %d," % len(m["products"]))
    o.append("    .missions = k_missions, .mission_count = %d," % len(m["missions"]))
    o.append("    .track = k_track, .track_count = %d," % len(m["track"]))
    o.append("    .xp_base = %d, .xp_step = %d, .xp_divisor = %r, // economy.xp_curve" % (c["xp_base"], c["xp_step"], c["xp_divisor"]))
    o.append("    .max_level = %d, // KN" % c["max_level"])
    o.append("    .past_max_level_reward = {\"currency\", %s, %d, NULL, -1, NULL}, // economy.past_max_level_reward"
             % (cstr(c["past"]["currency"]), c["past"]["amount"]))
    o.append("    .golden_spin_meter = %d, // economy.golden_spin_meter_default" % c["golden"])
    o.append("    .missions_unlock_level = %d, .daily_mission_count = %d, .deal_count = %d, // KN"
             % (c["missions_unlock"], c["daily_missions"], c["deals"]))
    o.append("    .packages_kr = k_packages_kr, .package_count = %d," % len(c["packages"]))
    o.append("    .starter_classes = k_starter_classes, .starter_class_count = %d," % len(c["starter"]))
    o.append("};")
    o.append("")
    o.append("const StrikeEconomy *strike_economy(void) { return &k_economy; }")
    return "\n".join(o) + "\n"


# ------------------------------------------------------------------------------------------- SQL

def sq(v, dialect):
    if v is None:
        return "NULL"
    if isinstance(v, (int, float)):
        return repr(v)
    s = str(v).replace("'", "''")
    if dialect == "mysql":
        s = s.replace("\\", "\\\\")
    return "'" + s + "'"


def rewards_json(rws):
    return json.dumps([{k: v for k, v in r.items() if v is not None} for r in rws], separators=(",", ":"), sort_keys=True)


TYPES = {
    "postgres": dict(text="text", int="integer", small="smallint", big="bigint", real="double precision", ts="timestamptz",
                     now="now()", serial="bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY", key="text",
                     acct="integer NOT NULL REFERENCES accounts(id) ON DELETE CASCADE"),
    "sqlite": dict(text="TEXT", int="INTEGER", small="INTEGER", big="INTEGER", real="REAL", ts="TIMESTAMP",
                   now="(strftime('%Y-%m-%d %H:%M:%f', 'now'))", serial="INTEGER PRIMARY KEY AUTOINCREMENT", key="TEXT",
                   acct="INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE"),
    "mysql": dict(text="TEXT", int="INT", small="SMALLINT", big="BIGINT", real="DOUBLE", ts="DATETIME(3)",
                  now="CURRENT_TIMESTAMP(3)", serial="BIGINT AUTO_INCREMENT PRIMARY KEY", key="VARCHAR(64)", acct="BIGINT NOT NULL"),
}


def gen_sql(m, dialect):
    T = TYPES[dialect]
    K, X, I, S, B, R, TS, NOW = T["key"], T["text"], T["int"], T["small"], T["big"], T["real"], T["ts"], T["now"]
    name = "VARCHAR(255) NOT NULL DEFAULT ''" if dialect == "mysql" else "%s NOT NULL DEFAULT ''" % X
    long = "TEXT NOT NULL" if dialect == "mysql" else "%s NOT NULL DEFAULT ''" % X

    o = ["-- 0320_strike_economy: Krunker Strike's economy and progression, its own tables (keyed by account,",
         "-- never Box Office's players / inventory / kr_transactions). Owner: strike-economy.",
         "-- GENERATED by tools/strike/gen_economy.py from tools/strike/strike_data.json (FRVR Krunker Strike %s)" % m["version"],
         "-- and assets/skins.json: do not edit; regenerate. Additive: new tables only.",
         "--",
         "-- Catalogue (seeded below; kn_brain serves it, services/brain/strike_econ.c): strike_item_defs (FRVR's",
         "-- item definitions), strike_pools / strike_pool_items (wheel pools), strike_wheels / strike_wheel_slices",
         "-- (spin wheels: odds = the bundle's crate tiers), strike_products (the store; no real money),",
         "-- strike_missions_def (daily missions), strike_track (account progression rewards), strike_config.",
         "-- Per account: strike_wallets (xp, kr = krunkies, spin tickets, ...), strike_items (inventory),",
         "-- strike_loadout (equipped per slot), strike_progress (XP, stats, golden spin meter: leaderboards),",
         "-- strike_wheel_spins, strike_daily_missions, strike_track_claims, strike_purchases, strike_ledger,",
         "-- strike_match_results (commit idempotency).", ""]
    acct = T["acct"]
    o.append("""CREATE TABLE strike_item_defs (
    id          {K} PRIMARY KEY,
    item_type   {S} NOT NULL,
    weapon      {S} NOT NULL DEFAULT -1,
    name        {name},
    rarity      {S} NOT NULL DEFAULT -1,
    skin_index  {I} NOT NULL DEFAULT -1,
    class_id    {S} NOT NULL DEFAULT -1,
    is_default  {S} NOT NULL DEFAULT 0,
    spinnable   {S} NOT NULL DEFAULT 0,
    origin      {origin},
    sort        {I} NOT NULL DEFAULT 0
);
CREATE TABLE strike_pools (
    id      {K} PRIMARY KEY,
    name    {name},
    color   {name},
    rarity  {S} NOT NULL DEFAULT 0,
    sort    {I} NOT NULL DEFAULT 0
);
CREATE TABLE strike_pool_items (
    pool_id {K} NOT NULL,
    item_id {K} NOT NULL,
    PRIMARY KEY (pool_id, item_id)
);
CREATE TABLE strike_wheels (
    id              {K} PRIMARY KEY,
    name            {name},
    name_key        {name},
    wheel_type      {name},
    price_currency  {origin},
    price_amount    {I} NOT NULL DEFAULT 0,
    time_between_s  {I} NOT NULL DEFAULT 0,
    max_accumulated {I} NOT NULL DEFAULT 0,
    heroic_chance   {R} NOT NULL DEFAULT 0,
    hide_in_shop    {S} NOT NULL DEFAULT 0,
    counts_golden   {S} NOT NULL DEFAULT 0,
    button_color    {name},
    category        {name},
    active          {S} NOT NULL DEFAULT 1,
    sort            {I} NOT NULL DEFAULT 0
);
CREATE TABLE strike_wheel_slices (
    wheel_id      {K} NOT NULL,
    pool_id       {K} NOT NULL,
    weight        {R} NOT NULL,
    guaranteed_if {I} NOT NULL DEFAULT 0,
    sort          {I} NOT NULL DEFAULT 0,
    PRIMARY KEY (wheel_id, pool_id)
);
CREATE TABLE strike_products (
    id             {K} PRIMARY KEY,
    section        {name},
    title          {name},
    price_type     {name},
    price_currency {origin},
    price_amount   {I} NOT NULL DEFAULT 0,
    rewards        {long},
    refresh_s      {I} NOT NULL DEFAULT 0,
    max_purchases  {I} NOT NULL DEFAULT 0,
    active         {S} NOT NULL DEFAULT 1,
    sort           {I} NOT NULL DEFAULT 0
);
CREATE TABLE strike_missions_def (
    id           {K} PRIMARY KEY,
    mission_type {name},
    mkey         {name},
    text_key     {name},
    required     {I} NOT NULL,
    max_mode     {S} NOT NULL DEFAULT 0,
    weight       {I} NOT NULL DEFAULT 1,
    rewards      {long},
    active       {S} NOT NULL DEFAULT 1,
    sort         {I} NOT NULL DEFAULT 0
);
CREATE TABLE strike_track (
    slot_id       {K} PRIMARY KEY,
    level         {I} NOT NULL,
    player_choice {S} NOT NULL DEFAULT 0,
    rewards       {long}
);
CREATE TABLE strike_config (
    ckey   {K} PRIMARY KEY,
    value  {long}
);
""".format(K=K, S=S, I=I, R=R, name=name, origin=name, long=long))
    o.append("""CREATE TABLE strike_wallets (
    account_id {acct},
    currency   {K} NOT NULL,
    amount     {B} NOT NULL DEFAULT 0 CHECK (amount >= 0),
    PRIMARY KEY (account_id, currency)
);
CREATE TABLE strike_items (
    account_id {acct},
    item_id    {K} NOT NULL,
    count      {I} NOT NULL DEFAULT 1 CHECK (count >= 0),
    first_at   {TS} NOT NULL DEFAULT {NOW},
    PRIMARY KEY (account_id, item_id)
);
CREATE TABLE strike_loadout (
    account_id {acct},
    slot       {K} NOT NULL,
    item_id    {K} NOT NULL,
    PRIMARY KEY (account_id, slot)
);
CREATE TABLE strike_progress (
    account_id        {pk},
    xp                {B} NOT NULL DEFAULT 0,
    kills             {B} NOT NULL DEFAULT 0,
    deaths            {B} NOT NULL DEFAULT 0,
    wins              {B} NOT NULL DEFAULT 0,
    games             {B} NOT NULL DEFAULT 0,
    score             {B} NOT NULL DEFAULT 0,
    time_ms           {B} NOT NULL DEFAULT 0,
    golden_spin_count {I} NOT NULL DEFAULT 0,
    updated_at        {TS} NOT NULL DEFAULT {NOW}
);
CREATE INDEX strike_progress_xp_idx ON strike_progress (xp);
CREATE INDEX strike_progress_kills_idx ON strike_progress (kills);
CREATE INDEX strike_progress_wins_idx ON strike_progress (wins);
CREATE INDEX strike_progress_score_idx ON strike_progress (score);
CREATE TABLE strike_wheel_spins (
    account_id   {acct},
    wheel_id     {K} NOT NULL,
    spins        {B} NOT NULL DEFAULT 0,
    last_spin_ms {B} NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, wheel_id)
);
CREATE TABLE strike_daily_missions (
    account_id {acct},
    day        {I} NOT NULL,
    mission_id {K} NOT NULL,
    progress   {B} NOT NULL DEFAULT 0,
    completed  {S} NOT NULL DEFAULT 0,
    claimed    {S} NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, day, mission_id)
);
CREATE TABLE strike_track_claims (
    account_id {acct},
    slot_id    {K} NOT NULL,
    choice     {I} NOT NULL DEFAULT 0,
    claimed_at {TS} NOT NULL DEFAULT {NOW},
    PRIMARY KEY (account_id, slot_id)
);
CREATE TABLE strike_purchases (
    account_id {acct},
    product_id {K} NOT NULL,
    period     {B} NOT NULL DEFAULT 0,
    count      {I} NOT NULL DEFAULT 0,
    PRIMARY KEY (account_id, product_id, period)
);
CREATE TABLE strike_ledger (
    id         {serial},
    account_id {acct},
    kind       {K} NOT NULL,
    currency   {K} NOT NULL DEFAULT '',
    delta      {B} NOT NULL DEFAULT 0,
    item_id    {K} NOT NULL DEFAULT '',
    ref        {K} NOT NULL DEFAULT '',
    at         {TS} NOT NULL DEFAULT {NOW}
);
CREATE INDEX strike_ledger_account_idx ON strike_ledger (account_id, id);
CREATE TABLE strike_match_results (
    match_key  {K} NOT NULL,
    account_id {acct},
    at         {TS} NOT NULL DEFAULT {NOW},
    PRIMARY KEY (match_key, account_id)
);
""".format(acct=acct, K=K if dialect != "mysql" else "VARCHAR(160)", I=I, S=S, B=B,
           TS=TS, NOW=NOW, serial=T["serial"],
           pk=(acct + " PRIMARY KEY") if dialect != "mysql" else "BIGINT NOT NULL PRIMARY KEY"))

    def insert(table, cols, rows, chunk=200):
        for i in range(0, len(rows), chunk):
            vals = ",\n".join("    (" + ", ".join(sq(v, dialect) for v in r) + ")" for r in rows[i:i + chunk])
            o.append("INSERT INTO %s (%s) VALUES\n%s;" % (table, ", ".join(cols), vals))

    o.append("-- ---- the catalogue (seed rows)")
    insert("strike_item_defs", ["id", "item_type", "weapon", "name", "rarity", "skin_index", "class_id", "is_default", "spinnable",
                                "origin", "sort"],
           [(it["id"], it["type"], it["weapon"], it["name"], it["rarity"], it["skin"], it["class_id"], it["default"], it["spin"],
             it["origin"] or "", n) for n, it in enumerate(m["items"])])
    insert("strike_pools", ["id", "name", "color", "rarity", "sort"],
           [(p["id"], p["name"], p["color"], p["rarity"], n) for n, p in enumerate(m["pools"])])
    insert("strike_pool_items", ["pool_id", "item_id"],
           [(p["id"], m["items"][i]["id"]) for p in m["pools"] for i in p["items"]], chunk=400)
    insert("strike_wheels", ["id", "name", "name_key", "wheel_type", "price_currency", "price_amount", "time_between_s",
                             "max_accumulated", "heroic_chance", "hide_in_shop", "counts_golden", "button_color", "category", "sort"],
           [(w["id"], w["name"], w["key"], w["type"], w["cur"] or "", w["amount"], w["every"], w["accumulate"], float(w["heroic"]),
             w["hide"], w["golden"], w["color"], w["category"], n) for n, w in enumerate(m["wheels"])])
    insert("strike_wheel_slices", ["wheel_id", "pool_id", "weight", "guaranteed_if", "sort"],
           [(w["id"], s["pool"], s["weight"], s["guaranteed"], n) for w in m["wheels"] for n, s in enumerate(w["slices"])])
    insert("strike_products", ["id", "section", "title", "price_type", "price_currency", "price_amount", "rewards", "refresh_s",
                               "max_purchases", "sort"],
           [(p["id"], p["section"], p["title"], p["ptype"], p["cur"] or "", p["amount"], rewards_json(p["rewards"]), p["refresh"],
             p["maxp"], n) for n, p in enumerate(m["products"])])
    insert("strike_missions_def", ["id", "mission_type", "mkey", "text_key", "required", "max_mode", "weight", "rewards", "sort"],
           [(ms["id"], "daily", ms["key"], ms["text"], ms["required"], ms["max"], ms["weight"], rewards_json(ms["rewards"]), n)
            for n, ms in enumerate(m["missions"])])
    insert("strike_track", ["slot_id", "level", "player_choice", "rewards"],
           [(t["slot"], t["level"], t["pc"], rewards_json(t["rewards"])) for t in m["track"]])
    c = m["cfg"]
    conf = [("game_version", m["version"]), ("xp_base", str(c["xp_base"])), ("xp_step", str(c["xp_step"])),
            ("xp_divisor", repr(c["xp_divisor"])), ("max_level", str(c["max_level"])),
            ("past_max_level_reward", rewards_json([dict(type="currency", **c["past"])])),
            ("golden_spin_meter", str(c["golden"])), ("missions_unlock_level", str(c["missions_unlock"])),
            ("daily_mission_count", str(c["daily_missions"])), ("deal_count", str(c["deals"])),
            ("deal_prices", json.dumps([r["deal"] for r in m["rarities"]], separators=(",", ":"))),
            ("starter_classes", json.dumps(c["starter"], separators=(",", ":")))]
    insert("strike_config", ["ckey", "value"], conf)
    return "\n".join(o) + "\n"


def catalogue_json(m):
    return json.dumps({"items": [{k: it[k] for k in ("id", "type", "weapon", "name", "rarity", "skin", "class_id", "default", "spin")}
                                 for it in m["items"]],
                       "pools": [{"id": p["id"], "count": len(p["items"])} for p in m["pools"]],
                       "wheels": [{"id": w["id"], "type": w["type"], "currency": w["cur"], "amount": w["amount"], "tier": w["tier"],
                                   "slices": w["slices"], "heroic": w["heroic"], "golden": w["golden"], "every": w["every"]}
                                  for w in m["wheels"]],
                       "products": m["products"], "missions": m["missions"], "track": m["track"], "cfg": m["cfg"],
                       "rarities": m["rarities"]}, indent=1, sort_keys=True)


def main(argv):
    d, skins = load()
    m = build(d, skins)
    outs = {OUT_C: gen_c(m)}
    for dialect, path in OUT_SQL.items():
        outs[path] = gen_sql(m, dialect)
    if "--json" in argv:
        print(catalogue_json(m))
        return 0
    stale = []
    for path, text in outs.items():
        cur = None
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                cur = f.read()
        if cur == text:
            continue
        if "--check" in argv:
            stale.append(os.path.relpath(path, ROOT))
            continue
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)
        print("gen_economy: wrote %s" % os.path.relpath(path, ROOT))
    if stale:
        print("gen_economy: stale: %s (rerun python3 -I tools/strike/gen_economy.py)" % ", ".join(stale))
        return 1
    print("gen_economy: %d items, %d wheels, %d products, %d missions, %d track levels" % (
        len(m["items"]), len(m["wheels"]), len(m["products"]), len(m["missions"]), len(m["track"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
