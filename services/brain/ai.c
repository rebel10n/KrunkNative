/*
 * services/brain/ai.c - the AI proxy of the map editor (ai.h): /ai/models, /ai/status,
 * /ai/chat (streaming), per-account limits, the upstream health probe and the server-tool
 * bridge.
 *
 * Upstream calls run on one detached worker thread each (blocking libcurl, like auth.c's
 * hasJoined); the bytes come back to the loop thread through kn_loop_post_threadsafe in
 * coalesced batches, where shared/src/ai_proto.c parses them (chat-completions or responses
 * style) and the brain relays its own normalised chunk stream to the editor. A client that goes
 * away cancels the upstream transfer (curl's progress callback). The key only ever appears in
 * the Authorization header of those transfers.
 */
#include "ai.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "accounts.h"
#include "ai_proto.h"
#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#ifdef KN_HAVE_CURL
#include <curl/curl.h>
#endif

#define MOD "ai"
#define AI_MAX_PENDING_MODELS 64
#define AI_RPM_CAP 240
#define AI_PING_MS 15000
#define AI_MAX_RAW (8u << 20)          /* upstream body we keep for a non-SSE / error answer */
#define AI_TOOL_RESULT_MAX (48u << 10)  /* server tool result text handed to the model */

/* ------------------------------------------------------------------ config */

static struct {
    int enabled;
    char url[512];         /* base, no trailing '/' */
    char *key;             /* never logged */
    AiApi api;
    char default_model[96];
    int max_tokens, timeout_ms, connect_timeout_ms, max_messages, max_rounds;
    int models_ttl_s, health_interval_s, health_probe_max;
    int lim_rpm, lim_daily, lim_concurrent;
    int64_t lim_daily_tokens;
    char ua[64];
    char hide[32][96];
    int nhide;
    int banter_enabled, banter_rpm, banter_max_tokens, banter_timeout_ms;
} C;

static Brain *g_b;
static const AiServerTools *g_server_tools;

int ai_configured(void) { return C.enabled && C.url[0] && C.key && C.key[0]; }

void ai_register_server_tools(const AiServerTools *t) { g_server_tools = t; }

static void copy_str(char *out, const size_t n, const char *s) { snprintf(out, n, "%s", s ? s : ""); }

/* ------------------------------------------------------------------ upstream transfers */

typedef struct Up Up;
typedef void (*UpChunkFn)(Up *u, const char *data, size_t n); /* loop thread */
typedef void (*UpDoneFn)(Up *u);                               /* loop thread; owns u (up_free) */

struct Up {
    char url[1024];
    char method[8];
    char *body;
    int timeout_ms;
    int sse;               /* Accept: text/event-stream */
    pthread_mutex_t mu;
    AiBuf pending;         /* worker -> loop */
    int task_posted;
    int finished;
    int delivered;
    atomic_int cancel;
    int status;            /* HTTP status, 0 = transport error */
    int hstatus;           /* status line seen (before the body; read by on_chunk) */
    char ctype[128];
    char err[256];
    uint64_t t0;
    UpChunkFn on_chunk;
    UpDoneFn on_done;
    void *ud;
};

/* live worker threads (ai_shutdown cancels them and waits a moment) */
static atomic_int g_up_live;
static pthread_mutex_t g_up_mu = PTHREAD_MUTEX_INITIALIZER;
static Up *g_up_list[256];

static void up_track(Up *u, const int add) {
    pthread_mutex_lock(&g_up_mu);
    for (int i = 0; i < 256; i++) {
        if (add ? !g_up_list[i] : g_up_list[i] == u) {
            g_up_list[i] = add ? u : NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_up_mu);
}

static void up_free(Up *u) {
    if (!u) return;
    up_track(u, 0);
    free(u->body);
    ai_buf_free(&u->pending);
    pthread_mutex_destroy(&u->mu);
    free(u);
}

static void up_task(kn_loop *loop, void *arg) {
    (void) loop;
    Up *u = arg;
    pthread_mutex_lock(&u->mu);
    AiBuf data = u->pending;
    memset(&u->pending, 0, sizeof(u->pending));
    u->task_posted = 0;
    const int fin = u->finished;
    pthread_mutex_unlock(&u->mu);
    if (data.len && u->on_chunk) u->on_chunk(u, data.data, data.len);
    ai_buf_free(&data);
    if (fin && !u->delivered) {
        u->delivered = 1;
        u->on_done(u);
    }
}

#ifdef KN_HAVE_CURL
static size_t up_write(char *data, size_t size, size_t n, void *ud) {
    Up *u = ud;
    const size_t len = size * n;
    if (atomic_load(&u->cancel)) return 0;
    pthread_mutex_lock(&u->mu);
    ai_buf_add(&u->pending, data, len);
    if (!u->task_posted) {
        u->task_posted = 1;
        kn_loop_post_threadsafe(g_b->loop, up_task, u);
    }
    pthread_mutex_unlock(&u->mu);
    return len;
}

static size_t up_header(char *data, size_t size, size_t n, void *ud) {
    Up *u = ud;
    const size_t len = size * n;
    if (len > 9 && !strncmp(data, "HTTP/", 5)) {
        const char *sp = memchr(data, ' ', len);
        if (sp) u->hstatus = atoi(sp + 1);
        u->ctype[0] = 0; /* a new response (after a 100 Continue / redirect) */
    }
    if (len > 13 && !strncasecmp(data, "content-type:", 13)) {
        const char *v = data + 13;
        while (*v == ' ') v++;
        size_t k = 0;
        while (k + 1 < sizeof(u->ctype) && v + k < data + len && v[k] != '\r' && v[k] != '\n') {
            u->ctype[k] = v[k];
            k++;
        }
        u->ctype[k] = 0;
    }
    return len;
}

static int up_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void) a; (void) b; (void) c; (void) d;
    return atomic_load(&((Up *) ud)->cancel) ? 1 : 0;
}
#endif

static void *up_thread(void *arg) {
    Up *u = arg;
#ifdef KN_HAVE_CURL
    CURL *c = curl_easy_init();
    if (!c) {
        copy_str(u->err, sizeof(u->err), "curl_easy_init failed");
    } else {
        struct curl_slist *h = NULL;
        char auth[1200];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", C.key ? C.key : "");
        h = curl_slist_append(h, auth);
        memset(auth, 0, sizeof(auth));
        h = curl_slist_append(h, u->sse ? "Accept: text/event-stream" : "Accept: application/json");
        if (u->body) h = curl_slist_append(h, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_URL, u->url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_USERAGENT, C.ua);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long) C.connect_timeout_ms);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long) u->timeout_ms);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, up_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, u);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, up_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, u);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, up_progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, u);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        if (u->body) {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, u->body);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long) strlen(u->body));
        }
        const CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        if (rc == CURLE_OK) u->status = (int) code;
        else if (atomic_load(&u->cancel)) copy_str(u->err, sizeof(u->err), "cancelled");
        else snprintf(u->err, sizeof(u->err), "%s", curl_easy_strerror(rc));
        curl_slist_free_all(h); /* the key's header copy */
        curl_easy_cleanup(c);
    }
#else
    copy_str(u->err, sizeof(u->err), "kn_brain was built without libcurl (no https upstream)");
#endif
    pthread_mutex_lock(&u->mu);
    u->finished = 1;
    if (!u->task_posted) {
        u->task_posted = 1;
        kn_loop_post_threadsafe(g_b->loop, up_task, u);
    }
    pthread_mutex_unlock(&u->mu);
    atomic_fetch_sub(&g_up_live, 1);
    return NULL;
}

/* path is appended to C.url ("/models", "/chat/completions"). body taken over (NULL = GET). */
static Up *up_start(const char *path, char *body, const int sse, const int timeout_ms, UpChunkFn on_chunk, UpDoneFn on_done, void *ud) {
    Up *u = calloc(1, sizeof(*u));
    if (!u) {
        free(body);
        return NULL;
    }
    pthread_mutex_init(&u->mu, NULL);
    snprintf(u->url, sizeof(u->url), "%s%s", C.url, path);
    copy_str(u->method, sizeof(u->method), body ? "POST" : "GET");
    u->body = body;
    u->sse = sse;
    u->timeout_ms = timeout_ms;
    u->on_chunk = on_chunk;
    u->on_done = on_done;
    u->ud = ud;
    u->t0 = kn_now_ms();
    atomic_init(&u->cancel, 0);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    up_track(u, 1);
    atomic_fetch_add(&g_up_live, 1);
    const int ok = pthread_create(&t, &attr, up_thread, u) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) {
        atomic_fetch_sub(&g_up_live, 1);
        up_free(u);
        return NULL;
    }
    return u;
}

