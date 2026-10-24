// The comp webhook's transport in server_bin (server/include/server_webhook.h). One detached worker thread per
// post, at most WEBHOOK_INFLIGHT_MAX at once, with a 5 s connect and a 10 s transfer bound; the game loop only
// starts the thread. The URL is a player's (the comp host's), so it must pass comp_webhook_url_ok and every
// address libcurl connects to must be public (comp_webhook_ipv4_public / _ipv6_public): no loopback, private,
// link-local (cloud metadata) or other internal targets, also when a public name resolves to one. The log names
// the host only: a webhook URL usually carries its secret in the path.
//
// KRUNKNATIVE_COMP_WEBHOOK_INSECURE=1 (tests and local development only) also allows http:// and internal
// addresses, so a test can receive the post on 127.0.0.1.
#include <server_webhook.h>
#include <comp_webhook.h>
#include <http_client.h>
#include <server_log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#define WEBHOOK_INFLIGHT_MAX 4
#define WEBHOOK_CONNECT_MS 5000
#define WEBHOOK_TIMEOUT_MS 10000
#define WEBHOOK_BODY_MAX (64 * 1024)
#define WEBHOOK_REPLY_MAX 4096

typedef struct {
    char url[COMP_WEBHOOK_URL_MAX + 1];
    char host[COMP_WEBHOOK_URL_MAX + 1];
    char *body;
    int insecure;
} WebhookJob;

static pthread_mutex_t g_webhook_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_webhook_inflight;

static int webhook_addr_public(const struct sockaddr *sa) {
    if (!sa) return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *) sa;
        return comp_webhook_ipv4_public(ntohl(in->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) sa;
        return comp_webhook_ipv6_public(in6->sin6_addr.s6_addr);
    }
    return 0;
}

// the authority of an http(s) URL, for the log
static void webhook_host(char *out, const size_t n, const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    snprintf(out, n, "%.*s", (int) strcspn(p, "/?#"), p);
}

static void *webhook_main(void *arg) {
    WebhookJob *job = arg;
    HttpResult r;
    const int rc = http_post_checked(job->url, "application/json", job->body, WEBHOOK_CONNECT_MS, WEBHOOK_TIMEOUT_MS,
                                     WEBHOOK_REPLY_MAX, job->insecure, job->insecure ? NULL : webhook_addr_public, &r);
    if (rc) server_log_error("[webhook] match_end to %s failed: %s", job->host, r.error);
    else if (r.status < 200 || r.status >= 300) server_log_error("[webhook] match_end to %s: HTTP %d", job->host, r.status);
    else server_log("[webhook] match_end posted to %s (HTTP %d)", job->host, r.status);
    http_result_free(&r);
    cJSON_free(job->body);
    free(job);
    pthread_mutex_lock(&g_webhook_lock);
    g_webhook_inflight--;
    pthread_mutex_unlock(&g_webhook_lock);
    return NULL;
}

int server_webhook_post(const char *url, char *body) {
    const char *env = getenv("KRUNKNATIVE_COMP_WEBHOOK_INSECURE");
    const int insecure = env && !strcmp(env, "1");
    char host[COMP_WEBHOOK_URL_MAX + 1] = "";
    if (url) webhook_host(host, sizeof(host), url);
    const char *refusal = !url || !body                                   ? "nothing to post"
                          : strlen(url) > COMP_WEBHOOK_URL_MAX             ? "the URL is too long"
                          : strlen(body) > WEBHOOK_BODY_MAX                ? "the body is too large"
                          : insecure ? (strncmp(url, "https://", 8) && strncmp(url, "http://", 7) ? "not an http(s):// URL" : NULL)
                          : !comp_webhook_url_ok(url)                      ? "not a public https:// URL"
                                                                           : NULL;
    if (refusal) {
        server_log_error("[webhook] match_end to %s not sent: %s", host[0] ? host : "(none)", refusal);
        cJSON_free(body);
        return -1;
    }
    WebhookJob *job = calloc(1, sizeof(*job));
    if (!job) {
        cJSON_free(body);
        return -1;
    }
    snprintf(job->url, sizeof(job->url), "%s", url);
    snprintf(job->host, sizeof(job->host), "%s", host);
    job->body = body;
    job->insecure = insecure;
    pthread_mutex_lock(&g_webhook_lock);
    const int busy = g_webhook_inflight >= WEBHOOK_INFLIGHT_MAX;
    if (!busy) g_webhook_inflight++;
    pthread_mutex_unlock(&g_webhook_lock);
    if (busy) {
        server_log_error("[webhook] match_end to %s not sent: %d posts already in flight", host, WEBHOOK_INFLIGHT_MAX);
        cJSON_free(body);
        free(job);
        return -1;
    }
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int failed = pthread_create(&thread, &attr, webhook_main, job);
    pthread_attr_destroy(&attr);
    if (failed) {
        pthread_mutex_lock(&g_webhook_lock);
        g_webhook_inflight--;
        pthread_mutex_unlock(&g_webhook_lock);
        server_log_error("[webhook] match_end to %s not sent: no worker thread", host);
        cJSON_free(body);
        free(job);
        return -1;
    }
    return 0;
}
