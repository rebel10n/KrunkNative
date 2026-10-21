// The "queued for ranked" notice's social frame (ranked_notice.h). Pure: no database, no socket.
#include "ranked_notice.h"

cJSON *ranked_notice_social_event(const int64_t account, const char *name) {
    const int named = name && name[0];
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "type", "ranked:queued");
    cJSON_AddNumberToObject(obj, "account", (double) account);
    cJSON_AddStringToObject(obj, "name", named ? name : "");
    cJSON_AddStringToObject(obj, "template", named ? "$ queued for ranked" : "A player queued for ranked");
    cJSON_AddStringToObject(obj, "color", "#5ce05a");
    return obj;
}