/* ------------------------------------------------------------------ health probe (per model)
 *
 * A known prompt with a random token goes to a few models: the configured preferred default
 * (ai.default_model) first, then the tool-capable ones, then the rest, at most
 * ai.health_probe_max per cycle (start + every ai.health_interval_s), one at a time, until one
 * echoes the token. Each probed model gets a status the picker shows:
 *   ok           answered and echoed the token
 *   degraded     answered without the token (the provider drops / rewrites request content)
 *   unavailable  an error (HTTP 4xx/5xx or an error body: "Blocked for: ...")
 * Chats that fail with a model error mark that model unavailable too (re-probed next cycle).
 * The effective default model is the preferred one when it is ok, else the first ok model, else
 * the first degraded one (tool-capable first); the service health is the effective default's. */

enum { MH_UNKNOWN = 0, MH_OK, MH_DEGRADED, MH_UNAVAILABLE };

typedef struct {
    char id[96];
    int state;
    char reason[200];
    int64_t checked_at;    /* wall ms */
} ModelHealth;

static struct {
    ModelHealth m[128];
    int n;
    int state;             /* AI_HEALTH_* of the effective default */
    char reason[256];
    int64_t checked_at;
    int64_t latency_ms;
    char model[96];        /* the effective default */
    char token[32];
    char probing[96];      /* model of the probe in flight ("" none) */
    AiBuf raw;
    int running;
    char queue[16][96];    /* this cycle's models still to probe */
    int nqueue;
    kn_timer_id timer;
    int cycles;
} H;

static const cJSON *models_list(void);
static int model_row_known(const char *id);

static ModelHealth *mh_get(const char *id, const int create) {
    for (int i = 0; i < H.n; i++) {
        if (!strcmp(H.m[i].id, id)) return &H.m[i];
    }
    if (!create || H.n >= (int) (sizeof(H.m) / sizeof(H.m[0]))) return NULL;
    ModelHealth *m = &H.m[H.n++];
    memset(m, 0, sizeof(*m));
    copy_str(m->id, sizeof(m->id), id);
    return m;
}

static const char *mh_name(const int s) {
    return s == MH_OK ? "ok" : s == MH_DEGRADED ? "degraded" : s == MH_UNAVAILABLE ? "unavailable" : "unknown";
}

static void mh_set(const char *id, const int state, const char *reason) {
    ModelHealth *m = mh_get(id, 1);
    if (!m) return;
    if (m->state != state) KN_INFO(MOD, "model %s: %s%s%s", id, mh_name(state), reason && reason[0] ? " (" : "", reason && reason[0] ? reason : "");
    m->state = state;
    copy_str(m->reason, sizeof(m->reason), reason);
    m->checked_at = kn_wall_ms();
}

static int model_tools(const cJSON *row) { return kn_json_bool(row, "tools", 1); }

/* the effective default: preferred if ok, else the first ok, else the preferred if degraded, else
 * the first degraded (tool-capable models before the rest), else the preferred */
static const char *effective_default(void) {
    const ModelHealth *pref = mh_get(C.default_model, 0);
    if (pref && pref->state == MH_OK) return C.default_model;
    for (int want = MH_OK; want <= MH_DEGRADED; want++) {
        if (want == MH_DEGRADED && pref && pref->state == MH_DEGRADED) return C.default_model;
        for (int pass = 0; pass < 2; pass++) {
            const cJSON *row;
            cJSON_ArrayForEach(row, models_list()) {
                if (pass == 0 && !model_tools(row)) continue;
                if (pass == 1 && model_tools(row)) continue;
                const ModelHealth *m = mh_get(kn_json_str(row, "id", ""), 0);
                if (m && m->state == want) return m->id;
            }
        }
    }
    return C.default_model;
}

static void health_update(void) {
    const char *def = effective_default();
    const ModelHealth *m = mh_get(def, 0);
    int state = AI_HEALTH_UNKNOWN;
    char reason[256] = "";
    if (m && m->state == MH_OK) state = AI_HEALTH_OK;
    else if (m && m->state == MH_DEGRADED) {
        state = AI_HEALTH_DEGRADED;
        snprintf(reason, sizeof(reason), "the AI provider answers but does not pass our prompts through (known-prompt check failed)");
    } else if (m && m->state == MH_UNAVAILABLE) {
        state = AI_HEALTH_DOWN;
        snprintf(reason, sizeof(reason), "no model answers (%.60s: %.150s)", m->id, m->reason);
    }
    if (state != H.state || strcmp(reason, H.reason) || strcmp(def, H.model)) {
        if (state == AI_HEALTH_OK) KN_INFO(MOD, "upstream healthy (default model %s)", def);
        else if (state != AI_HEALTH_UNKNOWN) KN_WARN(MOD, "upstream %s (default model %s): %s", state == AI_HEALTH_DEGRADED ? "degraded" : "down", def, reason);
    }
    H.state = state;
    copy_str(H.reason, sizeof(H.reason), reason);
    copy_str(H.model, sizeof(H.model), def);
}

int ai_health(void) { return ai_configured() ? H.state : AI_HEALTH_UNCONFIGURED; }
const char *ai_health_reason(void) { return ai_configured() ? H.reason : "the AI service is not configured on this server"; }

static const char *health_name(const int s) {
    switch (s) {
        case AI_HEALTH_OK: return "healthy";
        case AI_HEALTH_DEGRADED: return "degraded";
        case AI_HEALTH_DOWN: return "down";
        case AI_HEALTH_UNCONFIGURED: return "unconfigured";
        default: return "unknown";
    }
}

cJSON *ai_health_json(void) {
    cJSON *o = cJSON_CreateObject();
    const int s = ai_health();
    cJSON_AddStringToObject(o, "state", health_name(s));
    cJSON_AddStringToObject(o, "reason", ai_health_reason());
    if (H.checked_at) cJSON_AddNumberToObject(o, "checked_at", (double) H.checked_at);
    if (H.checked_at) cJSON_AddNumberToObject(o, "latency_ms", (double) H.latency_ms);
    if (H.model[0]) cJSON_AddStringToObject(o, "model", H.model);
    cJSON *models = cJSON_AddObjectToObject(o, "models");
    for (int i = 0; i < H.n; i++) {
        cJSON *m = cJSON_AddObjectToObject(models, H.m[i].id);
        cJSON_AddStringToObject(m, "status", mh_name(H.m[i].state));
        if (H.m[i].reason[0]) cJSON_AddStringToObject(m, "reason", H.m[i].reason);
    }
    return o;
}

static void probe_next(void);

static void probe_chunk(Up *u, const char *data, const size_t n) {
    (void) u;
    if (H.raw.len + n <= AI_MAX_RAW) ai_buf_add(&H.raw, data, n);
}

static void probe_done(Up *u) {
    H.running = 0;
    H.checked_at = kn_wall_ms();
    H.latency_ms = (int64_t) (kn_now_ms() - u->t0);
    char reason[200];
    int state;
    if (!u->status) {
        snprintf(reason, sizeof(reason), "unreachable (%.150s)", u->err);
        state = MH_UNAVAILABLE;
    } else if (u->status >= 400) {
        char e[160];
        ai_error_text(H.raw.data, H.raw.len, e, sizeof(e));
        snprintf(reason, sizeof(reason), "HTTP %d: %s", u->status, e);
        state = MH_UNAVAILABLE;
    } else {
        AiStream s;
        ai_stream_init(&s, NULL, NULL);
        if (strstr(u->ctype, "event-stream")) {
            ai_stream_feed(&s, H.raw.data ? H.raw.data : "", H.raw.len);
            ai_stream_finish(&s);
        } else {
            ai_parse_response(&s, H.raw.data ? H.raw.data : "", H.raw.len);
        }
        const int echoed = (s.text.data && strstr(s.text.data, H.token)) || (s.reasoning.data && strstr(s.reasoning.data, H.token));
        if (s.error[0]) {
            snprintf(reason, sizeof(reason), "%.190s", s.error);
            state = MH_UNAVAILABLE;
        } else if (!echoed) {
            snprintf(reason, sizeof(reason), "answers without passing the prompt through");
            state = MH_DEGRADED;
        } else {
            reason[0] = 0;
            state = MH_OK;
        }
        ai_stream_free(&s);
    }
    mh_set(H.probing, state, reason);
    ai_buf_free(&H.raw);
    up_free(u);
    if (state == MH_OK) H.nqueue = 0; /* one confirmed model is enough this cycle */
    H.probing[0] = 0;
    health_update();
    probe_next();
}

