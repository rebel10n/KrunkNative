// Social hub protocol + service (shared/include/social.h). Answers the frames Krunker
// v3.1.1's social page sends the way its offline server does (matchmaker/socialWs.ts,
// matchmaker/publicRoutes.ts), for a server without accounts.
#include <social.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ------------------------------------------------------------------ chunks

int social_send_message(const NetPacketType type, const uint32_t id, const char *msg, const size_t len,
                        const SocialSendFn send, void *user) {
    if (!send || (!msg && len) || len > NET_SOCIAL_MAX_MESSAGE) return 0;
    const size_t count = len ? (len + NET_SOCIAL_CHUNK_DATA - 1) / NET_SOCIAL_CHUNK_DATA : 1;
    NetSocialChunk chunk;
    chunk.id = id;
    chunk.count = (uint16_t) count;
    for (size_t i = 0; i < count; i++) {
        const size_t offset = i * NET_SOCIAL_CHUNK_DATA;
        const size_t n = len - offset < NET_SOCIAL_CHUNK_DATA ? len - offset : NET_SOCIAL_CHUNK_DATA;
        chunk.index = (uint16_t) i;
        if (n) memcpy(chunk.data, msg + offset, n);
        if (!send(user, type, &chunk, (uint16_t) (NET_SOCIAL_CHUNK_HEADER + n))) return 0;
    }
    return 1;
}

void social_assembly_free(SocialAssembly *a) {
    if (!a) return;
    free(a->buf);
    memset(a, 0, sizeof(*a));
}

int social_assembly_push(SocialAssembly *a, const void *payload, const size_t length, char **out, uint32_t *id) {
    if (!a || !payload || length < NET_SOCIAL_CHUNK_HEADER || length > sizeof(NetSocialChunk)) return -1;
    NetSocialChunk chunk;
    memcpy(&chunk, payload, length);
    const size_t n = length - NET_SOCIAL_CHUNK_HEADER;
    if (!chunk.count || chunk.index >= chunk.count) return -1;
    if (chunk.index == 0) {
        a->id = chunk.id;
        a->count = chunk.count;
        a->next = 0;
        a->len = 0;
    } else if (chunk.id != a->id || chunk.index != a->next || chunk.count != a->count) {
        a->len = 0;
        a->next = 0;
        return -1;
    }
    if (a->len + n + 1 > NET_SOCIAL_MAX_MESSAGE + 1) {
        a->len = 0;
        a->next = 0;
        return -1;
    }
    if (a->len + n + 1 > a->cap) {
        size_t cap = a->cap ? a->cap : 1024;
        while (cap < a->len + n + 1) cap *= 2;
        char *grown = realloc(a->buf, cap);
        if (!grown) return -1;
        a->buf = grown;
        a->cap = cap;
    }
    memcpy(a->buf + a->len, chunk.data, n);
    a->len += n;
    a->next++;
    if (a->next < a->count) return 0;

    char *msg = malloc(a->len + 1);
    if (!msg) return -1;
    memcpy(msg, a->buf, a->len);
    msg[a->len] = 0;
    *out = msg;
    if (id) *id = a->id;
    a->len = 0;
    a->next = 0;
    return 1;
}

// ------------------------------------------------------------------ maps (publicRoutes.ts)

size_t social_maps_from_game(const Game *game, SocialMapInfo *out, const size_t max) {
    if (!game || !out) return 0;
    size_t count = 0;
    for (size_t i = 0; i < game->map_count && count < max; i++) {
        const cJSON *map = game->maps[i];
        if (!map) continue;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(map, "name");
        if (!cJSON_IsString(name) || !name->valuestring[0]) continue;
        int duplicate = 0;
        for (size_t j = 0; j < count; j++) duplicate |= !strcmp(out[j].name, name->valuestring);
        if (duplicate) continue;
        const cJSON *creator = cJSON_GetObjectItemCaseSensitive(map, "creator");
        const cJSON *description = cJSON_GetObjectItemCaseSensitive(map, "description");
        int featured = 0;
        for (size_t j = 0; j < sizeof(g_maps) / sizeof(g_maps[0]); j++) featured |= g_maps[j] == map;
        out[count].name = name->valuestring;
        out[count].creator = cJSON_IsString(creator) && creator->valuestring[0] ? creator->valuestring : (featured ? "Krunker" : "Community");
        out[count].description = cJSON_IsString(description) ? description->valuestring : "";
        out[count].featured = featured;
        count++;
    }
    return count;
}

