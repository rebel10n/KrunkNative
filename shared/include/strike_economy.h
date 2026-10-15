#pragma once
// Krunker Strike's economy and progression as data: the item catalogue (skins, rarities, the weapons
// they apply to), the prize wheels (types, odds, costs), the store, the wallet currencies
// (krunkies), the account XP track and its rewards, the daily missions, and the pure rules over them
// (levels, rolls, guaranteed pools, the golden spin meter, daily picks).
//
// Sources (doc/v10/krunker-strike.md, "Economy"): FRVR's Krunker Strike 1.37.2 bundle, read as text
// by tools/strike/extract_strike.py into tools/strike/strike_data.json (sections "items" and
// "economy"): the item / weapon / rarity enums, the wallet currencies, the crate tiers' rarity odds
// (Starter / Elite / Heroic), the rarity colours, the KR packages, the account XP curve, the golden
// spin meter (10), the reward past the track's last level, the mission keys and the gateway's zod
// shapes. FRVR serves the catalogue itself from its gateway (/v1/item-definitions/all,
// /v2/spin-wheels/*, /v1/shop, /v1/missions/me, /v1/game-config): its item definitions name
// Krunker store skins (`id`, `weapon`, `rarity`), so KrunkNative's catalogue is the Krunker store
// skins (assets/skins.json) of Strike's six class weapons, melee and cosmetics of seasons 1-2.
// Prices, the track's rewards and the mission list are server data FRVR never shipped in the
// client: tools/strike/gen_economy.py documents each KrunkNative choice next to its value.
//
// tools/strike/gen_economy.py writes shared/src/rules/strike/strike_economy_data.c (these tables)
// and services/db/migrations/*/0320_strike_economy.sql (the same catalogue as seed rows: kn_brain
// serves Strike's catalogue from those tables, services/brain/strike_econ.c). Regenerate, never
// hand-edit.
//
// Pure C; part of the shared core (no GL / net). Linked by the client (the Strike screens' data:
// client/include/strike_econ.h), kn_brain (rolls and levels) and shared/tests/strike_economy_test.c.
#include <stddef.h>
#include <stdint.h>

// ---- enums (the bundle's own numbering) ------------------------------------------------------

// item.type ($O: Weapon 0 .. Class 15): Krunker's store item types
enum {
    STRIKE_ITEM_WEAPON = 0, STRIKE_ITEM_HAT = 1, STRIKE_ITEM_BACK = 2, STRIKE_ITEM_MELEE = 3, STRIKE_ITEM_SPRAY = 4,
    STRIKE_ITEM_DYE = 5, STRIKE_ITEM_WAIST = 6, STRIKE_ITEM_FACE = 7, STRIKE_ITEM_SHOE = 8, STRIKE_ITEM_PET = 9,
    STRIKE_ITEM_COLLECTIBLE = 10, STRIKE_ITEM_WRIST = 11, STRIKE_ITEM_CHARM = 12, STRIKE_ITEM_TICKET = 13,
    STRIKE_ITEM_CHEST = 14, STRIKE_ITEM_CLASS = 15, STRIKE_ITEM_TYPE_COUNT = 16
};

// item.rarity (tU: Uncommon 0 .. Nft 7)
enum { STRIKE_RARITY_COUNT = 8 };

// wallet currencies (Gt: xp, kr, spin_ticket, ...); the index is KrunkNative's
typedef enum {
    STRIKE_CUR_XP = 0,
    STRIKE_CUR_KR,             // "kr": krunkies
    STRIKE_CUR_SPIN_TICKET,
    STRIKE_CUR_GOLDEN_TICKET,  // "spin_golden_ticket"
    STRIKE_CUR_NAME_CHANGE,
    STRIKE_CUR_DAILY_KR,       // "daily_kr_gained": KR earned in matches today
    STRIKE_CUR_EVENT_TOKENS,   // "seasonal_event_tokens"
    STRIKE_CUR_COUNT
} StrikeCurrency;

// ---- tables ------------------------------------------------------------------------------------

typedef struct {
    const char *name;    // "Uncommon"
    const char *color;   // "#b2f252" (items.rarities[].color)
    const char *i18n;    // "windows.spin.rarity.uncommon"
    int sell;            // Krunker's quick-sell KR (items.rarities[].sell)
    int deal_price;      // the store's daily deal price in KR (KrunkNative; FRVR's shop is server data)
} StrikeRarity;