static void probe_start(const char *model) {
    snprintf(H.token, sizeof(H.token), "KN-PROBE-%06u", (unsigned) ((kn_now_ms() * 2654435761u) ^ (uint64_t) rand()) % 1000000u);
    copy_str(H.probing, sizeof(H.probing), model);
    cJSON *msgs = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    char prompt[160];
    snprintf(prompt, sizeof(prompt), "Health check. Reply with exactly this token and nothing else: %s", H.token);
    cJSON_AddStringToObject(m, "content", prompt);
    cJSON_AddItemToArray(msgs, m);
    char *body = ai_request_body(C.api, model, msgs, NULL, NULL, 0, 512);
    cJSON_Delete(msgs);
    H.running = up_start(ai_api_path(C.api), body, 0, 60000, probe_chunk, probe_done, NULL) != NULL;
    if (!H.running) H.probing[0] = 0;
}

static void probe_next(void) {
    if (H.running || !H.nqueue) return;
    char model[96];
    copy_str(model, sizeof(model), H.queue[0]);
    memmove(H.queue[0], H.queue[1], sizeof(H.queue[0]) * (size_t) (H.nqueue - 1));
    H.nqueue--;
    probe_start(model);
}

static void queue_add(const char *id) {
    if (!id || !id[0] || H.nqueue >= C.health_probe_max || H.nqueue >= 16) return;
    for (int i = 0; i < H.nqueue; i++) {
        if (!strcmp(H.queue[i], id)) return;
    }
    copy_str(H.queue[H.nqueue++], sizeof(H.queue[0]), id);
}

/* one cycle: the preferred default, then the tool-capable models, then the rest */
static void probe_cycle(kn_loop *loop, void *ud) {
    (void) loop; (void) ud;
    if (H.running || !ai_configured()) return;
    H.cycles++;
    H.nqueue = 0;
    if (!model_row_known(C.default_model) && models_list()) {
        /* the preferred model is not offered at all */
        mh_set(C.default_model, MH_UNAVAILABLE, "not in the provider's model list");
    } else {
        queue_add(C.default_model);
    }
    for (int pass = 0; pass < 2; pass++) {
        const cJSON *row;
        cJSON_ArrayForEach(row, models_list()) {
            if ((pass == 0) != (model_tools(row) != 0)) continue;
            queue_add(kn_json_str(row, "id", ""));
        }
    }
    probe_next();
}

/* a chat's model error: the model is marked unavailable until a probe says otherwise */
static void model_failed(const char *id, const char *reason) {
    mh_set(id, MH_UNAVAILABLE, reason);
    health_update();
}

static void model_answered(const char *id) {
    ModelHealth *m = mh_get(id, 0);
    if (m && m->state == MH_UNAVAILABLE) {
        m->state = MH_UNKNOWN; /* it answers again; the next probe tells ok from degraded */
        m->reason[0] = 0;
        health_update();
    }
}

/* ------------------------------------------------------------------ /ai/models */

static struct {
    cJSON *models;         /* normalised list (owned) */
    int64_t fetched_ms;    /* monotonic */
    char error[256];       /* last fetch error ("" none) */
    int fetching;
    AiBuf raw;
    kn_http_req *waiting[AI_MAX_PENDING_MODELS];
    int nwaiting;
} M;

static const cJSON *model_row(const char *id) {
    const cJSON *m;
    cJSON_ArrayForEach(m, M.models) {
        if (!strcmp(kn_json_str(m, "id", ""), id)) return m;
    }
    return NULL;
}

static const cJSON *models_list(void) { return M.models; }
static int model_row_known(const char *id) { return model_row(id) != NULL; }

static cJSON *limits_json(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "rpm", C.lim_rpm);
    cJSON_AddNumberToObject(o, "daily", C.lim_daily);
    cJSON_AddNumberToObject(o, "daily_tokens", (double) C.lim_daily_tokens);
    cJSON_AddNumberToObject(o, "concurrent", C.lim_concurrent);
    return o;
}

static void reply_cors_json(kn_http_req *req, const int status, const cJSON *o) { brain_reply_json(req, status, o); }

static void reply_error(kn_http_req *req, const int status, const char *code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 0);
    cJSON_AddStringToObject(o, "error", code);
    cJSON_AddStringToObject(o, "message", message);
    reply_cors_json(req, status, o);
    cJSON_Delete(o);
}

static cJSON *server_tool_names(void) {
    cJSON *names = cJSON_CreateArray();
    cJSON *defs = g_server_tools && g_server_tools->defs ? g_server_tools->defs() : NULL;
    const cJSON *t;
    cJSON_ArrayForEach(t, defs) {
        const char *n = kn_json_str(cJSON_GetObjectItemCaseSensitive(t, "function"), "name", NULL);
        if (n) cJSON_AddItemToArray(names, cJSON_CreateString(n));
    }
    cJSON_Delete(defs);
    return names;
}

