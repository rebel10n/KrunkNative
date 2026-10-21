/*
 * services/brain/v10_empty.c - the v10 REST routes KrunkNative has no data for, on the brain root
 * (api.krunker.io and gapi.svc.krunker.io both answer at "/"). Every route answers one of four ways.
 * The v10.js line is the client's call; the consumer it is read by is in parentheses.
 *
 * EMPTY (200 {"data": ..., "error": null}: the shape the consumer reads, nothing invented):
 *   GET /match-history/me/season/:s/gameMode/:m/isRanked/:x[/region/:r]  25940 (98633): data.matchHistory
 *       {matches: [], totalPages: 0}. The consumer reads the object under data.matchHistory, not data.
 *   GET /leaderboards/rankings/season/:s[/region/:r]   25972 (98600): data {leaderboard: [], totalPages: 0}
 *   GET /skins                                         26474 (137427): data {skins: []}
 *   GET /shop/wild-offer                               26539 (126666): data {offer: null} (no offer)
 *   GET /maps/:id/versions                             26626 (107087): data []
 *   GET /maps/:id/data?player=                         26655 (106987): data []
 *   GET /maps/by-name/:name/leaderboard                26602 (106970): data {entries: [], total: 0}
 *   GET /webhooks/general/maps/config?mn=:name         131917 (getMapPreset, read by presetLoaded 212624):
 *       the legacy body has no data envelope; {} carries no preset (da, fu), which the menu shows as DEFAULT.
 *
 * NOT_FOUND (404 {"error": {"code": "NOT_FOUND"}}):
 *   GET /skins/:id                                     26479: no KrunkNative skin record has a v10 skin id.
 *
 * REFUSAL (403 {"error": {"code": "NOT_IMPLEMENTED", "message": ...}}; the v10 wrapper throws on 5xx,
 * and the consumers that need a reply catch it):
 *   payments, no payment backend: POST /shop/cancel-subscription 26006, /iap/checkout 26018,
 *       /iap/claim 26024, /my-api/create-payment/:id 209123, /my-api/execute-payment/:id 209131.
 *   account reads KrunkNative cannot map to a v10 account: GET /player-profile/me/season/:s[/region/:r]
 *       25959 (98579), /daily-spin/progress 25999 (a zero state would read as a claimable spin), /player/me
 *       26312, /player/wallet 26317, /account/usernames 26347, /email/preferences 26424 (114573),
 *       /discord/status 26439 (97112 has no catch), /krunkcup/wristband 27309 (no call site),
 *       /webpush/status 26408 (is_subscribed would drop the local subscription).
 *   /ipinfo 16440: the IP checker URL (getter 19733). No v10.js call reads its body, so there is no
 *       shape to match, and an empty object would read as a lookup that found nothing.
 *   auth, every /auth route 26172-26304: KrunkNative's accounts use /auth/login and friends (auth.c),
 *       a different contract. Not mapped here.
 *   writes: POST /player-profile/me/claim-milestone/region/:r 25976, POST /spins/execute 27299 (the
 *       spin is the social socket's op), /account/2fa routes 26326-26339, POST /account/usernames/reserve
 *       and /swap 26352-26360, POST /email/preferences 26429, /discord/link|join|unlink 26444-26457,
 *       POST /webpush/subscription-change 26414, skin writes 26484-26511, POST /shop/wild-offer/purchase
 *       and /refresh 26544-26553, POST /krunkcup/wristband/claim 27314, POST /maps/:id/vote 26605,
 *       /feature 26613, DELETE /maps/:id 26621, POST /maps/:id/funds 26637, POST /maps/:id/leaderboard
 *       26645, POST /maps/:id/versions/:v/revert 26632, DELETE /maps/:id/data 26664.
 *   reads of an unmapped account: GET /player/me and /player/wallet (signed-out or no mapping).
 *
 * Not registered here, on purpose: the social tunnel's routes (social_gapi.c, clan_v10.c, studios.c,
 * strike_econ.c), /market, /items, /trades, /moderation, /mods, /kpd and the /players routes (v10_market.c),
 * the maps and titles v10_api.c serves (GET /maps, /maps/:id, /maps/by-name/:name, /titles), and the
 * routes other modules answer (the alias routes, /ranked/me/connection, /streams/top, /guest/profile).
 */