// One catalogue entry: FRVR's item definition {_id, type, weapon, name, rarity, id, default}.
typedef struct {
    const char *id;      // "_id": "s<skin>" store skin, "c<class>" class, "d<type>_<weapon>" default
    int type;            // STRIKE_ITEM_*
    int weapon;          // weapon enum (eU, = Krunker weapon id - 1) for weapon skins / defaults, else -1
    const char *name;
    int rarity;          // 0..7, -1 for a default item
    int skin;            // Krunker store.skins index (assets/skins.json), -1 = the plain look
    int class_id;        // STRIKE_ITEM_CLASS: Krunker class id, else -1
    unsigned char is_default;
    unsigned char spinnable; // in its rarity's wheel pool
    const char *origin;  // unlock origin: "spin_tickets", "rewards_track", "bundle", NULL
} StrikeItem;

typedef struct {
    const char *id;      // "uncommon" .. "unobtainable"
    const char *name;    // poolName
    const char *color;
    int rarity;
    const int *items;    // catalogue indices (strike_economy()->items)
    int count;
} StrikePool;

typedef struct {
    const char *pool;    // pool id
    double weight;       // fraction of the wheel (FRVR: weight * 100 = percent)
    int guaranteed_if;   // guaranteedIfSpinCountIs: a spin of that many lands one in this pool or better, 0 none
} StrikeWheelSlice;

typedef struct {
    const char *id;           // "_id"
    const char *name, *name_key;
    const char *type;         // Di: "default", "free", "golden_spin", ...
    const char *price_currency; // NULL: no price (free wheel)
    int price_amount;
    int time_between_s;       // free wheels: one spin per this many seconds
    int max_accumulated;      // ... banking at most this many
    double heroic_trigger_chance; // heroicSpinTriggerChance: per spin, a golden ticket on the side
    int hide_in_shop;
    int counts_for_golden;    // a spin fills the golden spin meter
    const char *button_color, *category;
    const StrikeWheelSlice *slices;
    int slice_count;
} StrikeWheel;

// A reward (FRVR's h0 union): currency {currency, amount} | item {item} | class {item, class_id} | feature
typedef struct {
    const char *type;     // "currency", "item", "class", "feature"
    const char *currency; // currency key ("kr", "spin_ticket", ...)
    int amount;
    const char *item;     // item id
    int class_id;
    const char *feature;  // "missions", ...
} StrikeReward;

typedef struct {
    const char *id;
    const char *section;  // store.section.*: "spins", "tickets", "bundles", "free", "deals"
    const char *title;
    const char *price_type; // "virtual_currency", "free" (KrunkNative takes no real money)
    const char *price_currency;
    int price_amount;
    const StrikeReward *rewards;
    int reward_count;
    int refresh_s;        // purchases count per this many seconds (0: forever)
    int max_purchases;    // per refresh window, 0 unlimited
} StrikeProduct;

typedef struct {
    const char *id;
    const char *key;      // the game's mission key (missions.update(game, key, ...)): "k", "hs", "w", ...
    const char *text_key; // i18n "chall.name.<key>"
    int required;         // requiredValue
    int max_mode;         // 1: progress is the best single value ("kstk"), 0: a sum
    int weight;           // daily pick weight
    const StrikeReward *rewards;
    int reward_count;
} StrikeMission;

typedef struct {
    int level;
    const char *slot_id;  // "<level>_00"
    int player_choice;    // isPlayerChoice: claim one of rewards[] (POST {slotId, choice})
    const StrikeReward *rewards;
    int reward_count;
} StrikeTrackSlot;

typedef struct StrikeEconomy_t {
    const char *game_version;          // "1.37.2"
    const char *const *currency_keys;  // StrikeCurrency -> "xp", "kr", ...
    const StrikeRarity *rarities;      // STRIKE_RARITY_COUNT
    const char *const *item_type_names; // STRIKE_ITEM_TYPE_COUNT ("Weapon", "Hat", ...)
    const char *const *weapon_names;   // weapon enum -> "Assault Rifle"
    int weapon_count;
    const StrikeItem *items;
    int item_count;
    const StrikePool *pools;
    int pool_count;
    const StrikeWheel *wheels;
    int wheel_count;
    const StrikeProduct *products;
    int product_count;
    const StrikeMission *missions;
    int mission_count;
    const StrikeTrackSlot *track;
    int track_count;
    // rules
    int xp_base, xp_step;              // level L costs xp_base + xp_step * floor(L / xp_divisor)
    double xp_divisor;
    int max_level;                     // the track's last level (maxLevelCap)
    StrikeReward past_max_level_reward; // per level past max_level (1 spin ticket)
    int golden_spin_meter;             // spinsToGetGoldenSpin
    int missions_unlock_level;
    int daily_mission_count;
    int deal_count;                    // daily deals in the store
    const int *packages_kr;            // KR Package amounts by rarity (items.packages)
    int package_count;
    const int *starter_classes;        // classes every account owns
    int starter_class_count;
} StrikeEconomy;

