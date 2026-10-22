// Krunker Strike director client (client/include/strike_director.h).
#include <strike_director.h>
#include "klog.h"
#include <backend.h>
#include <build_info.h> // kn_build_protocol: the x-game-version gate's value
#include <flavor.h>
#include <html_dom.h>   // html_storage_get
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

const char *strike_director_url(void) {
    static char url[256];
    url[0] = 0;
#ifdef __EMSCRIPTEN__
    char *q = (char *) EM_ASM_PTR({
        var d = null;
        try { d = new URLSearchParams(location.search).get('director'); } catch (e) {}
        return d ? stringToNewUTF8(d) : 0;
    });
    if (q) {
        snprintf(url, sizeof(url), "%s", q);
        free(q);
    }
#else
    const char *env = getenv("KRUNKNATIVE_STRIKE_DIRECTOR");
    if (env && env[0]) snprintf(url, sizeof(url), "%s", env);
#endif
    if (!url[0] && html_storage_get("knStrikeDirector", url, sizeof(url)) < 0) url[0] = 0;
    size_t n = strlen(url);
    while (n && url[n - 1] == '/') url[--n] = 0;
    return url;
}

int strike_director_enabled(void) {
    return flavor_get() == FLAVOR_STRIKE && strike_director_url()[0] != 0;
}

static void copy_str(const cJSON *o, const char *key, char *out, const size_t n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    snprintf(out, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

void strike_director_parse(const int status, const char *body, StrikeAllocation *out) {
    memset(out, 0, sizeof(*out));
    out->status = status;
    cJSON *o = status && body ? cJSON_Parse(body) : NULL;
    copy_str(o, "error", out->error, sizeof(out->error));
    copy_str(o, "host", out->host, sizeof(out->host));
    copy_str(o, "gameId", out->game_id, sizeof(out->game_id));
    copy_str(o, "ticket", out->ticket, sizeof(out->ticket));
    copy_str(o, "region", out->region, sizeof(out->region));
    copy_str(o, "code", out->code, sizeof(out->code));
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(o, "port");
    out->port = cJSON_IsNumber(port) ? port->valueint : 0;
    out->ok = status == 200 && out->host[0] && out->port > 0 && out->port < 65536 && out->ticket[0];
    if (!out->ok && !out->error[0]) snprintf(out->error, sizeof(out->error), "%s", status ? "Failed" : "Unreachable");
    cJSON_Delete(o);
}

typedef struct AllocReq {
    StrikeAllocFn fn;
    void *user;
} AllocReq;

static void on_allocate(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    AllocReq *r = user;
    StrikeAllocation a;
    strike_director_parse(status, body, &a);
    if (a.ok) KN_LOG_DEBUG1("[strike] director allocated %s at %s:%d%s%s\n", a.game_id, a.host, a.port, a.code[0] ? ", private code " : "", a.code);
    else fprintf(stderr, "[strike] director allocate failed (HTTP %d): %s\n", status, a.error);
    if (r->fn) r->fn(r->user, &a);
    free(r);
}

int strike_director_allocate(const int priv, const char *region, const char *mode, const char *room, const char *code,
                             const StrikeAllocFn fn, void *user) {
    const char *base = strike_director_url();
    if (!base[0]) return 0;
    char url[320];
    snprintf(url, sizeof(url), "%s/v1/director/allocate%s", base, priv ? "/private" : "");
    cJSON *body = cJSON_CreateObject();
    // the x-game-version gate: backend_http_request sets no custom headers, the director reads the
    // same value from the body
    cJSON_AddStringToObject(body, "gameVersion", kn_build_protocol());
    if (region && region[0]) cJSON_AddStringToObject(body, "region", region);
    if (mode && mode[0]) cJSON_AddStringToObject(body, "mode", mode);
    if (room && room[0]) cJSON_AddStringToObject(body, "roomId", room);
    if (priv && code && code[0]) cJSON_AddStringToObject(body, "customMatchId", code);
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    AllocReq *r = calloc(1, sizeof(*r));
    if (!text || !r) {
        free(r);
        cJSON_free(text);
        return 0;
    }
    r->fn = fn;
    r->user = user;
    KN_LOG_DEBUG1("[strike] POST %s\n", url);
    const int ok = backend_http_request("POST", url, text, on_allocate, r);
    cJSON_free(text);
    if (!ok) free(r);
    return ok;
}