#include "v10_empty.h"

#include "v10_api.h"

#define WHY_PAYMENT "Box Office has no payment backend"
#define WHY_AUTH "Box Office accounts use /auth/login, not the v10 auth flow"
#define WHY_ACCOUNT "Box Office has no v10 account mapping for this read"
#define WHY_WRITE "Box Office does not take this write"
#define WHY_IPINFO "Box Office has no IP lookup in the v10 shape"

/* ---- EMPTY reads */

static void h_match_history(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *data = cJSON_CreateObject();
    cJSON *history = cJSON_AddObjectToObject(data, "matchHistory");
    cJSON_AddItemToObject(history, "matches", cJSON_CreateArray());
    cJSON_AddNumberToObject(history, "totalPages", 0);
    v10_reply_data(req, 200, data);
}

static void h_rankings(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "leaderboard", cJSON_CreateArray());
    cJSON_AddNumberToObject(data, "totalPages", 0);
    v10_reply_data(req, 200, data);
}

static void h_skins_list(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "skins", cJSON_CreateArray());
    v10_reply_data(req, 200, data);
}

static void h_wild_offer(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNullToObject(data, "offer");
    v10_reply_data(req, 200, data);
}

static void h_empty_list(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_data(req, 200, cJSON_CreateArray());
}

static void h_map_leaderboard(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "entries", cJSON_CreateArray());
    cJSON_AddNumberToObject(data, "total", 0);
    v10_reply_data(req, 200, data);
}

/* the legacy webhook answers its body as {da, fu} at the top level (v10.js 131917): no preset is {} */
static void h_map_preset(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *none = cJSON_CreateObject();
    brain_reply_json(req, 200, none);
    cJSON_Delete(none);
}

/* ---- NOT_FOUND */

static void h_skin_not_found(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_error(req, 404, "NOT_FOUND", "Skin not found");
}

/* ---- REFUSAL: ud is the reason (a string literal) */

static void h_refuse(kn_http_req *req, void *ud) {
    const char *why = ud;
    v10_reply_error(req, 403, "NOT_IMPLEMENTED", why);
}

/* ---- the route table: method, pattern (kn_http_route), handler, ud */

typedef struct {
    const char *method;
    const char *pattern;
    kn_http_handler handler;
    void *ud;
} EmptyRoute;

