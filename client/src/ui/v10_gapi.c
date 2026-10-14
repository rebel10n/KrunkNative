// Krunker v10.0.2's gapi client (_10449: social / players / clans calls) over the brain's social
// socket: ["gapi", rid, method, path, body] -> ["gapi", rid, status, {data} | {error}]
// (services/brain/social_gapi.c). v10 sends HTTPS requests to gapi.svc.krunker.io with a bearer
// token; KrunkNative's client already holds the authenticated social socket, so the requests ride
// on it (doc/v10/social.md). Callbacks run on the main thread from backend_poll().
#include <backend.h>
#include <v10_social.h>
#include <i18n.h>
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

int v10_gapi(const char *method, const char *path, const cJSON *body, const V10GapiFn fn, void *user) {
    if (!fn || !path || backend_available() != 1) return 0;
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
