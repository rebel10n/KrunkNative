// Krunker v10.0.2's gapi client (_10449: social / players / clans calls) over the brain's social
// socket: ["gapi", rid, method, path, body] -> ["gapi", rid, status, {data} | {error}]
// (services/brain/social_gapi.c). v10 sends HTTPS requests to gapi.svc.krunker.io with a bearer
// token; KrunkNative's client already holds the authenticated social socket, so the requests ride
// on it (doc/v10/social.md). Callbacks run on the main thread from backend_poll().
//
// While krunker.io is the active account backend (backend_ui.h: an official krunker.io game), the
// same calls are v10's own: HTTPS to krunker.io's gapi (krunker_account_api(KRUNKER_SVC_API), v10's
// _10449 = new _10077(GAPI_URI), v10:27600) with the krunker.io account's fresh access token, the
// {data, error} answers unwrapped as v10's _10067 does (v10:26094). Only reads go there: GETs of the
// player / social / clan / Turf Wars groups the Social hub, the profile and the clan views use.
// Anything else (follow, vote, comment, post, gift, report, clan apply / settings, other groups) is
// not ported for the krunker.io account and is not sent at all (0: the caller's own failure path);
// the kn_brain socket is held meanwhile, so nothing reaches the Box Office account either.
#include <backend.h>
#include <backend_ui.h>
#include <krunker_account.h>
#include <v10_social.h>
#include <i18n.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAPI_PENDING 64

static struct {
    int64_t rid;
    V10GapiFn fn;
    void *user;
} g_pending[GAPI_PENDING];
static int64_t g_next_rid = 1;
static int g_listening;

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "__close")) { // the socket dropped: every pending request fails
        for (int i = 0; i < GAPI_PENDING; i++) {
            if (!g_pending[i].fn) continue;
            const V10GapiFn fn = g_pending[i].fn;
            void *u = g_pending[i].user;
            g_pending[i].fn = NULL;
            fn(u, 0, NULL, NULL);
        }
        return;
    }
    if (strcmp(label, "gapi") != 0) return;
    const cJSON *rid = cJSON_GetArrayItem(frame, 1);
    if (!cJSON_IsNumber(rid)) return;
    for (int i = 0; i < GAPI_PENDING; i++) {
        if (!g_pending[i].fn || g_pending[i].rid != (int64_t) rid->valuedouble) continue;
        const V10GapiFn fn = g_pending[i].fn;
        void *u = g_pending[i].user;
        g_pending[i].fn = NULL;
        const cJSON *status = cJSON_GetArrayItem(frame, 2), *payload = cJSON_GetArrayItem(frame, 3);
        const cJSON *data = cJSON_GetObjectItem(payload, "data"), *error = cJSON_GetObjectItem(payload, "error");
        if (cJSON_IsNull(data)) data = NULL;
        if (cJSON_IsNull(error)) error = NULL;
        fn(u, cJSON_IsNumber(status) ? status->valueint : 200, data, error);
        return;
    }
}

// ---- krunker.io's gapi (the krunker.io account's real data)

#define KIO_PENDING 32
static struct {
    unsigned id; // 0 = free
    V10GapiFn fn;
    void *user;
} g_kio[KIO_PENDING];
static unsigned g_kio_ids;

// the reads v10 makes on _10449 from the Social hub, the profile, the clan view and Turf Wars
static int kio_readable(const char *method, const char *path) {
    // the reads v10's own pages make of gapi (v10 getNews, getUpcomingEvents, getTopStreams); nothing that changes an account
    static const char *const groups[] = {"/players/", "/social/", "/clans/", "/territorial-wars/", "/creators/", "/streams/"};
    if (!method || strcmp(method, "GET") != 0) return 0;
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
        if (!strncmp(path, groups[i], strlen(groups[i]))) return 1;
    return 0;
}

