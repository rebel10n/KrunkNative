// kn_rentd's pure part (server/include/rentd.h; doc/v10/rented-servers.md).
#include <rentd.h>

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t size, const char *src) { snprintf(dst, size, "%s", src ? src : ""); }

static const char *jstr(const cJSON *o, const char *k) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
    return v ? v : "";
}

static int jint(const cJSON *o, const char *k, int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

int rentd_parse_slots(const char *json, RentdSlot *out, int max) {
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    const cJSON *slots = cJSON_GetObjectItemCaseSensitive(root, "slots");
    if (!cJSON_IsArray(slots)) {
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    const cJSON *s;
    cJSON_ArrayForEach(s, slots) {
        if (n >= max) break;
        RentdSlot *r = &out[n];
        memset(r, 0, sizeof(*r));
        r->id = jint(s, "id", 0);
        r->port = jint(s, "port", 0);
        if (r->id <= 0 || r->port <= 0 || r->port > 65535) continue;
        r->region = jint(s, "region", 0);
        r->generation = jint(s, "generation", 0);
        r->run = jint(s, "run", 0);
        copy_str(r->name, sizeof(r->name), jstr(s, "name"));
        copy_str(r->public_host, sizeof(r->public_host), jstr(s, "public_host"));
        copy_str(r->map, sizeof(r->map), jstr(s, "map"));
        copy_str(r->creator, sizeof(r->creator), jstr(s, "creator"));
        copy_str(r->key, sizeof(r->key), jstr(s, "key"));
        n++;
    }
    cJSON_Delete(root);
    return n;
}

// A value that can sit between quotes on one server.cfg line, without a path or a list separator
static int cfg_safe(const char *s) {
    for (; *s; s++)
        if (*s == '"' || *s == '\\' || *s == '\n' || *s == '\r' || *s == ',' || (unsigned char) *s < 0x20) return 0;
    return 1;
}

int rentd_map_arg(const RentdSlot *slot, char *out, size_t size) {
    out[0] = 0;
    if (!slot) return 0;
    if (slot->key[0]) {
        if (!cfg_safe(slot->key) || strchr(slot->key, '/')) return 0;
        snprintf(out, size, "%s", slot->key);
    } else if (slot->map[0] && slot->creator[0]) {
        if (!cfg_safe(slot->map) || !cfg_safe(slot->creator) || strchr(slot->creator, '/')) return 0;
        snprintf(out, size, "published:%s/%s", slot->creator, slot->map);
    } else if (slot->map[0]) {
        if (!cfg_safe(slot->map)) return 0;
        snprintf(out, size, "%s", slot->map);
    }
    return out[0] != 0;
}

int rentd_cfg_text(const RentdSlot *slot, const RentdShared *sh, const char *state_dir, char *out, size_t size) {
    char map[256];
    if (!rentd_map_arg(slot, map, sizeof(map)) || !cfg_safe(slot->name) || !cfg_safe(slot->public_host)) return -1;
    const char *pub = slot->public_host[0] ? slot->public_host : sh->public_host;
    const int n = snprintf(out, size,
                           "// Written by kn_rentd (doc/v10/rented-servers.md): rented server %d, generation %d.\n"
                           "hostname        \"%s\"\n"
                           "region          \"%s\"\n"
                           "node_id         \"RENT%d\"\n"
                           "port            %d\n"
                           "query_port      0\n"
                           "listen_host     \"%s\"\n"
                           "public_host     \"%s\"\n"
                           "max_players     %d\n"
                           "maps            \"%s\"\n"
                           "dedicated       1\n"
                           "bot_quota       0\n"
                           "online_mode     1\n"
                           "auth_url        \"%s\"\n"
                           "rcon_port       0\n"
                           "bans_file       \"%s/rent-%d-bans.json\"\n"
                           "shutdown_grace  30\n"
                           "coordinator_url \"%s\"\n"
                           "coordinator_secret \"%s\"\n"
                           "join_secret     \"%s\"\n"
                           "brain_http_url  \"%s\"\n",
                           slot->id, slot->generation, slot->name, sh->region, slot->id, slot->port, sh->listen_host, pub,
                           sh->max_players > 0 ? sh->max_players : 8, map, sh->auth_url, state_dir, slot->id, sh->coordinator_url,
                           sh->coordinator_secret, sh->join_secret, sh->brain_http_url);
    return n > 0 && (size_t) n < size ? n : -1;
}

RentdAction rentd_decide(const RentdSlot *want, const int alive, const int gen, const double now, const double not_before) {
    const int run = want && want->run;
    if (!run) return alive ? RENTD_STOP : RENTD_KEEP;
    if (!alive) return now >= not_before ? RENTD_START : RENTD_KEEP;
    return gen != want->generation ? RENTD_RESTART : RENTD_KEEP;
}

double rentd_backoff(const int crashes) {
    double s = 1.0;
    for (int i = 1; i < crashes && s < 60.0; i++) s *= 2.0;
    return s > 60.0 ? 60.0 : s;
}