static void models_reply(kn_http_req *req) {
    if (!kn_http_req_alive(req)) {
        kn_http_respond_error(req, 499, "gone");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    const int ok = M.models != NULL;
    cJSON_AddBoolToObject(o, "success", ok);
    cJSON *rows = ok ? cJSON_Duplicate(M.models, 1) : cJSON_CreateArray();
    cJSON *row;
    cJSON_ArrayForEach(row, rows) { /* the probe / chat results: the picker marks unavailable models */
        const ModelHealth *mh = mh_get(kn_json_str(row, "id", ""), 0);
        cJSON_AddStringToObject(row, "status", mh_name(mh ? mh->state : MH_UNKNOWN));
        if (mh && mh->reason[0]) cJSON_AddStringToObject(row, "status_reason", mh->reason);
    }
    cJSON_AddItemToObject(o, "models", rows);
    cJSON_AddStringToObject(o, "default", effective_default());
    cJSON_AddStringToObject(o, "preferred", C.default_model);
    if (!ok) {
        cJSON_AddStringToObject(o, "error", "AIUnavailable");
        cJSON_AddStringToObject(o, "message", M.error[0] ? M.error : "the model list is not available");
    }
    cJSON_AddItemToObject(o, "health", ai_health_json());
    cJSON_AddItemToObject(o, "limits", limits_json());
    cJSON_AddItemToObject(o, "server_tools", server_tool_names());
    cJSON_AddStringToObject(o, "api", ai_api_name(C.api));
    reply_cors_json(req, ok ? 200 : 503, o);
    cJSON_Delete(o);
}

static int model_hidden(const char *id) {
    for (int i = 0; i < C.nhide; i++) {
        if (!strcmp(C.hide[i], id)) return 1;
    }
    return 0;
}

static void models_chunk(Up *u, const char *data, const size_t n) {
    (void) u;
    if (M.raw.len + n <= AI_MAX_RAW) ai_buf_add(&M.raw, data, n);
}

static void models_done(Up *u) {
    M.fetching = 0;
    if (u->status == 200) {
        cJSON *list = ai_models_normalise(M.raw.data ? M.raw.data : "", M.raw.len);
        if (list) {
            for (int i = cJSON_GetArraySize(list) - 1; i >= 0; i--) {
                if (model_hidden(kn_json_str(cJSON_GetArrayItem(list, i), "id", ""))) cJSON_DeleteItemFromArray(list, i);
            }
            cJSON_Delete(M.models);
            M.models = list;
            M.fetched_ms = (int64_t) kn_now_ms();
            M.error[0] = 0;
            KN_INFO(MOD, "%d models from the upstream", cJSON_GetArraySize(list));
            if (!H.cycles) probe_cycle(g_b->loop, NULL); /* the first probes, now that the list is known */
        } else {
            copy_str(M.error, sizeof(M.error), "the AI provider's model list is not readable");
        }
    } else if (!u->status) {
        snprintf(M.error, sizeof(M.error), "the AI provider is unreachable (%.200s)", u->err);
    } else {
        char e[200];
        ai_error_text(M.raw.data, M.raw.len, e, sizeof(e));
        snprintf(M.error, sizeof(M.error), "the AI provider answered HTTP %d: %s", u->status, e);
    }
    if (M.error[0]) KN_WARN(MOD, "model list: %s", M.error);
    ai_buf_free(&M.raw);
    up_free(u);
    const int n = M.nwaiting;
    kn_http_req *w[AI_MAX_PENDING_MODELS];
    memcpy(w, M.waiting, sizeof(w[0]) * (size_t) n);
    M.nwaiting = 0;
    for (int i = 0; i < n; i++) models_reply(w[i]);
}

static int models_fresh(void) {
    return M.models && (C.models_ttl_s <= 0 || (int64_t) kn_now_ms() - M.fetched_ms < (int64_t) C.models_ttl_s * 1000);
}

static void models_fetch(void) {
    if (M.fetching || !ai_configured()) return;
    M.fetching = up_start("/models", NULL, 0, 30000, models_chunk, models_done, NULL) != NULL;
}

static void h_models(kn_http_req *req, void *ud) {
    (void) ud;
    if (!C.enabled) {
        reply_error(req, 503, "AIUnavailable", "AI is turned off on this server");
        return;
    }
    if (!ai_configured()) {
        reply_error(req, 503, "AIUnavailable", "the AI service is not configured on this server");
        return;
    }
    if (models_fresh()) {
        models_reply(req);
        return;
    }
    if (M.nwaiting >= AI_MAX_PENDING_MODELS) {
        if (M.models) models_reply(req); /* stale but useful */
        else reply_error(req, 503, "AIUnavailable", "busy, try again");
        return;
    }
    M.waiting[M.nwaiting++] = req;
    models_fetch();
    if (!M.fetching && M.nwaiting) { /* could not start */
        copy_str(M.error, sizeof(M.error), "cannot start the upstream request");
        const int n = M.nwaiting;
        M.nwaiting = 0;
        for (int i = 0; i < n; i++) models_reply(M.waiting[i]);
    }
}

static void h_status(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", C.enabled);
    cJSON_AddBoolToObject(o, "configured", ai_configured());
    cJSON_AddItemToObject(o, "health", ai_health_json());
    cJSON_AddItemToObject(o, "limits", limits_json());
    cJSON_AddStringToObject(o, "default", effective_default());
    cJSON_AddStringToObject(o, "preferred", C.default_model);
    cJSON_AddItemToObject(o, "server_tools", server_tool_names());
    reply_cors_json(req, 200, o);
    cJSON_Delete(o);
}

/* ------------------------------------------------------------------ per-account limits */

typedef struct Lim {
    int64_t id;
    int64_t day;           /* UTC day of the counters */
    int requests;          /* today */
    int64_t tokens;        /* today */
    int active;            /* streams now */
    uint64_t stamps[AI_RPM_CAP]; /* request times (monotonic ms), ring */
    int head, count;
} Lim;

static Lim **g_lims;
static size_t g_nlims, g_caplims;

static Lim *lim_get(const int64_t id) {
    for (size_t i = 0; i < g_nlims; i++) {
        if (g_lims[i]->id == id) return g_lims[i];
    }
    if (g_nlims == g_caplims) {
        const size_t cap = g_caplims ? g_caplims * 2 : 64;
        Lim **grown = realloc(g_lims, cap * sizeof(*grown));
        if (!grown) return NULL;
        g_lims = grown;
        g_caplims = cap;
    }
    Lim *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->id = id;
    g_lims[g_nlims++] = l;
    return l;
}

static void lim_roll(Lim *l) {
    const int64_t day = kn_wall_ms() / 86400000;
    if (l->day != day) {
        l->day = day;
        l->requests = 0;
        l->tokens = 0;
    }
}

/* 0 ok (the request is counted), else a 429 message in msg. */
static int lim_admit(Lim *l, char *msg, const size_t n, int *retry_s) {
    lim_roll(l);
    const uint64_t now = kn_now_ms();
    while (l->count && now - l->stamps[(l->head - l->count + AI_RPM_CAP) % AI_RPM_CAP] >= 60000) l->count--;
    *retry_s = 0;
    if (C.lim_concurrent > 0 && l->active >= C.lim_concurrent) {
        snprintf(msg, n, "%d AI replies are already running for your account: wait for one to finish", l->active);
        *retry_s = 2;
        return -1;
    }
    if (C.lim_rpm > 0 && l->count >= C.lim_rpm) {
        const uint64_t oldest = l->stamps[(l->head - l->count + AI_RPM_CAP) % AI_RPM_CAP];
        *retry_s = (int) ((60000 - (now - oldest)) / 1000) + 1;
        snprintf(msg, n, "Too many AI requests this minute (limit %d): try again in %d s", C.lim_rpm, *retry_s);
        return -1;
    }
    if (C.lim_daily > 0 && l->requests >= C.lim_daily) {
        snprintf(msg, n, "Daily AI request limit reached (%d): it resets at 00:00 UTC", C.lim_daily);
        *retry_s = (int) ((86400000 - kn_wall_ms() % 86400000) / 1000);
        return -1;
    }
    if (C.lim_daily_tokens > 0 && l->tokens >= C.lim_daily_tokens) {
        snprintf(msg, n, "Daily AI token budget used up (%lld tokens): it resets at 00:00 UTC", (long long) C.lim_daily_tokens);
        *retry_s = (int) ((86400000 - kn_wall_ms() % 86400000) / 1000);
        return -1;
    }
    l->stamps[l->head] = now;
    l->head = (l->head + 1) % AI_RPM_CAP;
    if (l->count < AI_RPM_CAP) l->count++;
    l->requests++;
    l->active++;
    return 0;
}

static cJSON *lim_usage_json(const Lim *l) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "requests_today", l ? l->requests : 0);
    cJSON_AddNumberToObject(o, "tokens_today", l ? (double) l->tokens : 0);
    cJSON_AddNumberToObject(o, "daily", C.lim_daily);
    cJSON_AddNumberToObject(o, "daily_tokens", (double) C.lim_daily_tokens);
    return o;
}

/* ------------------------------------------------------------------ /ai/chat */

typedef struct Chat {
    kn_http_req *req;
    int64_t account_id;
    char account[64];
    Lim *lim;
    cJSON *body;           /* the client's request (owned) */
    cJSON *messages;       /* the conversation sent upstream (owned) */
    cJSON *appended;       /* what this turn added (owned): assistant messages + server tool results */
    cJSON *tools;          /* client tools + server tools (owned, may be NULL) */
    char model[96];
    char effort[16];
    int max_tokens;
    int stream;            /* SSE to the client */
    int web;               /* server tools wanted */
    int sse_started;
    int round;
    AiStream st;
    Up *up;
    AiBuf raw;             /* non-SSE / error upstream body */
    int upstream_sse;
    kn_timer_id ping;
    int64_t prompt_tokens, completion_tokens, reasoning_tokens, cached_tokens;
    char finish[32];
    char model_seen[96];
    int pending_tools;     /* server tool calls running */
    cJSON *tool_results;   /* results of the round's server tools, in call order */
    uint64_t t0;
    int finished;
} Chat;

static void chat_free(Chat *c) {
    if (c->ping) kn_loop_cancel_timer(g_b->loop, c->ping);
    if (c->lim && c->lim->active > 0) c->lim->active--;
    cJSON_Delete(c->body);
    cJSON_Delete(c->messages);
    cJSON_Delete(c->appended);
    cJSON_Delete(c->tools);
    cJSON_Delete(c->tool_results);
    ai_stream_free(&c->st);
    ai_buf_free(&c->raw);
    free(c);
}

static int chat_alive(Chat *c) { return kn_http_req_alive(c->req); }

static void chat_send(Chat *c, char *sse) {
    if (!sse) return;
    if (c->stream && c->sse_started && chat_alive(c)) kn_http_stream_write(c->req, sse, strlen(sse));
    free(sse);
}

static void chat_ping(kn_loop *loop, void *ud) {
    (void) loop;
    Chat *c = ud;
    if (c->sse_started && chat_alive(c)) kn_http_stream_write(c->req, ": ping\n\n", 8);
    else if (c->up) atomic_store(&c->up->cancel, 1);
}

static cJSON *chat_usage(const Chat *c) {
    cJSON *u = cJSON_CreateObject();
    cJSON_AddNumberToObject(u, "prompt_tokens", (double) c->prompt_tokens);
    cJSON_AddNumberToObject(u, "completion_tokens", (double) c->completion_tokens);
    if (c->reasoning_tokens) cJSON_AddNumberToObject(u, "reasoning_tokens", (double) c->reasoning_tokens);
    if (c->cached_tokens) cJSON_AddNumberToObject(u, "cached_tokens", (double) c->cached_tokens);
    return u;
}

