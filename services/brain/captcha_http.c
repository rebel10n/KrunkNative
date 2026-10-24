/*
 * services/brain/captcha_http.c - the captcha module (captcha.h, doc/v10/captcha.md): the captcha.* config and
 * the challenge route.
 *
 *   GET /mm/captcha?for=login|register|hosting|seek
 *       -> 200 {"algorithm":"SHA-256","challenge","maxnumber","salt","signature"} (an ALTCHA challenge: what
 *          v10.0.3's "cpt" packet and bCpt answer carry, deob-10.0.3.js:111425, 133914; what an <altcha-widget
 *          challengeurl> fetches)
 *       -> 400 {"success":false,"error":"BadRequest"} for another purpose, 503 "Unavailable" with no key.
 *
 * v10.0.3 has no such HTTP route: its sign-in captcha comes from Cloudflare (Turnstile's api.js,
 * deob-10.0.3.js:130307) and its ALTCHA challenges over the game socket (cpt, bCpt). The route is Box Office's,
 * served whether or not captcha.enforce is on, so clients solve before any brain enforces (the rollout,
 * doc/backend.md 7.10). Issuing is stateless (signed, nothing stored), so it is not rate-limited. It sits under /mm/
 * (next to /mm/generate-token) so that every proxy in front of the brain passes it to a web page: net-bridge --web
 * (brainProxy.ts PROXIED_PREFIXES) and the deploy's OpenResty (location ^/(auth|mm|api|social|ai)/).
 */
#include <string.h>

#include "brain.h"
#include "brain_internal.h" /* mm_settings()->join_secret */
#include "captcha.h"
#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/log.h"

static void challenge_error(kn_http_req *req, int status, const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 0);
    cJSON_AddStringToObject(o, "error", name);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static void h_challenge(kn_http_req *req, void *ud) {
    (void) ud;
    if (!strcmp(req->method, "OPTIONS")) {
        brain_cors_headers(req);
        kn_http_respond(req, 204, NULL, NULL, 0);
        return;
    }
    char purpose[24] = "";
    kn_http_query_get(req, "for", purpose, sizeof purpose);
    if (!captcha_purpose_public(purpose)) {
        challenge_error(req, 400, "BadRequest");
        return;
    }
    char *json = captcha_issue(purpose, 0);
    if (!json) {
        challenge_error(req, 503, "Unavailable");
        return;
    }
    brain_cors_headers(req);
    kn_http_set_header(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    kn_http_set_header(req, "Pragma", "no-cache");
    kn_http_set_header(req, "Expires", "0");
    kn_http_respond(req, 200, "application/json", json, strlen(json));
    cJSON_free(json);
}

int captcha_init(Brain *b) {
    const MmSettings *mm = mm_settings();
    const CaptchaSettings s = {
        .join_secret = mm ? mm->join_secret : "",
        .max_number = (long) kn_config_int(b->cfg, "captcha.max_number", CAPTCHA_DEFAULT_MAX_NUMBER),
        .ttl_s = (int) kn_config_int(b->cfg, "captcha.ttl_s", CAPTCHA_DEFAULT_TTL_S),
        .enforce = kn_config_bool(b->cfg, "captcha.enforce", 0),
    };
    if (captcha_configure(&s) != 0) {
        KN_WARN("captcha", "no key for captcha challenges (brain.join_secret is blank): none issued%s",
                s.enforce ? ", captcha.enforce has no effect" : "");
    } else if (captcha_enforced()) { /* NOTICE: a startup line, as the brain's own (the enforcement state matters) */
        KN_NOTICE("captcha", "captcha enforced: sign-in, registration and hosting need a solved challenge (max_number %ld, ttl %d s)",
                captcha_max_number(), captcha_ttl());
    } else {
        KN_NOTICE("captcha", "captcha challenges issued, not enforced (captcha.enforce off; max_number %ld, ttl %d s)",
                captcha_max_number(), captcha_ttl());
    }
    kn_http_route(b->http, NULL, "/mm/captcha", h_challenge, b);
    return 0;
}