// fnv1a(name.toLowerCase()) as publicRoutes.ts computes map_id.
static uint32_t map_id_of(const char *name) {
    uint32_t h = 0x811c9dc5u;
    for (const unsigned char *p = (const unsigned char *) name; *p; p++) {
        h ^= (uint32_t) tolower(*p);
        h += (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24);
    }
    return h;
}

#define SOCIAL_TAB_PAGE_SIZE 42 // publicRoutes.ts TAB_PAGE_SIZE

static cJSON *map_row(const SocialMapInfo *m) {
    cJSON *row = cJSON_CreateObject();
    if (!row) return NULL;
    cJSON_AddNumberToObject(row, "map_id", map_id_of(m->name));
    cJSON_AddStringToObject(row, "map_name", m->name);
    // The offline server dates rows by the map file's mtime; KrunkNative's maps carry no
    // dates, so no row is "NEW" (La(): a missing date is age -1).
    cJSON_AddStringToObject(row, "map_date", "");
    cJSON_AddStringToObject(row, "map_initialdate", "");
    cJSON_AddStringToObject(row, "map_info", "{}");
    cJSON_AddStringToObject(row, "map_image", "");
    cJSON_AddNumberToObject(row, "map_votes", 0);
    cJSON_AddNumberToObject(row, "map_pl", 0);
    cJSON_AddNumberToObject(row, "map_updatecounter", 1);
    cJSON_AddNumberToObject(row, "map_featured", m->featured);
    cJSON_AddStringToObject(row, "map_description", m->description ? m->description : "");
    cJSON_AddStringToObject(row, "creatorname", m->creator ? m->creator : "Krunker");
    cJSON_AddNumberToObject(row, "fund", 0);
    cJSON_AddNumberToObject(row, "fav", 0);
    cJSON_AddStringToObject(row, "mod_url", "");
    cJSON_AddStringToObject(row, "mod_id", "");
    return row;
}

static int contains_ci(const char *haystack, const char *needle) {
    const size_t n = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < n && h[i] && tolower((unsigned char) h[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == n) return 1;
    }
    return !n;
}

static double param_num(const cJSON *params, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v)) return atof(v->valuestring);
    return 0;
}

static const char *param_str(const cJSON *params, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// /api/maps (sortMaps): 0 Popular (newest first), 1 Featured, 2 New, 3 [] (KR), 4 all,
// others like 0. Rows carry no dates here, so the date sorts keep the server's order.
static cJSON *api_maps(const SocialHost *host, const cJSON *params) {
    const int index = (int) param_num(params, "index");
    const int pos = (int) param_num(params, "pos");
    const char *search = param_str(params, "val");
    cJSON *data = cJSON_CreateArray();
    int skipped = 0, added = 0;
    for (size_t i = 0; host && i < host->map_count; i++) {
        const SocialMapInfo *m = &host->maps[i];
        if (search && *search) {
            if (!contains_ci(m->name, search)) continue;
        } else if (index == 3 || (index == 1 && !m->featured)) {
            continue;
        }
        if (skipped < pos * SOCIAL_TAB_PAGE_SIZE) {
            skipped++;
            continue;
        }
        if (added >= SOCIAL_TAB_PAGE_SIZE) break;
        cJSON_AddItemToArray(data, map_row(m));
        added++;
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "data", data);
    cJSON_AddNumberToObject(res, "index", index);
    cJSON_AddNumberToObject(res, "pos", pos);
    cJSON_AddNullToObject(res, "error");
    return res;
}

// /api/search?type=map&val= (all matches), anything else empty.
static cJSON *api_search(const SocialHost *host, const cJSON *params) {
    const char *type = param_str(params, "type");
    const char *val = param_str(params, "val");
    cJSON *data = cJSON_CreateArray();
    if (type && !strcmp(type, "map") && val && *val) {
        for (size_t i = 0; host && i < host->map_count; i++) {
            if (contains_ci(host->maps[i].name, val)) cJSON_AddItemToArray(data, map_row(&host->maps[i]));
        }
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "data", data);
    cJSON_AddNumberToObject(res, "index", 0);
    cJSON_AddNullToObject(res, "error");
    return res;
}

static cJSON *api_empty(const cJSON *params) {
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "data", cJSON_CreateArray());
    cJSON_AddNumberToObject(res, "index", (int) param_num(params, "index"));
    cJSON_AddNullToObject(res, "error");
    return res;
}