/* The end of a /ai/chat request: error (code + message) or the turn's result. Frees c. */
static void chat_end(Chat *c, const char *code, const char *message) {
    if (c->finished) return;
    c->finished = 1;
    if (c->lim) c->lim->tokens += c->prompt_tokens + c->completion_tokens;
    const double secs = (double) (kn_now_ms() - c->t0) / 1000.0;
    const cJSON *row = model_row(c->model);
    const double cost = ai_usage_cost(row, c->prompt_tokens, c->completion_tokens);
    if (code) KN_INFO(MOD, "chat #%lld %s: %s (%s)", (long long) c->account_id, c->model, code, message ? message : "");
    else
        KN_INFO(MOD, "chat #%lld %s: %d round(s), %lld+%lld tokens, %.1f s, %s", (long long) c->account_id, c->model, c->round,
                (long long) c->prompt_tokens, (long long) c->completion_tokens, secs, c->finish);
    cJSON *done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "finish", code ? "error" : c->finish[0] ? c->finish : "stop");
    cJSON_AddStringToObject(done, "model", c->model_seen[0] ? c->model_seen : c->model);
    cJSON_AddItemToObject(done, "usage", chat_usage(c));
    if (cost >= 0) cJSON_AddNumberToObject(done, "cost_usd", cost);
    cJSON_AddNumberToObject(done, "seconds", secs);
    cJSON_AddNumberToObject(done, "rounds", c->round);
    cJSON_AddItemToObject(done, "messages", c->appended ? cJSON_Duplicate(c->appended, 1) : cJSON_CreateArray());
    cJSON_AddItemToObject(done, "account", lim_usage_json(c->lim));
    if (code) {
        cJSON_AddStringToObject(done, "error", code);
        cJSON_AddStringToObject(done, "message", message ? message : "");
    }
    if (c->stream && c->sse_started) {
        if (code) chat_send(c, ai_sse_error(code, message));
        else {
            cJSON *usage = chat_usage(c);
            chat_send(c, ai_sse_finish(c->finish[0] ? c->finish : "stop", usage, c->model_seen[0] ? c->model_seen : c->model));
            cJSON_Delete(usage);
        }
        chat_send(c, ai_sse_event("kn.done", done));
        chat_send(c, strdup(AI_SSE_DONE));
        kn_http_stream_end(c->req);
    } else if (!chat_alive(c)) {
        kn_http_respond_error(c->req, 499, "gone");
    } else {
        cJSON_AddBoolToObject(done, "success", code == NULL);
        const cJSON *last = cJSON_GetArrayItem(c->appended, cJSON_GetArraySize(c->appended) - 1);
        if (last) cJSON_AddItemToObject(done, "message", cJSON_Duplicate(last, 1));
        int status = 200;
        if (code) status = !strcmp(code, "UpstreamError") || !strcmp(code, "ModelUnavailable") ? 502 : !strcmp(code, "Timeout") ? 504 : 500;
        reply_cors_json(c->req, status, done);
    }
    cJSON_Delete(done);
    chat_free(c);
}

static void chat_delta(void *user, const int kind, const char *text, const size_t len) {
    Chat *c = user;
    if (kind == AI_DELTA_TOOL) {
        cJSON *o = cJSON_CreateObject();
        char name[160];
        snprintf(name, sizeof(name), "%.*s", (int) len, text);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddBoolToObject(o, "server", g_server_tools && g_server_tools->owns(name));
        chat_send(c, ai_sse_event("kn.tool_start", o));
        cJSON_Delete(o);
        return;
    }
    chat_send(c, ai_sse_delta(kind, text, len));
}

static void chat_round(Chat *c);

static void chat_chunk(Up *u, const char *data, const size_t n) {
    Chat *c = u->ud;
    if (!chat_alive(c)) {
        atomic_store(&u->cancel, 1);
        return;
    }
    if (!c->raw.len && !c->st.events && u->hstatus < 400 && strstr(u->ctype, "event-stream")) c->upstream_sse = 1;
    if (c->upstream_sse) ai_stream_feed(&c->st, data, n);
    else if (c->raw.len + n <= AI_MAX_RAW) ai_buf_add(&c->raw, data, n);
}

static void tool_done(void *ud, const int is_error, const char *result);

typedef struct ToolCallCtx {
    Chat *c;
    int index;
    char id[96];
    char name[128];
    uint64_t t0;
} ToolCallCtx;

static int server_owns(const char *name) { return g_server_tools && g_server_tools->owns && g_server_tools->owns(name); }

/* The round's answer is complete: record it, then either run server tools and go again, or end. */
static void chat_round_done(Chat *c) {
    const AiStream *s = &c->st;
    if (s->has_usage) {
        c->prompt_tokens += s->prompt_tokens;
        c->completion_tokens += s->completion_tokens;
        c->reasoning_tokens += s->reasoning_tokens;
        c->cached_tokens += s->cached_tokens;
    }
    if (s->model[0]) copy_str(c->model_seen, sizeof(c->model_seen), s->model);
    copy_str(c->finish, sizeof(c->finish), s->finish[0] ? s->finish : "stop");
    cJSON *msg = ai_stream_message(s);
    cJSON_AddItemToArray(c->messages, cJSON_Duplicate(msg, 1));
    cJSON_AddItemToArray(c->appended, msg);
    if (s->ncalls) chat_send(c, ai_sse_tool_calls(s));

    /* server tools: when every call of the answer is one, run them and ask again */
    int server = 0;
    for (int i = 0; i < s->ncalls; i++) server += s->calls[i].name[0] && server_owns(s->calls[i].name);
    if (server && c->round >= C.max_rounds) {
        copy_str(c->finish, sizeof(c->finish), "max_rounds"); /* the editor cannot run server tools */
        chat_end(c, NULL, NULL);
        return;
    }
    if (server && chat_alive(c)) {
        c->pending_tools = server;
        cJSON_Delete(c->tool_results);
        c->tool_results = cJSON_CreateArray();
        /* reserve result slots in call order (the tool messages must follow the call order) */
        for (int i = 0; i < s->ncalls; i++) {
            if (s->calls[i].name[0] && server_owns(s->calls[i].name)) cJSON_AddItemToArray(c->tool_results, cJSON_CreateNull());
        }
        int slot = 0;
        /* copy what the callbacks need: they may run synchronously */
        ToolCallCtx *ctxs[AI_MAX_TOOL_CALLS];
        char *args[AI_MAX_TOOL_CALLS];
        int nctx = 0;
        for (int i = 0; i < s->ncalls; i++) {
            const AiToolCall *tc = &s->calls[i];
            if (!tc->name[0] || !server_owns(tc->name)) continue;
            ToolCallCtx *t = calloc(1, sizeof(*t));
            if (!t) continue;
            t->c = c;
            t->index = slot++;
            if (tc->id[0]) copy_str(t->id, sizeof(t->id), tc->id);
            else snprintf(t->id, sizeof(t->id), "call_%d", i);
            copy_str(t->name, sizeof(t->name), tc->name);
            t->t0 = kn_now_ms();
            cJSON *ev = cJSON_CreateObject();
            cJSON_AddStringToObject(ev, "id", t->id);
            cJSON_AddStringToObject(ev, "name", t->name);
            cJSON_AddStringToObject(ev, "arguments", tc->args.len ? tc->args.data : "{}");
            cJSON_AddStringToObject(ev, "status", "running");
            chat_send(c, ai_sse_event("kn.server_tool", ev));
            cJSON_Delete(ev);
            ctxs[nctx] = t;
            args[nctx] = strdup(tc->args.len ? tc->args.data : "{}");
            nctx++;
        }
        c->pending_tools = nctx;
        if (!nctx) {
            chat_end(c, "Internal", "server tools could not start");
            return;
        }
        /* mixed answers: the editor's calls go back to it once the server ones are done (tool_done) */
        for (int i = 0; i < nctx; i++) {
            g_server_tools->call(ctxs[i]->name, args[i] ? args[i] : "{}", tool_done, ctxs[i]);
            free(args[i]);
        }
        return;
    }
    chat_end(c, NULL, NULL);
}

static void tool_done(void *ud, const int is_error, const char *result) {
    ToolCallCtx *t = ud;
    Chat *c = t->c;
    const double ms = (double) (kn_now_ms() - t->t0);
    char *clip = NULL;
    const char *text = result ? result : "";
    if (strlen(text) > AI_TOOL_RESULT_MAX) {
        clip = malloc(AI_TOOL_RESULT_MAX + 64);
        if (clip) {
            snprintf(clip, AI_TOOL_RESULT_MAX + 64, "%.*s\n[... cut at %u bytes]", (int) AI_TOOL_RESULT_MAX, text, (unsigned) AI_TOOL_RESULT_MAX);
            text = clip;
        }
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "tool");
    cJSON_AddStringToObject(m, "tool_call_id", t->id);
    AiBuf content = {0};
    if (is_error) ai_buf_add(&content, "Error: ", 7);
    ai_buf_add(&content, text, strlen(text));
    cJSON_AddStringToObject(m, "content", content.data ? content.data : "");
    ai_buf_free(&content);
    cJSON_ReplaceItemInArray(c->tool_results, t->index, m);
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "id", t->id);
    cJSON_AddStringToObject(ev, "name", t->name);
    cJSON_AddStringToObject(ev, "status", is_error ? "error" : "done");
    cJSON_AddStringToObject(ev, "result", text);
    cJSON_AddNumberToObject(ev, "ms", ms);
    chat_send(c, ai_sse_event("kn.server_tool", ev));
    cJSON_Delete(ev);
    free(clip);
    free(t);
    if (--c->pending_tools > 0) return;
    /* every server tool answered: append the results; continue unless the editor has calls too */
    const cJSON *r;
    cJSON_ArrayForEach(r, c->tool_results) {
        cJSON_AddItemToArray(c->messages, cJSON_Duplicate(r, 1));
        cJSON_AddItemToArray(c->appended, cJSON_Duplicate(r, 1));
    }
    int client = 0;
    for (int i = 0; i < c->st.ncalls; i++) client += c->st.calls[i].name[0] && !server_owns(c->st.calls[i].name);
    if (client || !chat_alive(c)) {
        copy_str(c->finish, sizeof(c->finish), "tool_calls");
        chat_end(c, NULL, NULL);
        return;
    }
    chat_round(c);
}

