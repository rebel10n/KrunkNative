// krunker.io real-money purchases: the request frames, their shapes and PayStation's page (client/include/krunker_checkout.h).
#include <krunker_checkout.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *frame_start(const char *name) {
    cJSON *f = cJSON_CreateArray();
    if (f) cJSON_AddItemToArray(f, cJSON_CreateString(name));
    return f;
}

static void add_num(cJSON *f, const double v) { cJSON_AddItemToArray(f, cJSON_CreateNumber(v)); }
static void add_str_or_null(cJSON *f, const char *s) { cJSON_AddItemToArray(f, s && s[0] ? cJSON_CreateString(s) : cJSON_CreateNull()); }
static void add_lang(cJSON *f, const char *lang) { cJSON_AddItemToArray(f, cJSON_CreateString(lang && lang[0] ? lang : "en")); }

// XsollaPopup(a, b): a PayStation method and a flag, passed on as given (null when absent)
static void add_method(cJSON *f, const KrunkerCheckoutOpts *o) {
    if (o && o->method >= 0) add_num(f, o->method);
    else cJSON_AddItemToArray(f, cJSON_CreateNull());
    add_str_or_null(f, o ? o->flag : NULL);
}

static void add_channel(cJSON *f) { cJSON_AddItemToArray(f, cJSON_CreateNull()); } // window.paymentsChannelId: null outside the MS PWA

cJSON *krunker_checkout_kr(const int pack, const KrunkerCheckoutOpts *o) {
    if (pack < 0) return NULL;
    cJSON *f = frame_start("xst"); // :212348
    add_num(f, pack);
    add_lang(f, o ? o->lang : NULL);
    add_method(f, o);
    add_channel(f);
    return f;
}

cJSON *krunker_checkout_product(const char *id, const int display, const int quantity, const char *gift, const KrunkerCheckoutOpts *o) {
    if (!id || !id[0]) return NULL;
    cJSON *f = frame_start("xpurchase"); // :212396
    cJSON_AddItemToArray(f, cJSON_CreateString(id));
    add_lang(f, o ? o->lang : NULL);
    add_method(f, o);
    add_channel(f);
    if (display >= 0) add_num(f, display);
    else cJSON_AddItemToArray(f, cJSON_CreateNull()); // a wild offer / username: display null
    add_num(f, quantity >= 1 ? quantity : 1);
    add_str_or_null(f, gift);
    return f;
}

static cJSON *gifted(const char *name, const int index, const char *gift, const KrunkerCheckoutOpts *o) {
    if (index < 0) return NULL;
    cJSON *f = frame_start(name);
    add_num(f, index);
    add_lang(f, o ? o->lang : NULL);
    add_method(f, o);
    add_channel(f);
    add_str_or_null(f, gift);
    return f;
}

cJSON *krunker_checkout_bundle(const int bundle, const char *gift, const KrunkerCheckoutOpts *o) { return gifted("xstb", bundle, gift, o); } // :212927
cJSON *krunker_checkout_premium(const int premium, const char *gift, const KrunkerCheckoutOpts *o) { return gifted("xstp", premium, gift, o); } // :220228

cJSON *krunker_checkout_bp_tier(const int tier, const char *gift, const char *lang) { // :212959 (no method: null, null)
    const KrunkerCheckoutOpts o = {.lang = lang, .method = -1, .flag = NULL};
    return gifted("xstbp", tier, gift, &o);
}

cJSON *krunker_checkout_bp_levels(const int levels, const char *lang) { // :213007
    if (levels < 1) return NULL;
    cJSON *f = frame_start("xstbpl");
    add_num(f, levels);
    add_lang(f, lang);
    cJSON_AddItemToArray(f, cJSON_CreateNull());
    cJSON_AddItemToArray(f, cJSON_CreateNull());
    add_channel(f);
    return f;
}

cJSON *krunker_checkout_invoice(const double invoice) { // :212028
    cJSON *f = frame_start("xsin");
    add_num(f, invoice);
    return f;
}

// Each frame's arguments: n number, s string, z null; "nz" = either.
static const struct {
    const char *name;
    const char *args[8];
    int n;
} k_shapes[] = {
    {"xst", {"n", "s", "nz", "sz", "sz"}, 5},
    {"xpurchase", {"s", "s", "nz", "sz", "sz", "nz", "n", "sz"}, 8},
    {"xstb", {"n", "s", "nz", "sz", "sz", "sz"}, 6},
    {"xstp", {"n", "s", "nz", "sz", "sz", "sz"}, 6},
    {"xstbp", {"n", "s", "z", "z", "sz", "sz"}, 6},
    {"xstbpl", {"n", "s", "z", "z", "sz"}, 5},
    {"xsin", {"ns"}, 1},
};

static int arg_ok(const cJSON *v, const char *types) {
    if (cJSON_IsNumber(v)) return strchr(types, 'n') != NULL;
    if (cJSON_IsString(v)) return strchr(types, 's') != NULL && strlen(v->valuestring) < 128;
    if (cJSON_IsNull(v)) return strchr(types, 'z') != NULL;
    return 0;
}

int krunker_checkout_frame_ok(const cJSON *frame) {
    const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!name) return 0;
    const int n = cJSON_GetArraySize(frame) - 1;
    for (size_t i = 0; i < sizeof(k_shapes) / sizeof(k_shapes[0]); i++) {
        if (strcmp(k_shapes[i].name, name)) continue;
        if (n != k_shapes[i].n) return 0;
        for (int a = 0; a < n; a++)
            if (!arg_ok(cJSON_GetArrayItem(frame, a + 1), k_shapes[i].args[a])) return 0;
        return 1;
    }
    return 0;
}