// ------------------------------------------------------------------ frames

static cJSON *frame_dup(const cJSON *v) {
    return v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull();
}

char *social_service_handle(const SocialHost *host, const char *request) {
    cJSON *frame = request ? cJSON_Parse(request) : NULL;
    if (!cJSON_IsArray(frame) || !cJSON_IsString(cJSON_GetArrayItem(frame, 0))) {
        cJSON_Delete(frame);
        return NULL;
    }
    const char *label = cJSON_GetArrayItem(frame, 0)->valuestring;
    cJSON *replies = cJSON_CreateArray();
    cJSON *reply = NULL;

    if (!strcmp(label, "r")) {
        // socialWs.ts "r": profile rows come from accounts (none here), the feed from
        // FEED_POST_LIST (no posts here); every other page is null ("page unavailable").
        const cJSON *page = cJSON_GetArrayItem(frame, 1);
        const cJSON *query = cJSON_GetArrayItem(frame, 2);
        reply = cJSON_CreateArray();
        cJSON_AddItemToArray(reply, cJSON_CreateString("0"));
        cJSON_AddItemToArray(reply, frame_dup(cJSON_IsString(page) ? page : NULL));
        if (cJSON_IsString(page) && !strcmp(page->valuestring, "feed")) {
            cJSON_AddItemToArray(reply, cJSON_CreateNull());
            cJSON_AddItemToArray(reply, cJSON_CreateArray());
        } else {
            cJSON_AddItemToArray(reply, frame_dup(cJSON_IsNull(query) ? NULL : query));
            cJSON_AddItemToArray(reply, cJSON_CreateNull());
        }
    } else if (!strcmp(label, "a")) {
        // handleAuth: logout (-1) has no reply; there is no account to log into.
        const cJSON *action = cJSON_GetArrayItem(frame, 1);
        if (!cJSON_IsNumber(action) || action->valueint != -1) {
            reply = cJSON_CreateArray();
            cJSON_AddItemToArray(reply, cJSON_CreateString("a"));
            cJSON_AddItemToArray(reply, cJSON_CreateString("Invalid login."));
            for (int i = 0; i < 4; i++) cJSON_AddItemToArray(reply, cJSON_CreateNull());
        }
    } else if (!strcmp(label, "api")) {
        const cJSON *path = cJSON_GetArrayItem(frame, 1);
        const cJSON *params = cJSON_GetArrayItem(frame, 2);
        if (cJSON_IsString(path)) {
            cJSON *res;
            if (!strcmp(path->valuestring, "/maps")) res = api_maps(host, params);
            else if (!strcmp(path->valuestring, "/search")) res = api_search(host, params);
            else res = api_empty(params); // /mods, /assets: no registry
            reply = cJSON_CreateArray();
            cJSON_AddItemToArray(reply, cJSON_CreateString("api"));
            cJSON_AddItemToArray(reply, cJSON_CreateString(path->valuestring));
            cJSON_AddItemToArray(reply, res);
        }
    } else if (!strcmp(label, "pst")) {
        reply = cJSON_CreateArray();
        cJSON_AddItemToArray(reply, cJSON_CreateString("psd"));
        cJSON_AddItemToArray(reply, cJSON_CreateArray());
    } else if (!strcmp(label, "guf")) {
        reply = cJSON_CreateArray();
        cJSON_AddItemToArray(reply, cJSON_CreateString("uf"));
        cJSON_AddItemToArray(reply, cJSON_CreateArray());
    }
    // "sp" (player search), "f"/"kr"/"ctr"/... (login required), "po": no reply, like the
    // offline server (unknown labels are dropped; auth-only ones answer with a notify the
    // page has no handler for).

    cJSON_Delete(frame);
    if (!reply) {
        cJSON_Delete(replies);
        return NULL;
    }
    cJSON_AddItemToArray(replies, reply);
    char *out = cJSON_PrintUnformatted(replies);
    cJSON_Delete(replies);
    if (!out) return NULL;
    // cJSON_PrintUnformatted uses cJSON's allocator (malloc by default): hand back a plain
    // malloc'd copy so callers can always free().
    char *copy = malloc(strlen(out) + 1);
    if (copy) strcpy(copy, out);
    cJSON_free(out);
    return copy;
}
