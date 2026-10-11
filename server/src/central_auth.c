// Central auth hasJoined lookups (server/include/central_auth.h).
#include <central_auth.h>
#include <http_client.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CA_QUEUE_MAX 128

typedef struct {
    char url[1024];
    uint32_t rid;
} CaJob;

static pthread_mutex_t g_ca_lock = PTHREAD_MUTEX_INITIALIZER;
static CentralAuthResult g_ca_queue[CA_QUEUE_MAX];
static int g_ca_head, g_ca_count;

static void ca_push(const CentralAuthResult *r) {
    pthread_mutex_lock(&g_ca_lock);
    if (g_ca_count < CA_QUEUE_MAX) {
        g_ca_queue[(g_ca_head + g_ca_count) % CA_QUEUE_MAX] = *r;
        g_ca_count++;
    } else {
        cJSON_Delete(r->reply);
    }
    pthread_mutex_unlock(&g_ca_lock);
}

int central_auth_poll(CentralAuthResult *out) {
    pthread_mutex_lock(&g_ca_lock);
    const int have = g_ca_count > 0;
    if (have) {
        *out = g_ca_queue[g_ca_head];
        g_ca_head = (g_ca_head + 1) % CA_QUEUE_MAX;
        g_ca_count--;
    }
    pthread_mutex_unlock(&g_ca_lock);
    return have;
}

static void *ca_thread(void *arg) {
    CaJob *job = arg;
    CentralAuthResult r = {.rid = job->rid, .status = -1};
    HttpResult http;
    if (http_request("GET", job->url, NULL, NULL, 8000, 16384, &http)) {
        snprintf(r.error, sizeof(r.error), "auth service unreachable: %s", http.error);
    } else if (http.status == 200) {
        r.reply = http.body ? cJSON_Parse(http.body) : NULL;
        const cJSON *id = cJSON_GetObjectItem(r.reply, "id");
        if (cJSON_IsNumber(id) && id->valuedouble > 0) r.status = 1;
        else {
            cJSON_Delete(r.reply);
            r.reply = NULL;
            snprintf(r.error, sizeof(r.error), "auth service: malformed hasJoined reply");
        }
    } else if (http.status == 204 || (http.status >= 400 && http.status < 500 && http.status != 429)) {
        r.status = 0;
        snprintf(r.error, sizeof(r.error), "not joined (HTTP %d)", http.status);
    } else {
        snprintf(r.error, sizeof(r.error), "auth service: HTTP %d", http.status);
    }
    http_result_free(&http);
    ca_push(&r);
    free(job);
    return NULL;
}

int central_auth_has_joined(const char *auth_url, const uint32_t rid, const char *username, const char *server_hash, const char *ip) {
    CaJob *job = calloc(1, sizeof(*job));
    if (!job) return -1;
    char user[128], hash[160], addr[96];
    http_url_encode(user, sizeof(user), username);
    http_url_encode(hash, sizeof(hash), server_hash);
    http_url_encode(addr, sizeof(addr), ip);
    size_t base = strlen(auth_url);
    while (base && auth_url[base - 1] == '/') base--;
    snprintf(job->url, sizeof(job->url), "%.*s/auth/hasJoined?username=%s&serverHash=%s%s%s", (int) base, auth_url, user, hash,
             addr[0] ? "&ip=" : "", addr);
    job->rid = rid;
    pthread_t thread;
    if (pthread_create(&thread, NULL, ca_thread, job)) {
        free(job);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}