int krunker_checkout_label(const char *label) {
    static const char *const labels[] = {"xst", "pur", "ppr", "purprm", "purb", "purbp", "purbpl", "pErr", "uprm", "abd", "gbdr"};
    if (!label) return 0;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
        if (!strcmp(labels[i], label)) return 1;
    return 0;
}

int krunker_checkout_paystation_url(const char *token, const int sandbox, char *out, const size_t n) {
    if (!token || !token[0] || !out || n == 0) return 0;
    // getPaystationUrl() + $.param({access_token}): encodeURIComponent of the token
    int w = snprintf(out, n, "%s", sandbox ? "https://sandbox-secure.xsolla.com/paystation2/?access_token=" : "https://secure.xsolla.com/paystation2/?access_token=");
    if (w < 0 || (size_t) w >= n) return 0;
    size_t at = (size_t) w;
    for (const unsigned char *p = (const unsigned char *) token; *p; p++) {
        const int plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-_.!~*'()", *p);
        if (at + (plain ? 1 : 3) >= n) return 0;
        if (plain) out[at++] = (char) *p;
        else at += (size_t) snprintf(out + at, n - at, "%%%02X", *p);
    }
    out[at] = 0;
    return 1;
}

// a query parameter whose value is a secret (case-insensitive, the whole name)
static int secret_param(const char *name, const size_t len) {
    static const char *const names[] = {"access_token", "token", "at", "key", "secret", "code", "password", "session", "sid", "auth"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) != len) continue;
        size_t k = 0;
        while (k < len && (name[k] >= 'A' && name[k] <= 'Z' ? name[k] + 32 : name[k]) == names[i][k]) k++; // ASCII lower case
        if (k == len) return 1;
    }
    return 0;
}

void krunker_checkout_log_url(const char *url, char *out, const size_t n) {
    if (!out || n == 0) return;
    out[0] = 0;
    if (!url) return;
    size_t at = 0;
#define PUT(s, l)                                                     \
    do {                                                              \
        const size_t put_ = (l) < n - 1 - at ? (l) : n - 1 - at;       \
        memcpy(out + at, (s), put_);                                  \
        at += put_;                                                   \
    } while (0)
    const char *frag = strchr(url, '#');
    const char *end = frag ? frag : url + strlen(url);
    const char *q = memchr(url, '?', (size_t) (end - url));
    PUT(url, (size_t) ((q ? q : end) - url));
    if (q) {
        PUT("?", 1);
        const char *p = q + 1;
        while (p < end) {
            const char *amp = memchr(p, '&', (size_t) (end - p));
            const char *stop = amp ? amp : end;
            const char *eq = memchr(p, '=', (size_t) (stop - p));
            if (eq && secret_param(p, (size_t) (eq - p))) {
                PUT(p, (size_t) (eq - p + 1));
                PUT("(not logged)", 12);
            } else {
                PUT(p, (size_t) (stop - p));
            }
            if (amp) PUT("&", 1);
            p = amp ? amp + 1 : end;
        }
    }
    if (frag) PUT("#(not logged)", 13);
#undef PUT
    out[at] = 0;
}

int krunker_checkout_parse_catalog(const char *spec, int *display, char *ref, const size_t n) {
    if (!spec || strncmp(spec, "catalog:", 8) || !ref || n == 0) return 0;
    const char *bar = strrchr(spec + 8, '|');
    if (!bar || !bar[1] || strlen(bar + 1) >= n) return 0;
    *display = -1;
    char json[256];
    const size_t jl = (size_t) (bar - (spec + 8));
    if (jl > 0 && jl < sizeof(json)) {
        memcpy(json, spec + 8, jl);
        json[jl] = 0;
        cJSON *o = cJSON_Parse(json); // JSON.parse(catalog's object)
        const cJSON *d = cJSON_GetObjectItem(o, "display");
        if (cJSON_IsNumber(d) && d->valuedouble >= 0) *display = (int) d->valuedouble;
        cJSON_Delete(o);
    }
    snprintf(ref, n, "%s", bar + 1);
    return 1;
}

void krunker_checkout_wait_begin(KrunkerCheckoutWait *w, const double now, const int has_funds, const double funds) {
    memset(w, 0, sizeof(*w));
    w->active = 1;
    w->start_ms = now;
    w->next_poll_ms = now + KRUNKER_CHECKOUT_POLL_MS;
    w->has_baseline = has_funds != 0;
    w->baseline = has_funds ? funds : 0;
}

int krunker_checkout_wait_tick(KrunkerCheckoutWait *w, const double now) {
    if (!w->active) return KRUNKER_CHECKOUT_WAIT_IDLE;
    if (now - w->start_ms >= KRUNKER_CHECKOUT_WAIT_MS) {
        w->active = 0;
        return KRUNKER_CHECKOUT_WAIT_EXPIRED;
    }
    if (now < w->next_poll_ms) return KRUNKER_CHECKOUT_WAIT_IDLE;
    w->next_poll_ms = now + KRUNKER_CHECKOUT_POLL_MS;
    return KRUNKER_CHECKOUT_WAIT_POLL;
}

int krunker_checkout_wait_funds(KrunkerCheckoutWait *w, const double funds) {
    if (!w->active) return KRUNKER_CHECKOUT_WAIT_IDLE;
    if (!w->has_baseline) {
        w->has_baseline = 1;
        w->baseline = funds;
        return KRUNKER_CHECKOUT_WAIT_IDLE;
    }
    if (funds <= w->baseline) return KRUNKER_CHECKOUT_WAIT_IDLE;
    w->active = 0;
    return KRUNKER_CHECKOUT_WAIT_PAID;
}

void krunker_checkout_wait_end(KrunkerCheckoutWait *w) { w->active = 0; }
