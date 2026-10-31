// social_list: the hub's friends / conversations list (client/src/ui/social_list.c; the event bus store,
// deob-10.0.6.js:141245-141372, 141567-141575).
#include <social_list.h>
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

static double num(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

static const char *str(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

int main(void) {
    // friends:list offset > 0: a page's friends join unless their id is there (:141252-141265)
    cJSON *list = cJSON_Parse("[{\"id\":1,\"name\":\"a\",\"unread\":2},{\"id\":2,\"name\":\"b\"}]");
    cJSON *page = cJSON_Parse("[{\"id\":2,\"name\":\"b again\"},{\"id\":3,\"name\":\"c\"},{\"id\":3,\"name\":\"c twice\"},{\"id\":4,\"name\":\"d\"}]");
    CHECK(social_friends_merge(list, page) == 2, "two new friends");
    CHECK(cJSON_GetArraySize(list) == 4, "the list has 4 (%d)", cJSON_GetArraySize(list));
    CHECK(!strcmp(str(cJSON_GetArrayItem(list, 1), "name"), "b"), "a duplicate keeps the entry there");
    CHECK(num(cJSON_GetArrayItem(list, 2), "id") == 3 && !strcmp(str(cJSON_GetArrayItem(list, 2), "name"), "c"),
          "the first of a page's duplicates is the one pushed");
    CHECK(num(cJSON_GetArrayItem(list, 3), "id") == 4, "the new ones are the tail, in order");
    CHECK(social_friends_merge(list, page) == 0 && cJSON_GetArraySize(list) == 4, "the same page again adds nothing");
    CHECK(social_friends_merge(list, NULL) == 0, "no page: 0");
    cJSON_Delete(page);

    // msg:recv from a friend: the entry takes the message as its last one (:141358-141362)
    cJSON *f = social_dm_received(list, 1, NULL, 3, 501, "hi", 1700.0);
    CHECK(f == cJSON_GetArrayItem(list, 0), "the friend's own entry");
    CHECK(num(f, "unread") == 3 && num(f, "lastId") == 501 && !strcmp(str(f, "lastText"), "hi") && num(f, "lastTs") == 1700.0,
          "unread, lastId, lastText, lastTs");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(f, "lastFromMe")), "lastFromMe false");
    CHECK(!strcmp(str(f, "name"), "a"), "a friend's name stays");

    // a sender not in the list joins at the top as {id, name, online: true} (:141346-141356)
    f = social_dm_received(list, 77, "stranger", 1, 502, "yo", 1800.0);
    CHECK(f && f == cJSON_GetArrayItem(list, 0), "added at the top");
    CHECK(cJSON_GetArraySize(list) == 5, "the list has 5");
    CHECK(num(f, "id") == 77 && !strcmp(str(f, "name"), "stranger") && cJSON_IsTrue(cJSON_GetObjectItem(f, "online")),
          "{id, name, online: true}");
    CHECK(num(f, "unread") == 1 && num(f, "lastId") == 502 && !strcmp(str(f, "lastText"), "yo")
          && cJSON_IsFalse(cJSON_GetObjectItem(f, "lastFromMe")), "with the message as its last one");
    f = social_dm_received(list, 77, NULL, 2, 503, "again", 1900.0);
    CHECK(f == cJSON_GetArrayItem(list, 0) && cJSON_GetArraySize(list) == 5 && num(f, "unread") == 2, "the next one finds it");

    // with no name known nothing is added
    CHECK(social_dm_received(list, 88, NULL, 1, 504, "?", 2000.0) == NULL, "no name: NULL");
    CHECK(social_dm_received(list, 88, "", 1, 504, "?", 2000.0) == NULL, "empty name: NULL");
    CHECK(cJSON_GetArraySize(list) == 5, "nothing added");
    CHECK(social_dm_received(NULL, 1, "a", 1, 1, "x", 0) == NULL, "no list: NULL");

    // opening a conversation: unread 0 (:141567-141575)
    f = social_conversation_read(list, 77);
    CHECK(f && num(f, "unread") == 0, "the opened conversation reads 0");
    f = social_conversation_read(list, 2);
    CHECK(f && num(f, "unread") == 0, "an entry without unread gets 0");
    CHECK(social_conversation_read(list, 999) == NULL, "a peer without an entry: NULL");
    CHECK(num(cJSON_GetArrayItem(list, 1), "unread") == 3, "the others keep theirs");
    cJSON_Delete(list);

    if (failures) {
        printf("social_list_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("social_list_test: ok\n");
    return 0;
}
