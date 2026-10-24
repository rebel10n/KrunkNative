#pragma once
// Krunker v10.0.2 prize wheels ("spins"), shared by kn_brain (services/brain/store.c: the roll, the
// authority) and the client (the store's wheel UI).
//
// v10 has no wheel table in its client: store.wheels and store.itemPools come from gapi
// (`GET /data/spins`, v10.js:102788, applied with store.applySpinData). The live response of
// 2026-10-13 is pinned in tools/ui-import/v10_spins.jspck and compiled in as g_store_spins_json
// (shared/src/store_spins_table.c, written by tools/ui-import/gen_viewer_store.js). kn_brain can
// rotate the wheels without a release: a row in store_spin_data (migration 0118_store_v10) with
// the same JSON replaces the compiled-in set (store.c).
//
// A wheel draws by rarity (`rarities`: getSpnItems over the store table, this season's items) or,
// new in v10, from named item pools (`pools` + `poolWeights`: store.getSpnItems returns
// itemPools[pools[slice]].items). Tickets (TICKET items, type 13) redeem a wheel: their own
// `spinWheel` object, store.wheels[spinWheelId] (v10's Universal tickets) or the wheel named
// `spinWheelName`; a wheel's `ticketIndex` is the ticket that pays one spin of it instead of KR.
#include <stddef.h>
#include <stdint.h>

#define STORE_WHEEL_SLICES 8 // rarities (7) or pools per wheel

typedef struct StoreWheel {
    char name[32];
    char language[48];       // i18n key of the name
    char head_col[24], button_col[24], price_t[16], promo_text[32], promo_col[16];
    int price;               // KR per spin
    int multi;               // the "Nx Spin" count, 0 = single spins only
    int icons;               // img/shop/spins/<index>_<0..icons-1>.png
    int primary, free, kr, min_lvl, hide, locked;
    int ticket;              // ticketIndex: the TICKET item that pays a spin, -1 none
    int ticket_only;         // only through its ticket
    int big_rar;             // bigRar: a roll of this rarity or above draws from every rarity >= it
    int season;              // 0 = the store season (StoreSpins.season)
    int items_unique;        // itemsAreUnique: never an item the account owns
    unsigned dup_protected;  // duplicateProtectedPools: bit i = pool i never repeats an owned item
    int slices;              // entries of weights[]
    double weights[STORE_WHEEL_SLICES]; // rarities[] (pool_count 0) or poolWeights[]
    int pool_count;          // > 0: a pool wheel, slice i draws from pools[i]
    char pools[STORE_WHEEL_SLICES][40];
    char lim_type[24], lim_group[32], tag[24]; // limType / limGroup / tag (spinTag items)
    int64_t start_ms, end_ms, display_ms; // startDate / endDate / displayDate, 0 = none
    // aReq {ind, val, txt}: an account whose acc[ind] < val sees the wheel locked, priced txt
    // (the Prize Wheels tab, deob-10.0.3.js:145173-145179). a_req 0 = none.
    int a_req;
    char a_req_ind[24], a_req_txt[48];
    double a_req_val;
} StoreWheel;

typedef struct StorePool {
    char name[40];
    char label[32];          // the slice's label ("Contraband", "Featured Contra")
    char color[24];
    int count;
    int *items;              // store indices
} StorePool;

typedef struct StoreSpins {
    int season;              // config.currentSeason of the data (10)
    int wheel_count;
    StoreWheel *wheels;      // store.wheels order: the "spin" frame's wheel index, spinWheelId
    int pool_count;
    StorePool *pools;
} StoreSpins;

struct cJSON;

extern const char g_store_spins_json[];

// {season?, wheels, itemPools} JSON -> out (owned: store_spins_free). 0 ok, -1 not spin data.
int store_spins_parse(const char *json, StoreSpins *out);
void store_spins_free(StoreSpins *spins);
// One wheel object (store.wheels[i] or a TICKET item's spinWheel). 0 ok, -1 not an object.
int store_wheel_parse(const struct cJSON *obj, StoreWheel *wheel);
// g_store_spins_json, parsed on the first call (never NULL; empty when it does not parse)
const StoreSpins *store_spins_builtin(void);
const StorePool *store_spins_pool(const StoreSpins *spins, const char *name);
int store_spins_find(const StoreSpins *spins, const char *name); // wheel index by name, -1
// Spinnable at now_ms: not locked, after startDate, before endDate.
int store_wheel_open(const StoreWheel *wheel, int64_t now_ms);
double store_wheel_total(const StoreWheel *wheel);
// The slice a roll in [0, 100) lands on (past the sum of the odds: the last non-empty one).
int store_wheel_slice(const StoreWheel *wheel, double roll);
// "2026-01-07T10:00:00Z" (UTC, optional .fff) -> epoch ms, 0 when it does not parse.
int64_t store_iso_ms(const char *iso);
