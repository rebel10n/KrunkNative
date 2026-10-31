// The hub's friends / conversations list (client/include/social_list.h; deob-10.0.6.js:141245-141372).
#include <social_list.h>

static int64_t id_of(const cJSON *f) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(f, "id");
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : 0;
}

static cJSON *find(const cJSON *list, const int64_t id) {
    cJSON *f;
    cJSON_ArrayForEach(f, list) if (id_of(f) == id) return f;
    return NULL;
}

static void set_num(cJSON *o, const char *key, const double v) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    cJSON_AddNumberToObject(o, key, v);
}

int social_friends_merge(cJSON *list, const cJSON *page) {
    if (!cJSON_IsArray(list) || !cJSON_IsArray(page)) return 0;
    int added = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, page) {
        if (find(list, id_of(f))) continue; // some(id === f.id) || push(f)
        cJSON *copy = cJSON_Duplicate(f, 1);
        if (copy && cJSON_AddItemToArray(list, copy)) added++;
        else cJSON_Delete(copy);
    }
    return added;
}

cJSON *social_dm_received(cJSON *list, const int64_t from, const char *name, const int unread, const int64_t id, const char *text,
                          const double ts) {
    if (!cJSON_IsArray(list) || !from) return NULL;
    cJSON *f = find(list, from);
    if (!f) {
        if (!name || !name[0]) return NULL; // the name map has no entry: return
        f = cJSON_CreateObject();
        if (!f) return NULL;
        cJSON_AddNumberToObject(f, "id", (double) from);
        cJSON_AddStringToObject(f, "name", name);
        cJSON_AddTrueToObject(f, "online");
        cJSON_InsertItemInArray(list, 0, f); // unshift
    }
    set_num(f, "unread", unread);
    set_num(f, "lastId", (double) id);
    cJSON_DeleteItemFromObjectCaseSensitive(f, "lastText");
    cJSON_AddStringToObject(f, "lastText", text ? text : "");
    set_num(f, "lastTs", ts);
    cJSON_DeleteItemFromObjectCaseSensitive(f, "lastFromMe");
    cJSON_AddFalseToObject(f, "lastFromMe");
    return f;
}

cJSON *social_conversation_read(cJSON *list, const int64_t peer) {
    cJSON *f = cJSON_IsArray(list) ? find(list, peer) : NULL;
    if (f) set_num(f, "unread", 0); // IÍíïíii && (unread = 0)
    return f;
}