static void chat_up_done(Up *u) {
    Chat *c = u->ud;
    c->up = NULL;
    const int status = u->status;
    char err[256];
    copy_str(err, sizeof(err), u->err);
    const int sse = c->upstream_sse;
    up_free(u);
    if (!chat_alive(c)) {
        chat_end(c, "Cancelled", "the client went away");
        return;
    }
    if (!status) {
        char m[320];
        snprintf(m, sizeof(m), "The AI provider is unreachable (%s)", err);
        chat_end(c, strstr(err, "imeout") || strstr(err, "timed out") ? "Timeout" : "UpstreamError", m);
        return;
    }
    if (status >= 400) {
        char e[240], m[320];
        ai_error_text(c->raw.data, c->raw.len, e, sizeof(e));
        if (status < 500 && status != 401 && status != 403 && status != 429) { /* the model's own refusal (not key, rate or server trouble) */
            model_failed(c->model, e);
            snprintf(m, sizeof(m), "This model is unavailable: %s", e[0] ? e : "the provider refused it");
            chat_end(c, "ModelUnavailable", m);
            return;
        }
        snprintf(m, sizeof(m), "The AI provider answered HTTP %d: %s", status, e);
        chat_end(c, status == 429 ? "UpstreamBusy" : "UpstreamError", m);
        return;
    }
    if (sse) ai_stream_finish(&c->st);
    else if (!ai_parse_response(&c->st, c->raw.data ? c->raw.data : "", c->raw.len)) {
        chat_end(c, "UpstreamError", "The AI provider's answer is not readable");
        return;
    } else if (c->stream) {
        /* a non-streamed upstream answer: relay it in one piece */
        if (c->st.reasoning.len) chat_send(c, ai_sse_delta(AI_DELTA_REASONING, c->st.reasoning.data, c->st.reasoning.len));
        if (c->st.text.len) chat_send(c, ai_sse_delta(AI_DELTA_TEXT, c->st.text.data, c->st.text.len));
    }
    if (c->st.error[0]) {
        char m[600];
        model_failed(c->model, c->st.error);
        snprintf(m, sizeof(m), "This model is unavailable: %s", c->st.error);
        chat_end(c, "ModelUnavailable", m);
        return;
    }
    model_answered(c->model);
    chat_round_done(c);
}

static void chat_round(Chat *c) {
    c->round++;
    ai_stream_free(&c->st);
    ai_stream_init(&c->st, chat_delta, c);
    ai_buf_free(&c->raw);
    c->upstream_sse = 0;
    char *body = ai_request_body(C.api, c->model, c->messages, c->tools, c->effort, 1, c->max_tokens);
    if (!body) {
        chat_end(c, "Internal", "out of memory");
        return;
    }
    if (c->round > 1) {
        cJSON *ev = cJSON_CreateObject();
        cJSON_AddNumberToObject(ev, "round", c->round);
        chat_send(c, ai_sse_event("kn.round", ev));
        cJSON_Delete(ev);
    }
    c->up = up_start(ai_api_path(C.api), body, 1, C.timeout_ms, chat_chunk, chat_up_done, c);
    if (!c->up) chat_end(c, "Internal", "cannot start the upstream request");
}

static int valid_model_id(const char *s) {
    if (!s || !*s || strlen(s) >= 96) return 0;
    for (; *s; s++) {
        if (!isalnum((unsigned char) *s) && !strchr("._:/@-", *s)) return 0;
    }
    return 1;
}

/* The client's tools (chat shape) plus the server tools (web search) when wanted. */
static cJSON *chat_tools(const cJSON *client_tools, const int web, char *err, const size_t n) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *t;
    int count = 0;
    cJSON_ArrayForEach(t, client_tools) {
        const cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
        const char *name = kn_json_str(fn, "name", NULL);
        if (!cJSON_IsObject(fn) || !name || !name[0]) continue;
        if (server_owns(name)) continue; /* reserved names */
        if (++count > 128) {
            snprintf(err, n, "too many tools (max 128)");
            cJSON_Delete(out);
            return NULL;
        }
        cJSON_AddItemToArray(out, cJSON_Duplicate(t, 1));
    }
    if (web && g_server_tools && g_server_tools->defs) {
        cJSON *defs = g_server_tools->defs();
        cJSON *d;
        while (defs && (d = cJSON_DetachItemFromArray(defs, 0))) cJSON_AddItemToArray(out, d);
        cJSON_Delete(defs);
    }
    if (!cJSON_GetArraySize(out)) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

static void chat_start(Chat *c) {
    const cJSON *body = c->body;
    const cJSON *msgs = cJSON_GetObjectItemCaseSensitive(body, "messages");
    const int count = cJSON_GetArraySize(msgs);
    if (!cJSON_IsArray(msgs) || count < 1) {
        reply_error(c->req, 400, "BadRequest", "messages: a non-empty array is required");
        chat_free(c);
        return;
    }
    if (count > C.max_messages) {
        char m[96];
        snprintf(m, sizeof(m), "too many messages (max %d): start a new conversation", C.max_messages);
        reply_error(c->req, 400, "BadRequest", m);
        chat_free(c);
        return;
    }
    const cJSON *m;
    cJSON_ArrayForEach(m, msgs) {
        const char *role = kn_json_str(m, "role", "");
        if (strcmp(role, "system") && strcmp(role, "user") && strcmp(role, "assistant") && strcmp(role, "tool")) {
            reply_error(c->req, 400, "BadRequest", "messages: role must be system, user, assistant or tool");
            chat_free(c);
            return;
        }
    }
    const char *model = kn_json_str(body, "model", "");
    if (!model[0]) model = effective_default();
    if (!valid_model_id(model) || (M.models && !model_row(model))) {
        reply_error(c->req, 400, "UnknownModel", "unknown model: pick one from the list");
        chat_free(c);
        return;
    }
    copy_str(c->model, sizeof(c->model), model);
    const char *effort = kn_json_str(body, "effort", "");
    if (!strcmp(effort, "low") || !strcmp(effort, "medium") || !strcmp(effort, "high")) copy_str(c->effort, sizeof(c->effort), effort);
    int mt = (int) kn_json_int(body, "max_tokens", 0);
    const cJSON *row = model_row(c->model);
    const int model_max = (int) kn_json_int(row, "max_tokens", 0);
    if (mt <= 0 || mt > C.max_tokens) mt = C.max_tokens;
    if (model_max > 0 && mt > model_max) mt = model_max;
    c->max_tokens = mt;
    c->stream = kn_json_bool(body, "stream", 1);
    c->web = kn_json_bool(body, "web_search", 0);
    char err[128] = "";
    c->tools = chat_tools(cJSON_GetObjectItemCaseSensitive(body, "tools"), c->web, err, sizeof(err));
    if (err[0]) {
        reply_error(c->req, 400, "BadRequest", err);
        chat_free(c);
        return;
    }
    c->messages = cJSON_Duplicate(msgs, 1);
    c->appended = cJSON_CreateArray();

    char lm[200];
    int retry = 0;
    c->lim = lim_get(c->account_id);
    if (!c->lim) {
        reply_error(c->req, 500, "Internal", "out of memory");
        chat_free(c);
        return;
    }
    if (lim_admit(c->lim, lm, sizeof(lm), &retry)) {
        Lim *l = c->lim;
        c->lim = NULL; /* not admitted: nothing to release */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", 0);
        cJSON_AddStringToObject(o, "error", "RateLimited");
        cJSON_AddStringToObject(o, "message", lm);
        cJSON_AddNumberToObject(o, "retry_after_s", retry);
        cJSON_AddItemToObject(o, "account", lim_usage_json(l));
        char ra[16];
        snprintf(ra, sizeof(ra), "%d", retry > 0 ? retry : 1);
        kn_http_set_header(c->req, "Retry-After", ra);
        reply_cors_json(c->req, 429, o);
        cJSON_Delete(o);
        chat_free(c);
        return;
    }
    c->t0 = kn_now_ms();
    if (c->stream) {
        brain_cors_headers(c->req);
        kn_http_set_header(c->req, "Cache-Control", "no-cache, no-store");
        kn_http_set_header(c->req, "X-Accel-Buffering", "no");
        if (kn_http_stream_begin(c->req, 200, "text/event-stream; charset=utf-8") < 0) {
            chat_end(c, "Cancelled", "the client went away");
            return;
        }
        c->sse_started = 1;
        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "model", c->model);
        cJSON_AddStringToObject(meta, "account", c->account);
        cJSON_AddItemToObject(meta, "health", ai_health_json());
        cJSON_AddItemToObject(meta, "server_tools", server_tool_names());
        chat_send(c, ai_sse_event("kn.meta", meta));
        cJSON_Delete(meta);
        c->ping = kn_loop_add_timer(g_b->loop, AI_PING_MS, AI_PING_MS, chat_ping, c);
    }
    chat_round(c);
}