// _10067: 204 / 304 -> no data; 429 -> "Rate limit exceeded. Try again later."; < 200 or >= 500 or a
// body that is not JSON -> v10 throws (the callers' catch: their fallback text); else {data, error}
static void kio_done(void *user, const int status, const cJSON *res) {
    const unsigned id = (unsigned) (uintptr_t) user;
    for (int i = 0; i < KIO_PENDING; i++) {
        if (g_kio[i].id != id) continue;
        const V10GapiFn fn = g_kio[i].fn;
        void *u = g_kio[i].user;
        g_kio[i].id = 0;
        g_kio[i].fn = NULL;
        if (status == 204 || status == 304) {
            fn(u, status, NULL, NULL);
        } else if (status == 429) {
            cJSON *e = cJSON_CreateString("Rate limit exceeded. Try again later.");
            fn(u, status, NULL, e);
            cJSON_Delete(e);
        } else if (status < 200 || status >= 500 || !cJSON_IsObject(res)) {
            cJSON *e = status > 0 && status < 500 && !cJSON_IsObject(res) ? cJSON_CreateString("Invalid JSON response from server") : NULL;
            fn(u, status > 0 && e ? 500 : status, NULL, e);
            cJSON_Delete(e);
        } else {
            const cJSON *data = cJSON_GetObjectItem(res, "data"), *error = cJSON_GetObjectItem(res, "error");
            if (cJSON_IsNull(data)) data = NULL;
            if (cJSON_IsNull(error)) error = NULL;
            fn(u, status, data, error);
        }
        return;
    }
}

static int kio_gapi(const char *method, const char *path, const cJSON *body, const V10GapiFn fn, void *user) {
    (void) body;
    if (!kio_readable(method, path)) {
        printf("[gapi] %s %s is not sent for the krunker.io account (read-only)\n", method ? method : "GET", path);
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < KIO_PENDING && slot < 0; i++)
        if (!g_kio[i].id) slot = i;
    if (slot < 0) return 0;
    if (!++g_kio_ids) g_kio_ids = 1;
    g_kio[slot].id = g_kio_ids;
    g_kio[slot].fn = fn;
    g_kio[slot].user = user;
    if (!krunker_account_api("GET", KRUNKER_SVC_API, path, NULL, kio_done, (void *) (uintptr_t) g_kio_ids)) {
        g_kio[slot].id = 0;
        g_kio[slot].fn = NULL;
        return 0;
    }
    return 1;
}

typedef struct {
    BackendHttpFn fn;
    void *user;
} GapiHttp;

static void gapi_http_done(void *user, const int status, const cJSON *data, const cJSON *error) {
    GapiHttp *h = user;
    cJSON *env = cJSON_CreateObject();
    if (data) cJSON_AddItemToObject(env, "data", cJSON_Duplicate(data, 1));
    if (error) cJSON_AddItemToObject(env, "error", cJSON_Duplicate(error, 1));
    char *text = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    h->fn(h->user, status, text ? text : "", text ? strlen(text) : 0);
    free(text);
    free(h);
}

int v10_gapi_http(const char *path, const BackendHttpFn fn, void *user) {
    if (!fn || !path) return 0;
    GapiHttp *h = malloc(sizeof(*h));
    if (!h) return 0;
    h->fn = fn;
    h->user = user;
    if (!v10_gapi("GET", path, NULL, gapi_http_done, h)) {
        free(h);
        return 0;
    }
    return 1;
}

int v10_gapi(const char *method, const char *path, const cJSON *body, const V10GapiFn fn, void *user) {
    if (!fn || !path) return 0;
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) return kio_gapi(method ? method : "GET", path, body, fn, user);
    if (backend_available() != 1) return 0;
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL);
    int slot = -1;
    for (int i = 0; i < GAPI_PENDING && slot < 0; i++)
        if (!g_pending[i].fn) slot = i;
    if (slot < 0) return 0;
    backend_social_open();
    cJSON *f = cJSON_CreateArray();
    const int64_t rid = g_next_rid++;
    cJSON_AddItemToArray(f, cJSON_CreateString("gapi"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber((double) rid));
    cJSON_AddItemToArray(f, cJSON_CreateString(method ? method : "GET"));
    cJSON_AddItemToArray(f, cJSON_CreateString(path));
    cJSON_AddItemToArray(f, body ? cJSON_Duplicate(body, 1) : cJSON_CreateNull());
    const int sent = backend_social_send(f);
    cJSON_Delete(f);
    if (!sent) return 0;
    g_pending[slot].rid = rid;
    g_pending[slot].fn = fn;
    g_pending[slot].user = user;
    return 1;
}

void v10_gapi_cancel(const V10GapiFn fn, void *user) {
    for (int i = 0; i < GAPI_PENDING; i++)
        if (g_pending[i].fn == fn && g_pending[i].user == user) g_pending[i].fn = NULL;
    for (int i = 0; i < KIO_PENDING; i++)
        if (g_kio[i].id && g_kio[i].fn == fn && g_kio[i].user == user) g_kio[i].id = 0, g_kio[i].fn = NULL;
}

