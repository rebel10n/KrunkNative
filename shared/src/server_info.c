// Server-info query encode/decode (shared/include/server_info.h). No sockets, no state.
#include <server_info.h>
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

static const uint8_t k_prefix[SERVER_INFO_PREFIX_LEN] = {0xFF, 0xFF, 0xFF, 0xFF};
static const char k_request_tag[] = "KNINFO"; // 6 bytes
static const char k_reply_tag[] = "KNinfo";

int server_info_is_query(const uint8_t *buf, const size_t len) {
    return buf && len >= SERVER_INFO_PREFIX_LEN && !memcmp(buf, k_prefix, SERVER_INFO_PREFIX_LEN);
}

static int nonce_ok(const char *nonce, const size_t len) {
    if (len > SERVER_INFO_NONCE_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char) nonce[i];
        if (c < 0x21 || c > 0x7E || c == '"' || c == '\\') return 0; // printable, JSON-safe
    }
    return 1;
}

size_t server_info_build_request(uint8_t *out, const size_t cap, const char *nonce) {
    const size_t nlen = nonce ? strlen(nonce) : 0;
    const size_t total = SERVER_INFO_PREFIX_LEN + 6 + (nlen ? 1 + nlen : 0);
    if (!out || cap < total || !nonce_ok(nonce ? nonce : "", nlen)) return 0;
    memcpy(out, k_prefix, SERVER_INFO_PREFIX_LEN);
    memcpy(out + SERVER_INFO_PREFIX_LEN, k_request_tag, 6);
    if (nlen) {
        out[SERVER_INFO_PREFIX_LEN + 6] = 0;
        memcpy(out + SERVER_INFO_PREFIX_LEN + 7, nonce, nlen);
    }
    return total;
}

int server_info_parse_request(const uint8_t *in, const size_t len, char *nonce) {
    if (nonce) nonce[0] = 0;
    if (!server_info_is_query(in, len) || len < SERVER_INFO_PREFIX_LEN + 6) return 0;
    if (memcmp(in + SERVER_INFO_PREFIX_LEN, k_request_tag, 6)) return 0;
    const size_t rest = len - SERVER_INFO_PREFIX_LEN - 6;
    if (!rest) return 1;
    if (in[SERVER_INFO_PREFIX_LEN + 6] != 0) return 0;
    const size_t nlen = rest - 1;
    const char *n = (const char *) in + SERVER_INFO_PREFIX_LEN + 7;
    if (!nonce_ok(n, nlen)) return 0;
    if (nonce) {
        memcpy(nonce, n, nlen);
        nonce[nlen] = 0;
    }
    return 1;
}

size_t server_info_build_reply(uint8_t *out, const size_t cap, const ServerInfo *info) {
    if (!out || !info || cap < SERVER_INFO_PREFIX_LEN + 8) return 0;
    cJSON *o = cJSON_CreateObject();
    if (!o) return 0;
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "game", "krunknative");
    cJSON_AddStringToObject(o, "name", info->name);
    cJSON_AddStringToObject(o, "map", info->map);
    cJSON_AddStringToObject(o, "mode", info->mode);
    cJSON_AddNumberToObject(o, "modeId", info->mode_id);
    cJSON_AddNumberToObject(o, "players", info->players);
    cJSON_AddNumberToObject(o, "max", info->max_players);
    cJSON_AddNumberToObject(o, "bots", info->bots);
    cJSON *tags = cJSON_AddArrayToObject(o, "tags");
    for (int i = 0; i < info->tag_count && i < SERVER_INFO_TAGS_MAX; i++) cJSON_AddItemToArray(tags, cJSON_CreateString(info->tags[i]));
    cJSON_AddStringToObject(o, "community", info->community);
    cJSON_AddBoolToObject(o, "online_mode", info->online_mode);
    cJSON_AddBoolToObject(o, "password", info->password);
    cJSON_AddStringToObject(o, "region", info->region);
    cJSON_AddStringToObject(o, "version", info->version);
    cJSON_AddNumberToObject(o, "port", info->port);
    cJSON_AddStringToObject(o, "nonce", info->nonce);
    cJSON_AddStringToObject(o, "commit", info->commit);
    cJSON_AddNumberToObject(o, "commit_time", (double) info->commit_time);
    cJSON_AddStringToObject(o, "build_time", info->build_time);
    cJSON_AddStringToObject(o, "protocol", info->protocol);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return 0;
    const size_t jlen = strlen(json);
    const size_t total = SERVER_INFO_PREFIX_LEN + 7 + jlen;
    size_t written = 0;
    if (jlen <= SERVER_INFO_JSON_MAX && total <= cap) {
        memcpy(out, k_prefix, SERVER_INFO_PREFIX_LEN);
        memcpy(out + SERVER_INFO_PREFIX_LEN, k_reply_tag, 6);
        out[SERVER_INFO_PREFIX_LEN + 6] = 0;
        memcpy(out + SERVER_INFO_PREFIX_LEN + 7, json, jlen);
        written = total;
    }
    cJSON_free(json);
    return written;
}

static void copy_str(char *dst, const size_t size, const cJSON *o, const char *key) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
    snprintf(dst, size, "%s", v ? v : "");
}

static int get_int(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

int server_info_parse_reply(const uint8_t *in, const size_t len, ServerInfo *info) {
    if (!info || !server_info_is_query(in, len) || len < SERVER_INFO_PREFIX_LEN + 8) return 0;
    if (memcmp(in + SERVER_INFO_PREFIX_LEN, k_reply_tag, 6) || in[SERVER_INFO_PREFIX_LEN + 6] != 0) return 0;
    const size_t jlen = len - SERVER_INFO_PREFIX_LEN - 7;
    if (jlen > SERVER_INFO_JSON_MAX) return 0;
    cJSON *o = cJSON_ParseWithLength((const char *) in + SERVER_INFO_PREFIX_LEN + 7, jlen);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return 0;
    }
    memset(info, 0, sizeof(*info));
    copy_str(info->name, sizeof(info->name), o, "name");
    copy_str(info->map, sizeof(info->map), o, "map");
    copy_str(info->mode, sizeof(info->mode), o, "mode");
    info->mode_id = get_int(o, "modeId");
    info->players = get_int(o, "players");
    info->max_players = get_int(o, "max");
    info->bots = get_int(o, "bots");
    const cJSON *tag;
    cJSON_ArrayForEach(tag, cJSON_GetObjectItemCaseSensitive(o, "tags")) {
        if (info->tag_count >= SERVER_INFO_TAGS_MAX || !cJSON_IsString(tag)) continue;
        snprintf(info->tags[info->tag_count++], sizeof(info->tags[0]), "%s", tag->valuestring);
    }
    copy_str(info->community, sizeof(info->community), o, "community");
    info->online_mode = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "online_mode"));
    info->password = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "password"));
    copy_str(info->region, sizeof(info->region), o, "region");
    copy_str(info->version, sizeof(info->version), o, "version");
    info->port = get_int(o, "port");
    copy_str(info->nonce, sizeof(info->nonce), o, "nonce");
    copy_str(info->commit, sizeof(info->commit), o, "commit");
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(o, "commit_time");
    info->commit_time = cJSON_IsNumber(ct) && ct->valuedouble > 0 ? (int64_t) ct->valuedouble : 0;
    copy_str(info->build_time, sizeof(info->build_time), o, "build_time");
    copy_str(info->protocol, sizeof(info->protocol), o, "protocol");
    cJSON_Delete(o);
    return 1;
}