static void chat_on_account(void *ud, const char *err, const cJSON *data) {
    Chat *c = ud;
    if (!kn_http_req_alive(c->req)) {
        kn_http_respond_error(c->req, 499, "gone");
        chat_free(c);
        return;
    }
    const int64_t id = !err && data ? kn_json_int(data, "id", 0) : 0;
    if (id <= 0) {
        reply_error(c->req, 401, "SignInRequired", "Your session expired: sign in again to use AI");
        chat_free(c);
        return;
    }
    if (kn_json_bool(data, "banned", 0)) {
        reply_error(c->req, 403, "Banned", "This account cannot use AI");
        chat_free(c);
        return;
    }
    c->account_id = id;
    copy_str(c->account, sizeof(c->account), kn_json_str(data, "username", ""));
    chat_start(c);
}

static void h_chat(kn_http_req *req, void *ud) {
    Brain *b = ud;
    if (!C.enabled || !ai_configured()) {
        reply_error(req, 503, "AIUnavailable", C.enabled ? "the AI service is not configured on this server" : "AI is turned off on this server");
        return;
    }
    cJSON *body = kn_http_body_json(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        reply_error(req, 400, "BadRequest", "a JSON object body is required");
        return;
    }
    char token[512] = "";
    copy_str(token, sizeof(token), kn_json_str(body, "token", ""));
    if (!token[0] && kn_http_query_get(req, "token", token, sizeof(token)) <= 0) token[0] = 0;
    if (!token[0]) {
        const char *bearer = kn_http_bearer(req);
        if (bearer) copy_str(token, sizeof(token), bearer);
    }
    if (!token[0]) {
        cJSON_Delete(body);
        reply_error(req, 401, "SignInRequired", "Sign in to use AI");
        return;
    }
    Chat *c = calloc(1, sizeof(*c));
    if (!c) {
        cJSON_Delete(body);
        reply_error(req, 500, "Internal", "out of memory");
        return;
    }
    c->req = req;
    c->body = body;
    cJSON_DeleteItemFromObjectCaseSensitive(c->body, "token");
    if (!models_fresh()) models_fetch(); /* validation and prices next time */
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(b, "token", args, chat_on_account, c);
    cJSON_Delete(args);
}

/* ------------------------------------------------------------------ bot banter (internal WS)
 *
 * One short chat line for a bot player (ai.h "Bot chat"). A non-streamed completion on the
 * effective default model with a tight system prompt and a small output cap; the first line of
 * the answer, unquoted and clipped, goes back as {text}. A brain-wide per-minute limit keeps
 * a busy fleet of game servers from spending the editor's budget (ai.banter.rpm). */

#define AI_BANTER_LIVE_MAX 8
#define AI_BANTER_RAW_MAX (64u << 10)
#define AI_BANTER_CHARS 70 /* NET_CHAT_MAX_CHARS */

typedef struct {
    AiBanterDoneFn done;
    void *ud;
    AiBuf raw;
    char model[96];
    char name[40];
} Banter;

static struct {
    uint64_t window_start;
    int window_count;
    int live;
} BN;

static const char *banter_personality_desc(const char *p) {
    if (!strcmp(p, "tryhard")) return "competitive and cocky, loves ez and 1 tap, never cruel";
    if (!strcmp(p, "salty")) return "grumpy, blames lag, spawns and the map, never cruel";
    if (!strcmp(p, "hype")) return "excited and loud, caps and exclamation marks";
    if (!strcmp(p, "goofy")) return "silly, absurd excuses, self-deprecating";
    if (!strcmp(p, "quiet")) return "a player of very few words, one or two words at most";
    return "relaxed, friendly, low-key";
}