// The generated data (strike_economy_data.c). Never NULL.
const StrikeEconomy *strike_economy(void);

// ---- lookups (NULL / -1 when absent) ------------------------------------------------------------
const char *strike_currency_key(StrikeCurrency c);
StrikeCurrency strike_currency_from_key(const char *key); // STRIKE_CUR_COUNT when unknown
int strike_item_index(const char *id);
const StrikeItem *strike_item(const char *id);
const StrikeItem *strike_item_by_skin(int skin);
const StrikeItem *strike_default_item(int type, int weapon); // the default (plain) item of a slot
const StrikePool *strike_pool(const char *id);
const StrikeWheel *strike_wheel(const char *id);
const StrikeProduct *strike_product(const char *id);
const StrikeMission *strike_mission(const char *id);
const StrikeTrackSlot *strike_track_slot(const char *slot_id);
const StrikeTrackSlot *strike_track_level(int level);
const StrikeRarity *strike_rarity(int rarity);

// ---- the loadout ---------------------------------------------------------------------------------
// The slot an item equips into: "w<weapon>" for weapon skins (per weapon), "t<type>" for the other
// equippable types (hat, back, melee, spray, dye, waist, face, shoe, pet, wrist, charm); 0 when the
// item does not equip (classes, tickets, collectibles). out: >= 8 bytes.
int strike_item_slot(const StrikeItem *item, char *out, size_t size);

// ---- levels (the bundle's LA(xp)) ---------------------------------------------------------------
typedef struct {
    int level;           // 1-based
    int64_t xp_in_level; // xpInCurrentLevel
    int64_t xp_to_next;  // xpToNextLevel (the cost of this level)
    int64_t total_needed; // totalXpNeeded: XP at which the next level starts
} StrikeLevel;
StrikeLevel strike_level_for_xp(int64_t xp);
int64_t strike_xp_for_level(int level); // XP at which `level` starts (level 1: 0)

// ---- rolls ---------------------------------------------------------------------------------------
// The slice a uniform roll in [0, 1) lands on: slices in table order, weights normalised to their
// sum, zero / empty slices skipped. -1 when the wheel has no weight.
int strike_wheel_slice_for_roll(const double *weights, int count, double roll);
// One spin of `count` rolls on `wheel` (count 1..10): rolls[i] in [0, 1) pick the slice, picks[i] in
// [0, 1) the item in its pool. out_items[i] = catalogue index, out_slices[i] = slice. A slice with
// guaranteed_if == count is guaranteed: when no roll landed on it or a rarer slice, the last roll is
// replaced by one in it (picked with picks[count - 1]). Returns the number of results (0 on bad input).
int strike_wheel_spin(const StrikeWheel *wheel, int count, const double *rolls, const double *picks,
                      int *out_items, int *out_slices);
// The golden spin meter after `spins` paid spins: *count += spins; every full meter (golden_spin_meter)
// is one golden ticket. Returns the tickets earned, *count keeps the remainder.
int strike_golden_meter_add(int *count, int spins);

// ---- daily picks (deterministic: the brain and a client agree) ----------------------------------
// The day number of an epoch-ms time (UTC days since 1970).
int strike_day(int64_t epoch_ms);
// Today's daily missions of an account: `n` distinct mission indices (weighted, seeded by day and
// account). Returns the count written.
int strike_daily_missions(int day, int64_t account_id, int *out, int n);
// Today's store deals: `n` distinct catalogue indices among the spinnable weapon skins of rarity
// 1..4 (seeded by day; the same for every account). Returns the count written.
int strike_daily_deals(int day, int *out, int n);
// FNV-1a 64 over a string (seeds, stable ids)
uint64_t strike_hash(const char *s, uint64_t seed);
