// kio_trade: the offer serializer, the trade inventory rows, the create body and the trade paths (client/src/ui/kio_trade.c).
#include <kio_trade.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            failures++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

int main(void) {
    // stacks: {i, c}; a copy: {i, c: 1, iids: [sid]} and counted (deob-10.0.4.js:111287-111297)
    const EconTradeItem items[3] = {{.i = 3, .c = 2, .sid = 0}, {.i = 4, .c = 1, .sid = 7}, {.i = 9, .c = 5, .sid = 0}};
    int copies = 0;
    cJSON *offer = kio_trade_offer(items, 3, &copies);
    CHECK(cJSON_IsArray(offer) && cJSON_GetArraySize(offer) == 3, "offer has one row per unit");
    CHECK(copies == 1, "one copy counted (%d)", copies);
    const cJSON *a = cJSON_GetArrayItem(offer, 0), *b = cJSON_GetArrayItem(offer, 1), *c = cJSON_GetArrayItem(offer, 2);
    CHECK(cJSON_GetObjectItem(a, "i")->valueint == 3 && cJSON_GetObjectItem(a, "c")->valueint == 2
          && !cJSON_GetObjectItem(a, "iids"), "a stack is {i, c} with no iids");
    CHECK(cJSON_GetObjectItem(b, "i")->valueint == 4 && cJSON_GetObjectItem(b, "c")->valueint == 1,
          "a copy is c 1");
    const cJSON *iids = cJSON_GetObjectItem(b, "iids");
    CHECK(cJSON_IsArray(iids) && cJSON_GetArraySize(iids) == 1 && cJSON_GetArrayItem(iids, 0)->valueint == 7,
          "a copy carries its sid in iids");
    CHECK(cJSON_GetObjectItem(c, "c")->valueint == 5, "a stack keeps its count");
    cJSON_Delete(offer);

    int none = 0;
    cJSON *empty = kio_trade_offer(items, 0, &none);
    CHECK(cJSON_IsArray(empty) && cJSON_GetArraySize(empty) == 0 && none == 0, "no units, no rows");
    cJSON_Delete(empty);

    // the create body: target_player_id, my_offer, their_offer, message (deob-10.0.4.js:111326-111331)
    cJSON *body = kio_trade_create_body(42, kio_trade_offer(items, 1, NULL), NULL, "hi");
    CHECK(cJSON_GetObjectItem(body, "target_player_id")->valueint == 42, "the target id");
    CHECK(cJSON_IsArray(cJSON_GetObjectItem(body, "my_offer")) && cJSON_GetArraySize(cJSON_GetObjectItem(body, "my_offer")) == 1,
          "my_offer is the offer");
    CHECK(cJSON_IsArray(cJSON_GetObjectItem(body, "their_offer")) && cJSON_GetArraySize(cJSON_GetObjectItem(body, "their_offer")) == 0,
          "a missing their_offer is an empty array");
    CHECK(!strcmp(cJSON_GetObjectItem(body, "message")->valuestring, "hi"), "the message");
    cJSON_Delete(body);

    // an official row (trades/history, deob-10.0.4.js:27155) as the native row: skins {ind} -> units, KR from the
    // cid 1 currency entries, other_cur for JNK / RP (deob-10.0.4.js:136939-136942, :133246)
    cJSON *official = cJSON_Parse("{\"trid\":77,\"pl\":5,\"player\":\"Ann\",\"buyer\":\"Bo\",\"message\":\"hi\","
                                  "\"dte\":\"2026-10-28T00:00:00Z\",\"sksell\":[{\"ind\":3},{\"ind\":9}],"
                                  "\"skbuy\":[],\"csell\":[{\"cid\":1,\"amt\":250},{\"cid\":1,\"amt\":50}],"
                                  "\"cbuy\":[{\"cid\":2,\"amt\":5}]}");
    cJSON *row = kio_trade_row(official);
    CHECK(cJSON_GetObjectItem(row, "trid")->valueint == 77 && cJSON_GetObjectItem(row, "pl")->valueint == 5,
          "trid and pl keep their names");
    CHECK(!strcmp(cJSON_GetObjectItem(row, "player")->valuestring, "Ann") && !strcmp(cJSON_GetObjectItem(row, "buyer")->valuestring, "Bo"),
          "player and buyer keep their names");
    CHECK(!strcmp(cJSON_GetObjectItem(row, "message")->valuestring, "hi"), "message keeps its name");
    CHECK(!strcmp(cJSON_GetObjectItem(row, "sksell")->valuestring, "3,9,"), "sksell is the stack units (%s)",
          cJSON_GetObjectItem(row, "sksell")->valuestring);
    CHECK(!strcmp(cJSON_GetObjectItem(row, "skbuy")->valuestring, ""), "an empty side is an empty string");
    CHECK(cJSON_GetObjectItem(row, "krsell")->valuedouble == 300, "krsell sums the KR entries (%g)",
          cJSON_GetObjectItem(row, "krsell")->valuedouble);
    CHECK(cJSON_GetObjectItem(row, "krbuy")->valuedouble == 0, "krbuy has no KR");
    CHECK(cJSON_GetObjectItem(row, "jnbuy")->valuedouble == 5, "a JNK entry is its jnbuy amount");
    CHECK(cJSON_GetObjectItem(row, "other_cur")->valueint == 0, "a JNK entry does not set other_cur");
    cJSON_Delete(row);
    cJSON *plain = cJSON_Parse("{\"trid\":8,\"pl\":1,\"sksell\":[],\"skbuy\":[{\"ind\":4}],\"csell\":[],\"cbuy\":[{\"cid\":1,\"amt\":20}]}");
    cJSON *plain_row = kio_trade_row(plain);
    CHECK(cJSON_GetObjectItem(plain_row, "other_cur")->valueint == 0 && cJSON_GetObjectItem(plain_row, "krbuy")->valuedouble == 20
          && !strcmp(cJSON_GetObjectItem(plain_row, "skbuy")->valuestring, "4,"), "a KR-only row has other_cur 0");
    cJSON_Delete(plain_row);
    cJSON_Delete(plain);
    cJSON_Delete(official);

    // JNK (cid 2) and RP (cid 3) are shown, so they carry their amounts and do not set other_cur
    cJSON *cur = cJSON_Parse("{\"trid\":9,\"pl\":2,\"sksell\":[],\"skbuy\":[],\"csell\":[{\"cid\":2,\"amt\":40},{\"cid\":2,\"amt\":2}],"
                             "\"cbuy\":[{\"cid\":3,\"amt\":7}]}");
    cJSON *cur_row = kio_trade_row(cur);
    CHECK(cJSON_GetObjectItem(cur_row, "jnsell")->valuedouble == 42 && cJSON_GetObjectItem(cur_row, "rpbuy")->valuedouble == 7,
          "JNK and RP amounts are carried (jnsell %g, rpbuy %g)", cJSON_GetObjectItem(cur_row, "jnsell")->valuedouble,
          cJSON_GetObjectItem(cur_row, "rpbuy")->valuedouble);
    CHECK(cJSON_GetObjectItem(cur_row, "krsell")->valuedouble == 0 && cJSON_GetObjectItem(cur_row, "jnbuy")->valuedouble == 0
          && cJSON_GetObjectItem(cur_row, "rpsell")->valuedouble == 0, "the other currencies are zero where absent");
    CHECK(cJSON_GetObjectItem(cur_row, "other_cur")->valueint == 0, "JNK and RP do not set other_cur");
    cJSON_Delete(cur_row);
    cJSON_Delete(cur);

    // a currency id outside 1-3 is not shown: other_cur is set
    cJSON *unknown = cJSON_Parse("{\"trid\":10,\"pl\":2,\"sksell\":[],\"skbuy\":[],\"csell\":[{\"cid\":4,\"amt\":1}],\"cbuy\":[]}");
    cJSON *unknown_row = kio_trade_row(unknown);
    CHECK(cJSON_GetObjectItem(unknown_row, "other_cur")->valueint == 1, "a cid outside 1-3 sets other_cur");
    cJSON_Delete(unknown_row);
    cJSON_Delete(unknown);

    char path[64];
    kio_trade_path(path, sizeof(path), "accept", 12);
    CHECK(!strcmp(path, "trades/12/accept"), "accept path (%s)", path);
    kio_trade_path(path, sizeof(path), "cancel", 12);
    CHECK(!strcmp(path, "trades/12/cancel"), "cancel path (%s)", path);
    kio_trade_path(path, sizeof(path), "inventory", 42);
    CHECK(!strcmp(path, "trades/inventory/42"), "inventory path (%s)", path);

    // the official trade inventory rows (deob-10.0.4.js:111208-111262): a stack (st 1) is {i, c: cnt} with no sid,
    // a copy {i, c: 1, sid, m}; the copy's sid is the official instance id the offer sends as iids
    cJSON *official_inv = cJSON_Parse("[{\"i\":3,\"st\":1,\"sid\":900,\"cnt\":4},{\"i\":3,\"st\":0,\"sid\":5501,\"m\":{\"paint\":\"ff0000\"}},"
                                  "{\"i\":8,\"st\":1,\"sid\":901,\"c\":2,\"nt\":1},{\"st\":1,\"cnt\":3}]");
    cJSON *rows = kio_trade_inventory(official_inv);
    CHECK(cJSON_GetArraySize(rows) == 3, "a row without an item index is dropped (%d rows)", cJSON_GetArraySize(rows));
    const cJSON *stack = cJSON_GetArrayItem(rows, 0), *copy = cJSON_GetArrayItem(rows, 1), *other = cJSON_GetArrayItem(rows, 2);
    CHECK(cJSON_GetObjectItem(stack, "c")->valueint == 4 && !cJSON_GetObjectItem(stack, "sid"), "a stack takes cnt and loses its sid");
    CHECK(cJSON_GetObjectItem(copy, "c")->valueint == 1 && cJSON_GetObjectItem(copy, "sid")->valueint == 5501, "a copy is one unit with its instance id");
    CHECK(cJSON_IsObject(cJSON_GetObjectItem(copy, "m")), "a copy keeps its metadata (its paint)");
    CHECK(cJSON_GetObjectItem(other, "c")->valueint == 2 && cJSON_GetObjectItem(other, "nt")->valueint == 1, "a stack with c and nt keeps both");
    const EconTradeItem picked[1] = {{.i = 3, .c = 1, .sid = 5501}};
    cJSON *painted = kio_trade_offer(picked, 1, NULL);
    const cJSON *pids = cJSON_GetObjectItem(cJSON_GetArrayItem(painted, 0), "iids");
    CHECK(cJSON_GetArrayItem(pids, 0)->valueint == 5501, "a picked copy is offered by its instance id");
    cJSON_Delete(painted);
    cJSON_Delete(rows);
    cJSON_Delete(official_inv);
    cJSON *none_rows = kio_trade_inventory(NULL);
    CHECK(cJSON_IsArray(none_rows) && cJSON_GetArraySize(none_rows) == 0, "no inventory, no rows");
    cJSON_Delete(none_rows);

    if (failures) {
        printf("kio_trade_test: %d failures\n", failures);
        return 1;
    }
    printf("kio_trade_test: all checks passed\n");
    return 0;
}
