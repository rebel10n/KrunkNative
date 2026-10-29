// krunker.io trades: the offer, the create body and the paths (kio_trade.h). Pure: no network, no UI.
#include <kio_trade.h>
#include <stdio.h>
#include <string.h>

cJSON *kio_trade_offer(const EconTradeItem *items, const int n, int *copies) {
    cJSON *offer = cJSON_CreateArray();
    for (int k = 0; k < n; k++) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "i", items[k].i);
        if (items[k].sid > 0) { // a painted copy: c 1 and its id in iids (deob-10.0.4.js:111287-111297)
            cJSON_AddNumberToObject(u, "c", 1);
            cJSON *ids = cJSON_CreateArray();
            cJSON_AddItemToArray(ids, cJSON_CreateNumber(items[k].sid));
            cJSON_AddItemToObject(u, "iids", ids);
            if (copies) (*copies)++;
        } else {
            cJSON_AddNumberToObject(u, "c", items[k].c);
        }
        cJSON_AddItemToArray(offer, u);
    }
    return offer;
}

cJSON *kio_trade_inventory(const cJSON *official) {
    cJSON *rows = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, official) {
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(r, "i");
        if (!cJSON_IsNumber(i)) continue;
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(r, "st"), *sid = cJSON_GetObjectItemCaseSensitive(r, "sid");
        const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(r, "cnt"), *c = cJSON_GetObjectItemCaseSensitive(r, "c");
        const cJSON *nt = cJSON_GetObjectItemCaseSensitive(r, "nt"), *m = cJSON_GetObjectItemCaseSensitive(r, "m");
        cJSON *row = cJSON_CreateObject();
        cJSON_AddNumberToObject(row, "i", i->valuedouble);
        if (cJSON_IsNumber(st) && st->valueint == 1) { // iIîííïï: a stack, cnt || c (deob-10.0.4.js:111228)
            cJSON_AddNumberToObject(row, "c", cJSON_IsNumber(cnt) ? cnt->valuedouble : cJSON_IsNumber(c) ? c->valuedouble : 0);
        } else { // one copy, offered once as {i, c: 1, iids: [sid]} (:111258-111262)
            cJSON_AddNumberToObject(row, "c", 1);
            if (cJSON_IsNumber(sid)) cJSON_AddNumberToObject(row, "sid", sid->valuedouble);
            if (m && !cJSON_IsNull(m)) cJSON_AddItemToObject(row, "m", cJSON_Duplicate(m, 1));
        }
        if (cJSON_IsNumber(nt) || cJSON_IsBool(nt)) cJSON_AddItemToObject(row, "nt", cJSON_Duplicate(nt, 1));
        cJSON_AddItemToArray(rows, row);
    }
    return rows;
}

cJSON *kio_trade_create_body(const int target_player_id, cJSON *my_offer, cJSON *their_offer, const char *message) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "target_player_id", target_player_id);
    cJSON_AddItemToObject(body, "my_offer", my_offer ? my_offer : cJSON_CreateArray());
    cJSON_AddItemToObject(body, "their_offer", their_offer ? their_offer : cJSON_CreateArray());
    cJSON_AddStringToObject(body, "message", message ? message : "");
    return body;
}

void kio_trade_path(char *out, const size_t n, const char *action, const int id) {
    if (!strcmp(action, "inventory")) snprintf(out, n, "trades/inventory/%d", id);
    else snprintf(out, n, "trades/%d/%s", id, action);
}

// One side of an official row: its skins are the {ind} entries of skins (the trade window's yourSkins / theirSkins,
// deob-10.0.4.js:136939-136940; each tile reads its item from ind, :133156), its currency the {cid, amt} entries of
// currency (yoursCurrency / theirsCurrency, :136941-136942). cid 1 is KR, 2 JNK, 3 RP (the currency names, :133113-133117,
// shown as "+amt NAME" at :133246). A painted copy's paint is not in the row, so a copy is a plain unit here.
// A cid outside 1-3 is not something this client shows: other_cur is set for it.
static void side_units(cJSON *row, const cJSON *official, const char *skins, const char *currency,
                       const char *skins_out, const char *kr_out, const char *jn_out, const char *rp_out, int *other_cur) {
    char units[4096] = "";
    size_t len = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(official, skins)) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind");
        if (!cJSON_IsNumber(ind) || len + 16 >= sizeof(units)) continue;
        len += (size_t) snprintf(units + len, sizeof(units) - len, "%d,", ind->valueint);
    }
    double cur[3] = {0, 0, 0}; // KR, JNK, RP by cid
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(official, currency)) {
        const cJSON *cid = cJSON_GetObjectItemCaseSensitive(e, "cid"), *amt = cJSON_GetObjectItemCaseSensitive(e, "amt");
        if (!cJSON_IsNumber(cid) || cid->valueint < 1 || cid->valueint > 3) {
            *other_cur = 1;
            continue;
        }
        if (cJSON_IsNumber(amt)) cur[cid->valueint - 1] += amt->valuedouble;
    }
    cJSON_AddStringToObject(row, skins_out, units);
    cJSON_AddNumberToObject(row, kr_out, cur[0]);
    cJSON_AddNumberToObject(row, jn_out, cur[1]);
    cJSON_AddNumberToObject(row, rp_out, cur[2]);
}

cJSON *kio_trade_row(const cJSON *official) {
    cJSON *row = cJSON_CreateObject();
    static const char *const kept[] = {"trid", "pl", "player", "buyer", "message", "dte"};
    for (size_t k = 0; k < sizeof(kept) / sizeof(kept[0]); k++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(official, kept[k]);
        if (v) cJSON_AddItemToObject(row, kept[k], cJSON_Duplicate(v, 1));
    }
    int other_cur = 0;
    side_units(row, official, "sksell", "csell", "sksell", "krsell", "jnsell", "rpsell", &other_cur);
    side_units(row, official, "skbuy", "cbuy", "skbuy", "krbuy", "jnbuy", "rpbuy", &other_cur);
    cJSON_AddNumberToObject(row, "other_cur", other_cur);
    return row;
}
