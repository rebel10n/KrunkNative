// client/src/ui/market_trade_list.c (the Create Trade window's inventory side, v10_market.c ct_side): a search
// over a whole inventory (every store skin, 8069) is one pass; the window keeps every match and draws the rows
// near its scroll position (v10_market.c ct_inv_card), and the limit only caps what the helper writes to `out`.
//   cmake-build-debug/market_trade_list_test
#include <market_trade_list.h>
#include <viewer_store.h> // ViewerSkin (the store table entry)

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SKINS 8069 // store.skins (viewer_store_table.c)

static ViewerSkin g_skins[SKINS];
static char g_names[SKINS][40];
static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const ViewerSkin *skin_at(const int i) { return i >= 0 && i < SKINS ? &g_skins[i] : NULL; }

static int ci_has(const char *hay, const char *needle) { // a plain reference match for the expectations
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 32) == (needle[i] | 32)) i++;
        if (i == n) return 1;
    }
    return 0;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(void) {
    static const char *const kinds[4] = {"Rifle", "Sniper", "Plasma", "Knife"};
    for (int i = 0; i < SKINS; i++) {
        snprintf(g_names[i], sizeof(g_names[i]), "%s Skin %d", kinds[i % 4], i);
        g_skins[i].name = g_names[i];
        g_skins[i].rarity = i % 7;
        g_skins[i].seas = i % 5; // 0 = season 1
    }

    // this player's whole inventory: every skin, every other one a second copy; a few not tradable
    cJSON *inv = cJSON_CreateArray();
    for (int i = 0; i < SKINS; i++) {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddNumberToObject(row, "i", i);
        cJSON_AddNumberToObject(row, "c", 1 + i % 2);
        cJSON_AddNumberToObject(row, "nt", i % 11 == 0);
        cJSON_AddItemToArray(inv, row);
    }
    // the other player owns every third skin
    cJSON *other = cJSON_CreateArray();
    for (int i = 0; i < SKINS; i += 3) {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddNumberToObject(row, "i", i);
        cJSON_AddNumberToObject(row, "c", 1);
        cJSON_AddItemToArray(other, row);
    }

    enum { LIMIT = 60 };
    TradeListRow rows[SKINS];
    int total = -1;

    // no search: every row is a match, the window draws its first page only
    int drawn = market_trade_list_select(skin_at, inv, NULL, "", 0, LIMIT, rows, &total);
    CHECK(total == SKINS, "no search: every row is a match (total %d)", total);
    CHECK(drawn == LIMIT, "no search: a page is drawn (%d rows)", drawn);

    // a search over the whole inventory: the matches counted, the page drawn, every drawn row a match
    int expect = 0;
    for (int i = 0; i < SKINS; i++) expect += ci_has(g_names[i], "rifle");
    const double t0 = now_ms();
    drawn = market_trade_list_select(skin_at, inv, NULL, "rifle", 0, LIMIT, rows, &total);
    const double ms = now_ms() - t0;
    CHECK(total == expect, "search rifle: %d matches (expected %d)", total, expect);
    CHECK(drawn == LIMIT, "search rifle: only the page is drawn (%d rows)", drawn);
    int all_match = 1;
    for (int j = 0; j < drawn; j++) all_match &= ci_has(g_names[rows[j].skin], "rifle");
    CHECK(all_match, "search rifle: every drawn row is a rifle");
    printf("market_trade_list: search over %d rows drew %d in %.2f ms\n", SKINS, drawn, ms);

    // v10's order: rarity descending first (the top rarity of the matches leads), then season, then index
    int top_rarity = -1;
    for (int i = 0; i < SKINS; i++) if (ci_has(g_names[i], "rifle") && i % 7 > top_rarity) top_rarity = i % 7;
    CHECK(rows[0].skin >= 0 && g_skins[rows[0].skin].rarity == top_rarity, "the first row has the top rarity %d", top_rarity);
    int sorted = 1;
    for (int j = 1; j < drawn; j++) {
        const ViewerSkin *a = &g_skins[rows[j - 1].skin], *b = &g_skins[rows[j].skin];
        const int sa = a->seas ? a->seas : 1, sb = b->seas ? b->seas : 1;
        if (a->rarity < b->rarity) sorted = 0;
        else if (a->rarity == b->rarity && sa < sb) sorted = 0;
        else if (a->rarity == b->rarity && sa == sb && rows[j - 1].skin < rows[j].skin) sorted = 0;
    }
    CHECK(sorted, "the page is in v10's order");

    // the row's copies and "not tradable" flag come from the inventory row
    CHECK(rows[0].count == 1 + rows[0].skin % 2 && rows[0].locked == (rows[0].skin % 11 == 0), "copies and nt read per row");

    // "missing": the rows the other player already owns are left out
    int expect_missing = 0;
    for (int i = 0; i < SKINS; i++) expect_missing += i % 3 != 0;
    drawn = market_trade_list_select(skin_at, inv, other, "", 1, LIMIT, rows, &total);
    CHECK(total == expect_missing, "missing: %d rows the other player lacks (expected %d)", total, expect_missing);
    int none_owned = 1;
    for (int j = 0; j < drawn; j++) none_owned &= rows[j].skin % 3 != 0;
    CHECK(none_owned && drawn == LIMIT, "missing: a drawn row is one the other player lacks");

    // a search that names one skin in full, and a longer search that contains a skin's name
    drawn = market_trade_list_select(skin_at, inv, NULL, "plasma skin 2 x", 0, LIMIT, rows, &total);
    CHECK(total == 1 && drawn == 1 && rows[0].skin == 2, "a name inside the search matches (%d rows)", total);
    drawn = market_trade_list_select(skin_at, inv, NULL, "zzz-no-such-skin", 0, LIMIT, rows, &total);
    CHECK(total == 0 && drawn == 0, "no match: nothing drawn");

    // the page never exceeds its limit, and a limit above the matches draws them all
    for (int limit = 1; limit <= 500; limit *= 5) {
        drawn = market_trade_list_select(skin_at, inv, NULL, "knife", 0, limit, rows, &total);
        CHECK(drawn <= limit && drawn == (total < limit ? total : limit), "limit %d: %d drawn of %d", limit, drawn, total);
    }

    // the empty inputs draw nothing and count nothing
    total = -1;
    drawn = market_trade_list_select(skin_at, NULL, NULL, "", 0, LIMIT, rows, &total);
    CHECK(drawn == 0 && total == 0, "no inventory: nothing drawn");
    drawn = market_trade_list_select(skin_at, inv, NULL, "", 0, 0, rows, &total);
    CHECK(drawn == 0, "limit 0 draws nothing");

    cJSON_Delete(inv);
    cJSON_Delete(other);
    printf("market_trade_list_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