/* first non-empty line, wrapping quotes / a "Name:" prefix dropped, clipped to AI_BANTER_CHARS code points */
static void banter_clean(const char *in, const char *name, char *out, const size_t size) {
    out[0] = 0;
    if (!in) return;
    const char *p = in;
    while (*p && isspace((unsigned char) *p)) p++;
    const char *end = p;
    while (*end && *end != '\n' && *end != '\r') end++;
    while (end > p && isspace((unsigned char) end[-1])) end--;
    const size_t nl = strlen(name);
    if (nl && (size_t) (end - p) > nl + 1 && !strncasecmp(p, name, nl) && p[nl] == ':') {
        p += nl + 1;
        while (p < end && isspace((unsigned char) *p)) p++;
    }
    while (end - p >= 2 && (*p == '"' || *p == '\'' || *p == '`') && end[-1] == *p) {
        p++;
        end--;
    }
    size_t o = 0, chars = 0;
    for (const char *c = p; c < end && o + 4 < size; c++) {
        const unsigned char b = (unsigned char) *c;
        if (b < 0x20) continue;
        if ((b & 0xc0) != 0x80 && chars++ >= AI_BANTER_CHARS) break;
        out[o++] = (char) b;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static void banter_chunk(Up *u, const char *data, const size_t n) {
    Banter *bn = u->ud;
    if (bn->raw.len + n <= AI_BANTER_RAW_MAX) ai_buf_add(&bn->raw, data, n);
}

static void banter_done(Up *u) {
    Banter *bn = u->ud;
    BN.live--;
    char text[256] = "", err[200] = "";
    if (!u->status) snprintf(err, sizeof(err), "unreachable (%.150s)", u->err);
    else if (u->status >= 400) {
        char e[160];
        ai_error_text(bn->raw.data, bn->raw.len, e, sizeof(e));
        snprintf(err, sizeof(err), "HTTP %d: %s", u->status, e);
    } else {
        AiStream s;
        ai_stream_init(&s, NULL, NULL);
        if (strstr(u->ctype, "event-stream")) {
            ai_stream_feed(&s, bn->raw.data ? bn->raw.data : "", bn->raw.len);
            ai_stream_finish(&s);
        } else ai_parse_response(&s, bn->raw.data ? bn->raw.data : "", bn->raw.len);
        if (s.error[0]) snprintf(err, sizeof(err), "%.190s", s.error);
        else {
            banter_clean(s.text.data, bn->name, text, sizeof(text));
            if (!text[0]) snprintf(err, sizeof(err), "empty answer");
        }
        ai_stream_free(&s);
    }
    cJSON *reply = cJSON_CreateObject();
    if (!err[0]) cJSON_AddStringToObject(reply, "text", text);
    KN_DEBUG(MOD, "bot banter %s: %s (%lld ms)", bn->name, err[0] ? err : text, (long long) (kn_now_ms() - u->t0));
    bn->done(bn->ud, err[0] ? err : NULL, reply);
    cJSON_Delete(reply);
    ai_buf_free(&bn->raw);
    free(bn);
    up_free(u);
}

void ai_bot_banter(Brain *b, const cJSON *frame, AiBanterDoneFn done, void *ud) {
    (void) b;
    if (!C.enabled || !ai_configured() || !C.banter_enabled) {
        done(ud, "AI unavailable", NULL);
        return;
    }
    const uint64_t now = kn_now_ms();
    if (now - BN.window_start >= 60000) {
        BN.window_start = now;
        BN.window_count = 0;
    }
    if (BN.window_count >= C.banter_rpm || BN.live >= AI_BANTER_LIVE_MAX) {
        done(ud, "rate limited", NULL);
        return;
    }
    const char *name = kn_json_str(frame, "name", "Bot");
    const char *personality = kn_json_str(frame, "personality", "chill");
    const char *event = kn_json_str(frame, "event", "win");
    const char *other = kn_json_str(frame, "other", "");
    const char *mode = kn_json_str(frame, "mode", "");
    const char *map = kn_json_str(frame, "map", "");
    const int kills = (int) kn_json_int(frame, "kills", 0), deaths = (int) kn_json_int(frame, "deaths", 0);
    const int place = (int) kn_json_int(frame, "place", 0), players = (int) kn_json_int(frame, "players", 0);

    char sys[640], user[512], where[160] = "", score[160] = "";
    snprintf(sys, sizeof(sys),
             "You write one chat message for a bot player in the browser shooter Krunker. You are %s, a %s player. "
             "Answer with the message only: one line, at most 60 characters, plain text, no quotes, no hashtags, no emoji, "
             "no links, no names other than the ones given, no profanity, no slurs, no insults about real people, "
             "casual gamer tone (gg, wp, lol, ez are fine).",
             name, banter_personality_desc(personality));
    if (mode[0] || map[0]) snprintf(where, sizeof(where), " Mode: %s%s%s.", mode[0] ? mode : "a match", map[0] ? " on " : "", map);
    if (players > 0 && place > 0) snprintf(score, sizeof(score), " You: %d kills, %d deaths, place %d of %d.", kills, deaths, place, players);
    else snprintf(score, sizeof(score), " You: %d kills, %d deaths.", kills, deaths);
    if (!strcmp(event, "kill")) snprintf(user, sizeof(user), "Event: you just killed %s.%s%s", other[0] ? other : "someone", where, score);
    else if (!strcmp(event, "death")) snprintf(user, sizeof(user), "Event: %s just killed you.%s%s", other[0] ? other : "someone", where, score);
    else if (!strcmp(event, "draw")) snprintf(user, sizeof(user), "Event: the match just ended in a draw.%s%s", where, score);
    else snprintf(user, sizeof(user), "Event: the match just ended and you %s.%s%s%s%s", !strcmp(event, "win") ? "won" : "lost", where,
                  other[0] ? " Best player: " : "", other, other[0] ? "." : "");
    cJSON *msgs = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", sys);
    cJSON_AddItemToArray(msgs, m);
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user);
    cJSON_AddItemToArray(msgs, m);
    const char *model = effective_default();
    const cJSON *row = model_row(model);
    const char *effort = row && cJSON_GetArraySize(cJSON_GetObjectItem(row, "efforts")) > 0 ? "low" : NULL;
    char *body = ai_request_body(C.api, model, msgs, NULL, effort, 0, C.banter_max_tokens);
    cJSON_Delete(msgs);
    Banter *bn = calloc(1, sizeof(*bn));
    if (!body || !bn) {
        free(body);
        free(bn);
        done(ud, "out of memory", NULL);
        return;
    }
    bn->done = done;
    bn->ud = ud;
    copy_str(bn->model, sizeof(bn->model), model);
    copy_str(bn->name, sizeof(bn->name), name);
    if (!up_start(ai_api_path(C.api), body, 0, C.banter_timeout_ms, banter_chunk, banter_done, bn)) {
        free(bn);
        done(ud, "cannot start the upstream request", NULL);
        return;
    }
    BN.live++;
    BN.window_count++;
}

/* ------------------------------------------------------------------ module */

static void config_load(const struct kn_config *cfg) {
    C.enabled = kn_config_bool(cfg, "ai.enabled", 1);
    const char *url = kn_config_str(cfg, "ai.upstream_url", "");
    copy_str(C.url, sizeof(C.url), url);
    size_t n = strlen(C.url);
    while (n && C.url[n - 1] == '/') C.url[--n] = 0;
    free(C.key);
    const char *key = kn_config_str(cfg, "ai.upstream_key", "");
    C.key = key && key[0] ? strdup(key) : NULL;
    C.api = ai_api_parse(kn_config_str(cfg, "ai.api", "chat"), AI_API_CHAT);
    copy_str(C.default_model, sizeof(C.default_model), kn_config_str(cfg, "ai.default_model", "glm-5.3-flash"));
    C.max_tokens = (int) kn_config_int(cfg, "ai.max_tokens", 4096);
    C.timeout_ms = (int) kn_config_int(cfg, "ai.timeout_ms", 300000);
    C.connect_timeout_ms = (int) kn_config_int(cfg, "ai.connect_timeout_ms", 10000);
    C.max_messages = (int) kn_config_int(cfg, "ai.max_messages", 400);
    C.max_rounds = (int) kn_config_int(cfg, "ai.max_rounds", 8);
    C.models_ttl_s = (int) kn_config_int(cfg, "ai.models_ttl_s", 600);
    C.health_interval_s = (int) kn_config_int(cfg, "ai.health_interval_s", 900);
    C.health_probe_max = (int) kn_config_int(cfg, "ai.health_probe_max", 6);
    if (C.health_probe_max < 1) C.health_probe_max = 1;
    C.lim_rpm = (int) kn_config_int(cfg, "ai.limit.rpm", 60);
    if (C.lim_rpm > AI_RPM_CAP) C.lim_rpm = AI_RPM_CAP;
    C.lim_daily = (int) kn_config_int(cfg, "ai.limit.daily", 3000);
    C.lim_daily_tokens = kn_config_int(cfg, "ai.limit.daily_tokens", 20000000);
    C.lim_concurrent = (int) kn_config_int(cfg, "ai.limit.concurrent", 3);
    snprintf(C.ua, sizeof(C.ua), "KrunkNative-brain/%s", KN_BRAIN_VERSION);
    C.banter_enabled = kn_config_bool(cfg, "ai.banter.enabled", 1);
    C.banter_rpm = (int) kn_config_int(cfg, "ai.banter.rpm", 120);
    C.banter_max_tokens = (int) kn_config_int(cfg, "ai.banter.max_tokens", 200);
    C.banter_timeout_ms = (int) kn_config_int(cfg, "ai.banter.timeout_ms", 8000);
    const char *hide[32];
    const size_t nh = kn_config_str_list(cfg, "ai.models_hide", hide, 32);
    C.nhide = 0;
    for (size_t i = 0; i < nh && i < 32; i++) copy_str(C.hide[C.nhide++], sizeof(C.hide[0]), hide[i]);
}

static void probe_first(kn_loop *loop, void *ud) {
    if (!H.cycles) probe_cycle(loop, ud);
}

/* every ai.health_interval_s: a fresh model list (new models, prices), then a probe cycle */
static void health_tick(kn_loop *loop, void *ud) {
    M.fetched_ms = 0;
    models_fetch();
    probe_cycle(loop, ud);
}

static void route2(Brain *b, const char *method, const char *path, kn_http_handler h) {
    char alias[64];
    kn_http_route(b->http, method, path, h, b);
    snprintf(alias, sizeof(alias), "/api%s", path);
    kn_http_route(b->http, method, alias, h, b);
}

int ai_init(Brain *b) {
    g_b = b;
    config_load(b->cfg);
#ifdef KN_HAVE_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    route2(b, "GET", "/ai/models", h_models);
    route2(b, "GET", "/ai/status", h_status);
    route2(b, "POST", "/ai/chat", h_chat);
    if (!C.enabled) {
        KN_INFO(MOD, "AI proxy off (ai.enabled)");
        return 0;
    }
    if (!ai_configured()) {
        KN_INFO(MOD, "AI proxy unconfigured: set KN_AI_UPSTREAM_URL and KN_AI_UPSTREAM_KEY (routes answer 503)");
        return 0;
    }
#ifndef KN_HAVE_CURL
    KN_WARN(MOD, "built without libcurl: only plain http:// upstreams would work, and none do (no TLS)");
#endif
    KN_INFO(MOD, "AI proxy on: %s (%s api), default model %s, limits %d/min %d/day %lld tokens/day, %d concurrent", C.url,
            ai_api_name(C.api), C.default_model, C.lim_rpm, C.lim_daily, (long long) C.lim_daily_tokens, C.lim_concurrent);
    srand((unsigned) (kn_wall_ms() ^ (int64_t) getpid()));
    H.state = AI_HEALTH_UNKNOWN;
    copy_str(H.model, sizeof(H.model), C.default_model);
    /* the first cycle starts when the model list arrives (models_done); without one, after 20 s */
    kn_loop_add_timer(b->loop, 20000, 0, probe_first, NULL);
    if (C.health_interval_s > 0)
        H.timer = kn_loop_add_timer(b->loop, (uint64_t) C.health_interval_s * 1000, (uint64_t) C.health_interval_s * 1000, health_tick, NULL);
    models_fetch();
    return 0;
}

void ai_shutdown(Brain *b) {
    (void) b;
    /* cancel the running transfers and give their threads a moment to leave */
    pthread_mutex_lock(&g_up_mu);
    for (int i = 0; i < 256; i++) {
        if (g_up_list[i]) atomic_store(&g_up_list[i]->cancel, 1);
    }
    pthread_mutex_unlock(&g_up_mu);
    for (int i = 0; i < 150 && atomic_load(&g_up_live) > 0; i++) {
        struct timespec ts = {0, 10 * 1000000L};
        nanosleep(&ts, NULL);
    }
    if (H.timer) kn_loop_cancel_timer(g_b->loop, H.timer);
    H.timer = 0;
    cJSON_Delete(M.models);
    M.models = NULL;
    for (size_t i = 0; i < g_nlims; i++) free(g_lims[i]);
    free(g_lims);
    g_lims = NULL;
    g_nlims = g_caplims = 0;
    if (C.key) memset(C.key, 0, strlen(C.key));
    free(C.key);
    C.key = NULL;
}