// v10 errors.* (ui/i18n/en.json) by code: _34580's table (code -> key, game.deob.js:103796), the
// codes the Social hub, the profile and the clan views can see; shown through i18n.t(key)
static const struct {
    const char *code, *key, *text;
} k_errors[] = {
    {"SOCIAL_RATE_LIMITED", "errors.social.rateLimited", "You're posting too fast. Wait a moment before trying again."},
    {"SOCIAL_INSUFFICIENT_FUNDS", "errors.social.insufficientFunds", "You don't have enough KR to fund this giveaway."},
    {"SOCIAL_GIVEAWAY_TOO_SMALL", "errors.social.giveawayTooSmall", "Giveaways must be at least 10 KR."},
    {"SOCIAL_POST_TEXT_REQUIRED", "errors.social.postTextRequired", "Your post needs some text."},
    {"SOCIAL_POST_NOT_FOUND", "errors.social.postNotFound", "This post no longer exists."},
    {"SOCIAL_CREATE_FAILED", "errors.social.createFailed", "Could not create your post. Please try again."},
    {"SOCIAL_COMMENT_TEXT_REQUIRED", "errors.social.commentTextRequired", "Your comment needs some text."},
    {"SOCIAL_ALREADY_VOTED", "errors.social.alreadyVoted", "You have already voted on this post."},
    {"SOCIAL_LEVEL_TOO_LOW", "errors.social.levelTooLow", "You must be level 10 or higher to post."},
    {"SOCIAL_GIVEAWAY_LEVEL_TOO_LOW", "errors.social.giveawayLevelTooLow", "You must be level 20 or higher to post a giveaway."},
    {"GIFT_INSUFFICIENT_FUNDS", "errors.social.giftInsufficientFunds", "You don't have enough KR to send this gift."},
    {"PLAYERS_NOT_FOUND", "errors.profiles.notFound", "That player could not be found."},
    {"PLAYERS_SEARCH_INVALID", "errors.profiles.searchInvalid", "Enter at least 2 characters to search."},
    {"PLAYERS_SEARCH_LIMIT_INVALID", "errors.profiles.searchLimitInvalid", "Invalid search limit."},
    {"PLAYERS_INVALID_RANGE", "errors.profiles.invalidRange", "Invalid sales history window."},
    {"PLAYERS_PROFILE_FAILED", "errors.profiles.profileFailed", "Failed to load profile."},
    {"PLAYERS_SALES_FAILED", "errors.profiles.salesFailed", "Failed to load sales history."},
    {"PLAYERS_SEARCH_FAILED", "errors.profiles.searchFailed", "Search failed. Please try again."},
    {"PLAYERS_PROXY_FAILED", "errors.profiles.proxyFailed", "Some profile data is temporarily unavailable."},
    {"EB_AUTH_REQUIRED", "errors.eventBus.authRequired", "You must be signed in to use chat."},
    {"EB_AUTH_INVALID", "errors.eventBus.authInvalid", "Your session expired. Please sign in again."},
    {"EB_UNAUTHORIZED", "errors.eventBus.authRequired", "You must be signed in to use chat."},
    {"EB_TARGET_OFFLINE", "errors.eventBus.targetOffline", "That friend is offline right now."},
    {"EB_NOT_FRIEND", "errors.eventBus.notFriend", "You can only message friends."},
    {"EB_RATE_LIMITED", "errors.eventBus.rateLimited", "You're sending messages too fast. Slow down."},
    {"EB_MSG_INVALID", "errors.eventBus.msgInvalid", "The message could not be sent."},
    {"EB_TEXT_TOO_LONG", "errors.eventBus.textTooLong", "That message is too long."},
    {"EB_UNKNOWN_TYPE", "errors.eventBus.unknownType", "Chat client is out of date. Please refresh."},
    {"EB_INTERNAL", "errors.eventBus.internal", "Chat service hit an error. Try again."},
};

const char *v10_gapi_error_text(const cJSON *error, const char *fallback) {
    if (cJSON_IsString(error)) return error->valuestring;
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(error, "code"));
    if (code)
        for (size_t i = 0; i < sizeof(k_errors) / sizeof(k_errors[0]); i++)
            if (!strcmp(code, k_errors[i].code)) return I18N(k_errors[i].key, k_errors[i].text);
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
    if (msg && msg[0]) return msg;
    return fallback ? fallback : I18N("errors.common.unknown", "An unexpected error occurred. Please try again.");
}