static const EmptyRoute k_routes[] = {
    /* api.krunker.io */
    {"GET", "/webhooks/general/maps/config", h_map_preset, NULL},
    {"GET", "/match-history/me/season/:s/gameMode/:m/isRanked/:x", h_match_history, NULL},
    {"GET", "/match-history/me/season/:s/gameMode/:m/isRanked/:x/region/:r", h_match_history, NULL},
    {"GET", "/player-profile/me/season/:s", h_refuse, (void *) WHY_ACCOUNT},
    {"GET", "/player-profile/me/season/:s/region/:r", h_refuse, (void *) WHY_ACCOUNT},
    {"GET", "/leaderboards/rankings/season/:s", h_rankings, NULL},
    {"GET", "/leaderboards/rankings/season/:s/region/:r", h_rankings, NULL},
    {"POST", "/player-profile/me/claim-milestone/region/:r", h_refuse, (void *) WHY_WRITE},
    {"GET", "/daily-spin/progress", h_refuse, (void *) WHY_ACCOUNT},
    {"POST", "/shop/cancel-subscription", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/iap/checkout", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/iap/claim", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/my-api/create-payment/:id", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/my-api/execute-payment/:id", h_refuse, (void *) WHY_PAYMENT},
    {"GET", "/ipinfo", h_refuse, (void *) WHY_IPINFO},

    /* gapi.svc.krunker.io: auth (every route, 26172-26304) */
    {"POST", "/auth/nonce", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/login/username", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/login/email", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/login/email-code", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/code-challenge/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/resend-code-challenge/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/migrate/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/2fa/challenge/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/verification/check/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/verification/resend/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/verification/change-email/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/register/email-code", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/username-challenge/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/password-challenge/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/recover-account", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/kid/age-challenge/:id", h_refuse, (void *) WHY_AUTH},
    {"POST", "/auth/kid/parental-email/:id", h_refuse, (void *) WHY_AUTH},
    {"GET", "/auth/kid/consent-status/:id", h_refuse, (void *) WHY_AUTH},

    /* gapi: player, account, email, Discord, web push */
    {"GET", "/player/me", h_refuse, (void *) WHY_ACCOUNT},
    {"GET", "/player/wallet", h_refuse, (void *) WHY_ACCOUNT},
    {"POST", "/account/2fa/enable/start", h_refuse, (void *) WHY_WRITE},
    {"POST", "/account/2fa/enable/verify", h_refuse, (void *) WHY_WRITE},
    {"POST", "/account/2fa/disable", h_refuse, (void *) WHY_WRITE},
    {"GET", "/account/usernames", h_refuse, (void *) WHY_ACCOUNT},
    {"POST", "/account/usernames/reserve", h_refuse, (void *) WHY_WRITE},
    {"POST", "/account/usernames/swap", h_refuse, (void *) WHY_WRITE},
    {"GET", "/email/preferences", h_refuse, (void *) WHY_ACCOUNT},
    {"POST", "/email/preferences", h_refuse, (void *) WHY_WRITE},
    {"GET", "/discord/status", h_refuse, (void *) WHY_ACCOUNT},
    {"POST", "/discord/link", h_refuse, (void *) WHY_WRITE},
    {"POST", "/discord/join", h_refuse, (void *) WHY_WRITE},
    {"POST", "/discord/unlink", h_refuse, (void *) WHY_WRITE},
    {"POST", "/webpush/status", h_refuse, (void *) WHY_ACCOUNT},
    {"POST", "/webpush/subscription-change", h_refuse, (void *) WHY_WRITE},

    /* gapi: skins (the list is EMPTY; the one-skin and write routes are NOT_FOUND or REFUSAL) */
    {"GET", "/skins", h_skins_list, NULL},
    {"GET", "/skins/:id", h_skin_not_found, NULL},
    {"POST", "/skins", h_refuse, (void *) WHY_WRITE},
    {"PUT", "/skins/:id", h_refuse, (void *) WHY_WRITE},
    {"DELETE", "/skins/:id", h_refuse, (void *) WHY_WRITE},
    {"POST", "/skins/:id/upload", h_refuse, (void *) WHY_WRITE},
    {"POST", "/skins/:id/duplicate", h_refuse, (void *) WHY_WRITE},

    /* gapi: shop wild offer */
    {"GET", "/shop/wild-offer", h_wild_offer, NULL},
    {"POST", "/shop/wild-offer/purchase", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/shop/wild-offer/refresh", h_refuse, (void *) WHY_WRITE},

    /* gapi: KrunkCup wristband, spins */
    {"GET", "/krunkcup/wristband", h_refuse, (void *) WHY_ACCOUNT},
    {"POST", "/krunkcup/wristband/claim", h_refuse, (void *) WHY_WRITE},
    {"POST", "/spins/execute", h_refuse, (void *) WHY_WRITE},

    /* gapi: maps (GET /maps, /maps/:id and /maps/by-name/:name are v10_api.c's) */
    {"GET", "/maps/:id/versions", h_empty_list, NULL},
    {"POST", "/maps/:id/versions/:v/revert", h_refuse, (void *) WHY_WRITE},
    {"POST", "/maps/:id/funds", h_refuse, (void *) WHY_WRITE},
    {"POST", "/maps/:id/leaderboard", h_refuse, (void *) WHY_WRITE},
    {"GET", "/maps/:id/data", h_empty_list, NULL},
    {"DELETE", "/maps/:id/data", h_refuse, (void *) WHY_WRITE},
    {"GET", "/maps/by-name/:name/leaderboard", h_map_leaderboard, NULL},
    {"POST", "/maps/:id/vote", h_refuse, (void *) WHY_WRITE},
    {"POST", "/maps/:id/feature", h_refuse, (void *) WHY_WRITE},
    {"DELETE", "/maps/:id", h_refuse, (void *) WHY_WRITE},
};

int v10_empty_init(Brain *b) {
    for (size_t i = 0; i < sizeof k_routes / sizeof k_routes[0]; i++) {
        kn_http_route(b->http, k_routes[i].method, k_routes[i].pattern, k_routes[i].handler, k_routes[i].ud);
    }
    return 0;
}
