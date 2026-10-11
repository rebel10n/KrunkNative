/* knsvc/json.h implementation. */
#include "knsvc/json.h"

#include <stdlib.h>
#include <string.h>

static const cJSON *item(const cJSON *obj, const char *key) {
    return (obj && key) ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
}

const char *kn_json_str(const cJSON *obj, const char *key, const char *def) {
    const cJSON *it = item(obj, key);
    return cJSON_IsString(it) && it->valuestring ? it->valuestring : def;
}

int64_t kn_json_int(const cJSON *obj, const char *key, int64_t def) {
    const cJSON *it = item(obj, key);
    if (cJSON_IsNumber(it)) return (int64_t)it->valuedouble;
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    return def;
}

double kn_json_num(const cJSON *obj, const char *key, double def) {
    const cJSON *it = item(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

int kn_json_bool(const cJSON *obj, const char *key, int def) {
    const cJSON *it = item(obj, key);
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    if (cJSON_IsNumber(it)) return it->valuedouble != 0;
    return def;
}

char *kn_json_print(const cJSON *json) {
    return json ? cJSON_PrintUnformatted(json) : NULL;
}

cJSON *kn_json_parse(const char *data, size_t len) {
    if (!data) return NULL;
    return cJSON_ParseWithLength(data, len);
}
