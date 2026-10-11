// Blocking HTTP(S) client (server/include/http_client.h): libcurl when available,
// else a minimal HTTP/1.1 over a plain socket (http:// only, Connection: close).
#include <http_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef KRUNKNATIVE_HAVE_CURL
#include <curl/curl.h>
#include <pthread.h>

typedef struct {
    HttpResult *out;
    size_t max;
    int overflow;
} CurlSink;

static size_t http_curl_write(char *data, size_t size, size_t n, void *ud) {
    CurlSink *sink = ud;
    const size_t len = size * n;
    if (sink->out->body_len + len > sink->max) {
        sink->overflow = 1;
        return 0;
    }
    char *grown = realloc(sink->out->body, sink->out->body_len + len + 1);
    if (!grown) return 0;
    sink->out->body = grown;
    memcpy(grown + sink->out->body_len, data, len);
    sink->out->body_len += len;
    grown[sink->out->body_len] = 0;
    return len;
}

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void http_curl_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

int http_request(const char *method, const char *url, const char *content_type, const char *body, const int timeout_ms,
                 const size_t max_body, HttpResult *out) {
    memset(out, 0, sizeof(*out));
    pthread_once(&g_curl_once, http_curl_init);
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(out->error, sizeof(out->error), "curl init failed");
        return -1;
    }
    CurlSink sink = {out, max_body ? max_body : 65536, 0};
    struct curl_slist *headers = NULL;
    char ct[128];
    if (content_type) {
        snprintf(ct, sizeof(ct), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, ct);
    }
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "krunknative-server_bin");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long) timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long) timeout_ms);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        snprintf(out->error, sizeof(out->error), "%s", sink.overflow ? "response too large" : curl_easy_strerror(rc));
        out->status = 0;
        return -1;
    }
    out->status = (int) status;
    return 0;
}

#elif !defined(WIN32)
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int http_request(const char *method, const char *url, const char *content_type, const char *body, const int timeout_ms,
                 const size_t max_body, HttpResult *out) {
    memset(out, 0, sizeof(*out));
    if (strncmp(url, "http://", 7)) {
        snprintf(out->error, sizeof(out->error), "only http:// URLs (build without libcurl)");
        return -1;
    }
    const char *rest = url + 7;
    const size_t host_len = strcspn(rest, ":/");
    char host[256], port[8] = "80";
    snprintf(host, sizeof(host), "%.*s", (int) host_len, rest);
    if (rest[host_len] == ':') snprintf(port, sizeof(port), "%.*s", (int) strcspn(rest + host_len + 1, "/"), rest + host_len + 1);
    const char *path = strchr(rest, '/');
    if (!path) path = "/";

    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) || !res) {
        snprintf(out->error, sizeof(out->error), "cannot resolve %s", host);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        const struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, a->ai_addr, a->ai_addrlen)) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) {
        snprintf(out->error, sizeof(out->error), "cannot connect to %s:%s", host, port);
        return -1;
    }
    const size_t body_len = body ? strlen(body) : 0;
    char head[1024];
    const int n = snprintf(head, sizeof(head),
                           "%s %s HTTP/1.1\r\nHost: %s:%s\r\nUser-Agent: krunknative-server_bin\r\nAccept: application/json\r\n"
                           "Connection: close\r\n%s%s%sContent-Length: %zu\r\n\r\n",
                           method, path, host, port, content_type ? "Content-Type: " : "", content_type ? content_type : "",
                           content_type ? "\r\n" : "", body_len);
    int failed = n <= 0 || (size_t) n >= sizeof(head) || send(fd, head, (size_t) n, MSG_NOSIGNAL) != n ||
                 (body_len && send(fd, body, body_len, MSG_NOSIGNAL) != (ssize_t) body_len);
    char *buf = NULL;
    size_t len = 0;
    const size_t max = (max_body ? max_body : 65536) + 8192;
    while (!failed) {
        char chunk[8192];
        const ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got <= 0) break;
        if (len + (size_t) got > max) { failed = 2; break; }
        char *grown = realloc(buf, len + (size_t) got + 1);
        if (!grown) { failed = 1; break; }
        buf = grown;
        memcpy(buf + len, chunk, (size_t) got);
        len += (size_t) got;
        buf[len] = 0;
    }
    close(fd);
    const char *split = buf ? strstr(buf, "\r\n\r\n") : NULL;
    if (failed || !buf || !split || sscanf(buf, "HTTP/1.%*d %d", &out->status) != 1) {
        snprintf(out->error, sizeof(out->error), "%s", failed == 2 ? "response too large" : "bad HTTP response");
        free(buf);
        out->status = 0;
        return -1;
    }
    const size_t off = (size_t) (split + 4 - buf);
    out->body_len = len - off;
    memmove(buf, buf + off, out->body_len + 1);
    out->body = buf;
    return 0;
}

#else
int http_request(const char *method, const char *url, const char *content_type, const char *body, const int timeout_ms,
                 const size_t max_body, HttpResult *out) {
    (void) method; (void) url; (void) content_type; (void) body; (void) timeout_ms; (void) max_body;
    memset(out, 0, sizeof(*out));
    snprintf(out->error, sizeof(out->error), "HTTP client needs libcurl on Windows");
    return -1;
}
#endif

void http_result_free(HttpResult *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

void http_url_encode(char *out, const size_t size, const char *in) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *) (in ? in : ""); *p && n + 4 < size; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) out[n++] = (char) *p;
        else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    if (size) out[n < size ? n : size - 1] = 0;
}
