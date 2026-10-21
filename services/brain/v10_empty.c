/*
 * services/brain/v10_empty.c - the v10 REST routes KrunkNative has no data for, on the brain root
 * (api.krunker.io and gapi.svc.krunker.io both answer at "/"). Every route answers one of four ways.
 * The v10.js line is the client's call; the consumer it is read by is in parentheses.
 *
 * EMPTY (200 {"data": ..., "error": null}: the shape the consumer reads, nothing invented):
 *
 * REFUSAL (403 {"error": {"code": "NOT_IMPLEMENTED", "message": ...}}; the v10 wrapper throws on 5xx,
 * and the consumers that need a reply catch it):
 *   account reads KrunkNative cannot map to a v10 account: /account/usernames 26347,
 *       /email/preferences 26424 (114573),
 *       /discord/status 26439 (97112 has no catch).
 *   auth, every /auth route 26172-26304: KrunkNative's accounts use /auth/login and friends (auth.c),
 *       a different contract. Not mapped here.
 *   writes: /account/2fa routes 26326-26339, POST /account/usernames/reserve
 *       and /swap 26352-26360, POST /email/preferences 26429, /discord/link|join|unlink 26444-26457,
 *
 * Not registered here, on purpose: the social tunnel's routes (social_gapi.c, clan_v10.c, studios.c,
 * strike_econ.c), /market, /items, /trades, /moderation, /mods, /kpd and the /players routes (v10_market.c),
 * the account, ranked, daily spin, spin, wallet, web push and payment routes (v10_account.c),
 * the maps and titles v10_api.c serves (GET /maps, /maps/:id, /maps/by-name/:name, /titles), and the
 * routes other modules answer (the alias routes, /ranked/me/connection, /streams/top, /guest/profile).
 */
#include "v10_empty.h"

#include "v10_api.h"

#define WHY_PAYMENT "Box Office has no payment backend"
#define WHY_AUTH "Box Office accounts use /auth/login, not the v10 auth flow"
#define WHY_ACCOUNT "Box Office has no v10 account mapping for this read"
#define WHY_WRITE "Box Office does not take this write"

/* ---- EMPTY reads */

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

    /* gapi: account, email, Discord */
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

};

int v10_empty_init(Brain *b) {
    for (size_t i = 0; i < sizeof k_routes / sizeof k_routes[0]; i++) {
        kn_http_route(b->http, k_routes[i].method, k_routes[i].pattern, k_routes[i].handler, k_routes[i].ud);
    }
    return 0;
}
