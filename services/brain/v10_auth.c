/*
 * services/brain/v10_auth.c - the v10 gapi routes of the account: the auth routes at the brain root (the sign-in
 * flow of v10.js 26172-26304 and its consumer, 114100-114500), the account's 2FA (/account/2fa routes, 26326-26339),
 * the usernames (/account/usernames*, 26347-26360), the mail preference (/email/preferences, 26424-26429) and
 * the Discord link (the discord routes, 26439-26457). doc/v10/api-inventory.md has each route's status and source.
 *
 * The answers, mapped onto KrunkNative's real flows:
 *   password login (username or email) -> acct_auth (accounts.c): {type: "login_ok", access_token, refresh_token}
 *       where the token is the session acct_auth issued (KN's refresh keeps it, so refresh_token is the same).
 *   a 2FA account (v7.1.4 TOTP, account_security.c) -> {type: "check_2fa", challenge_id}; the code goes to
 *       POST /auth/2fa/challenge/:id, which runs acct_auth again with it. The challenge holds the password of
 *       the first step in memory for CHALLENGE_TTL_MS (the social socket keeps its own pending password the same
 *       way). A wrong code asks again (the same check_2fa); a refusal (the lock) ends the challenge.
 *   the emailed sign-in code (rows 35-37, accounts.login.email_code, default true): POST /auth/login/email-code
 *       {email} -> {type: "enter_code", challenge_id}, the same answer whether or not an account has the address
 *       (no enumeration); the six-digit code is mailed when one does (account_security.c, the mail of "login").
 *       POST /auth/code-challenge/:id {code}: a wrong code asks again (enter_code); the right one signs in with
 *       the session of a password login (acct_session_issue, accounts.c). A fifth wrong code ends the challenge
 *       (429 "Please Try Later", the login throttle's answer). POST /auth/resend-code-challenge/:id mails a new
 *       code for the same challenge and voids the old one. The challenge store is CODE_MAX entries for
 *       CHALLENGE_TTL_MS, like the 2FA challenges. Box Office assumes v10.0.3 offers the code (the consumer's
 *       enter_code step); a 2FA account gets no code (its sign-in is the password and the authenticator code).
 *   recover-account -> the reset mail of the socket's ["rpr"] (account_security.c): the consumer reads no field.
 *   the account's 2FA setup, verify and disable -> the 2FA procs of db_security.c (P_TWOFA_CONFIRM / _DISABLE).
 *   GET /account/usernames -> the account's own username: one name, active, never expires (accounts.username).
 *       Box Office sells no usernames and keeps no renewals, so reserve is refused and a swap only answers the
 *       active name.
 *   GET /email/preferences -> data null (no preference stored: the consumer shows 'not chosen'); POST is refused.
 *   GET /discord/status -> {linked: false} (no Discord link); link, join and unlink are refused.
 *   the email gate (accounts.login.require_verified, default on): a password login of an account whose email is
 *       not verified -> {type: "ensure_verified", challenge_id}, no token (the session acct_auth issued is revoked).
 *       The v10.0.3 consumer handles that state (its sign-in loop, v10.js 114131: the verify window, then
 *       checkVerification). ASSUMPTION: v10.0.3 emits ensure_verified only for accounts with an unverified email,
 *       which is what the gate sends; an account with no email is never gated.
 *   POST /auth/verification/check/:id -> the login again with the challenge's password: login_ok once the address
 *       is verified, else ensure_verified (a 2FA account: check_2fa) with the same id.
 *   POST /auth/verification/resend/:id -> the verify mail to the address (account_security.c acct_email_request_http).
 *   POST /auth/verification/change-email/:id {newEmail} -> the address, stored unverified, and its verify mail.
 *
 * Every /auth error body is {"data": null, "error": "<text>"} (the consumers throw the text); the refusals
 * are v10_reply_error's {"error": {"code": "NOT_IMPLEMENTED", ...}}. The flows Box Office does not have
 * (sign-up by email code, legacy migration, K-ID consent, and the username and password challenges) answer
 * 404 with the expired-sign-in text the consumer throws: no challenge of those ids exists. The email-code
 * sign-in (rows 35-37) and the verification challenges (rows 40-42) are served above.
 */
#include "v10_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "acct_internal.h"
#include "captcha.h"
#include "db_procs.h"
#include "roles.h"
#include "v10_api.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

#define WHY_SALE "Box Office does not sell usernames"
#define WHY_PREF "Box Office does not store email preferences"
#define WHY_DISCORD "Box Office has no Discord link"
#define WHY_UNOFFERED "This sign-in step is not offered on Box Office"
#define WHY_EMAIL_CODE "Sign in with your username or email and password on Box Office"
/* the consumer's text for a challenge id nobody issued (the sign-in is over): v10.js throws it */
#define MSG_EXPIRED "This sign-in has expired. Sign in again."

#define CHALLENGE_MAX 64
#define CHALLENGE_TTL_MS (5 * 60 * 1000)
#define CODE_MAX 64
#define CODE_FAILS 5                /* wrong codes that end an emailed-code sign-in */

static Brain *g_brain;
static int g_email_code = 1;        /* accounts.login.email_code: the emailed sign-in code is offered */

/* ---------------------------------------------------------------- replies */

/* {"data": data, "error": error} with the status; data is consumed (NULL = null), err NULL = null */
static void auth_reply(kn_http_req *req, int status, cJSON *data, const char *err) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateNull());
    if (err) cJSON_AddStringToObject(o, "error", err);
    else cJSON_AddNullToObject(o, "error");
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static void auth_fail(kn_http_req *req, int status, const char *msg) { auth_reply(req, status, NULL, msg); }

/* the status of a user-facing error text (the accounts' and the 2FA procs' messages) */
static int err_status(const char *err) {
    if (!err) return 400;
    if (!strncmp(err, "Server", 6) || strstr(err, "unavailable")) return 503;
    if (strstr(err, "Too many") || !strcmp(err, "Please Try Later")) return 429;
    if (!strncmp(err, "Banned", 6) || !strcmp(err, "Account banned.")) return 403;
    if (!strcmp(err, "Invalid login.") || !strcmp(err, "Invalid credentials.")) return 401;
    return 400;
}

static void refuse(kn_http_req *req, const char *why) { v10_reply_error(req, 403, "NOT_IMPLEMENTED", why); }

static void not_offered(kn_http_req *req, void *ud) { auth_fail(req, 404, (const char *) ud); }

/* ---------------------------------------------------------------- the 2FA challenges of a password login */

typedef struct Challenge {
    int used;
    char id[40];                /* kn_random_hex: 32 hex digits, a path segment */
    char name[256];             /* the login name as typed (a username or an email) */
    char *pass;                 /* the password of the first step; wiped when the challenge ends */
    int64_t account_id;         /* the account of an ensure_verified challenge (the email gate); 0 before it */
    uint64_t expires_ms;
} Challenge;

static Challenge g_challenges[CHALLENGE_MAX];

static void challenge_drop(Challenge *c) {
    if (!c) return;
    if (c->pass) {
        memset(c->pass, 0, strlen(c->pass));
        free(c->pass);
    }
    memset(c, 0, sizeof *c);
}

static void challenges_purge(uint64_t now) {
    for (size_t i = 0; i < CHALLENGE_MAX; i++) {
        if (g_challenges[i].used && now >= g_challenges[i].expires_ms) challenge_drop(&g_challenges[i]);
    }
}

static Challenge *challenge_find(const char *id) {
    if (!id || !*id) return NULL;
    challenges_purge(kn_now_ms());
    for (size_t i = 0; i < CHALLENGE_MAX; i++) {
        if (g_challenges[i].used && !strcmp(g_challenges[i].id, id)) return &g_challenges[i];
    }
    return NULL;
}

static Challenge *challenge_new(const char *name, const char *pass) {
    const uint64_t now = kn_now_ms();
    challenges_purge(now);
    for (size_t i = 0; i < CHALLENGE_MAX; i++) {
        Challenge *c = &g_challenges[i];
        if (c->used) continue;
        if (kn_random_hex(c->id, sizeof c->id, 16) != 0) return NULL;
        c->pass = strdup(pass);
        if (!c->pass) return NULL;
        snprintf(c->name, sizeof c->name, "%s", name);
        c->account_id = 0;
        c->expires_ms = now + CHALLENGE_TTL_MS;
        c->used = 1;
        return c;
    }
    return NULL;
}

/* each use of a challenge keeps it for CHALLENGE_TTL_MS more */
static void challenge_touch(Challenge *c) { c->expires_ms = kn_now_ms() + CHALLENGE_TTL_MS; }

/* ---------------------------------------------------------------- sign-in */

static void auth_login_ok(kn_http_req *req, const char *token) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "type", "login_ok");
    cJSON_AddStringToObject(d, "access_token", token);
    cJSON_AddStringToObject(d, "refresh_token", token);
    auth_reply(req, 200, d, NULL);
}

static void auth_check_2fa(kn_http_req *req, const char *id) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "type", "check_2fa");
    cJSON_AddStringToObject(d, "challenge_id", id);
    auth_reply(req, 200, d, NULL);
}

static void auth_ensure_verified(kn_http_req *req, const char *id) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "type", "ensure_verified");
    cJSON_AddStringToObject(d, "challenge_id", id);
    auth_reply(req, 200, d, NULL);
}

typedef struct LoginCtx {
    kn_http_req *req;
    char name[256];
    char *pass;                 /* heap; wiped when the step ends */
    char code[16];              /* the second step's 2FA code */
    char chid[40];              /* the second step's challenge ("" for the password step) */
} LoginCtx;

static void login_free(LoginCtx *c) {
    if (c->pass) {
        memset(c->pass, 0, strlen(c->pass));
        free(c->pass);
    }
    memset(c->code, 0, sizeof c->code);
    free(c);
}

/* ---------------------------------------------------------------- the email gate */

/* accounts.login.require_verified (default on): a password login whose account has an email that is not verified
 * answers {type: "ensure_verified", challenge_id} instead of login_ok, and the session the login issued is revoked.
 * The v10.0.3 consumer handles that state (its sign-in loop, v10.js 114131: the verify window, then
 * checkVerification). v10 emits it only for unverified accounts: that is the assumption the gate rests on.
 * 0 = the login stands as before. */
static int g_require_verified = 1;

typedef struct GateCtx {
    LoginCtx *c;                /* owned: freed when the gate answers */
    int64_t id;
    char token[80];             /* the session the login issued */
} GateCtx;

static void gate_done(kn_db_result *res, void *ud) {
    GateCtx *g = ud;
    LoginCtx *c = g->c;
    kn_http_req *req = c->req;
    cJSON *st = kn_db_ok(res) ? acct_db_json(res) : NULL; /* P_EMAIL_STATE: {email, verified, ...} */
    if (!st) {
        acct_revoke_token(g->token);
        auth_fail(req, 503, "Server error.");
    } else {
        const char *addr = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st, "email"));
        const int verified = (int) kn_json_int(st, "verified", 0);
        if (!addr || !*addr || verified) { /* no email, or verified: the login stands */
            challenge_drop(challenge_find(c->chid));
            auth_login_ok(req, g->token);
        } else {
            acct_revoke_token(g->token);
            /* a 2FA step's challenge carries on as the gate's (it holds the password too) */
            Challenge *ch = c->chid[0] ? challenge_find(c->chid) : NULL;
            if (!ch) ch = challenge_new(c->name, c->pass);
            if (!ch) {
                auth_fail(req, 503, "Too many sign-ins are waiting. Please try again.");
            } else {
                ch->account_id = g->id;
                challenge_touch(ch);
                auth_ensure_verified(req, ch->id);
            }
        }
    }
    cJSON_Delete(st);
    login_free(c);
    free(g);
}

/* the account's email state (P_EMAIL_STATE), then gate_done answers. Every path that does not log in revokes token. */
static void gate_check(LoginCtx *c, int64_t id, const char *token) {
    GateCtx *g = calloc(1, sizeof *g);
    if (g) {
        char idbuf[24], nowbuf[24];
        g->c = c;
        g->id = id;
        snprintf(g->token, sizeof g->token, "%s", token);
        /* cooldown 0: only the state is read (now in epoch ms, as mail_log's times are) */
        if (kn_db_call(g_brain->db, &P_EMAIL_STATE,
                       KN_DB_ARGS(acct_itoa(idbuf, id), "0", acct_itoa(nowbuf, (int64_t) time(NULL) * 1000)),
                       gate_done, g) == 0)
            return;
        free(g);
    }
    /* no state to read: refuse the sign-in (a refused submit calls back on no path) */
    acct_revoke_token(token);
    auth_fail(c->req, 503, "Server error.");
    login_free(c);
}

static void on_login(void *ud, const char *err, const cJSON *account) {
    LoginCtx *c = ud;
    kn_http_req *req = c->req;
    const char *token = !err && account ? kn_json_str(account, "token", NULL) : NULL;
    if (token && *token) {
        const int64_t id = kn_json_int(account, "id", 0);
        if (g_require_verified && id > 0) {
            gate_check(c, id, token); /* owns c: login_ok or ensure_verified from gate_done */
            return;
        }
        challenge_drop(challenge_find(c->chid));
        auth_login_ok(req, token);
    } else if (!err && account) {
        auth_fail(req, 503, "Server error.");
    } else if (err && (!strcmp(err, ACCT_ERR_2FA) || !strcmp(err, ACCT_ERR_2FA_BAD))) {
        if (c->chid[0]) {
            auth_check_2fa(req, c->chid);   /* a wrong code: ask again */
        } else {
            Challenge *ch = challenge_new(c->name, c->pass);
            if (ch) auth_check_2fa(req, ch->id);
            else auth_fail(req, 503, "Too many sign-ins are waiting. Please try again.");
        }
    } else {
        challenge_drop(challenge_find(c->chid));
        auth_fail(req, err_status(err), err ? err : "Invalid login.");
    }
    login_free(c);
}

static void start_login(kn_http_req *req, const char *name, const char *pass, const char *chid, const char *code) {
    LoginCtx *c = calloc(1, sizeof *c);
    if (!c) {
        auth_fail(req, 503, "Server error.");
        return;
    }
    c->req = req;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->pass = strdup(pass);
    if (chid) snprintf(c->chid, sizeof c->chid, "%s", chid);
    if (code) snprintf(c->code, sizeof c->code, "%s", code);
    if (!c->pass) {
        login_free(c);
        auth_fail(req, 503, "Server error.");
        return;
    }
    char ip[64];
    snprintf(ip, sizeof ip, "%s", req->remote_addr ? req->remote_addr : "");
    AcctAuthReq rq = {.action = 1, .name = c->name, .pass = c->pass, .ip = ip, .origin = "http",
                      .twofa = code ? c->code : NULL};
    acct_auth(&rq, on_login, c); /* copies what it keeps */
}

/* POST /auth/login/username {username, password, a?, b?} and /auth/login/email {email, password, a?, b?} */
static void login_route(kn_http_req *req, int by_email) {
    if (!auth_login_limit(req)) return;
    cJSON *body = kn_http_body_json(req);
    const char *name = kn_json_str(body, by_email ? "email" : "username", NULL);
    const char *pass = kn_json_str(body, "password", NULL);
    if (!name || !*name || !pass || !*pass) {
        cJSON_Delete(body);
        auth_fail(req, 400, "Invalid credentials.");
        return;
    }
    /* "b": v10.0.3's captcha field (loginUsername's 4th argument, the Turnstile token, deob-10.0.3.js:30111-30122,
     * 130762-130763). Box Office's captcha is an ALTCHA payload (captcha.h): "b" holds it on both routes (v10.0.3's
     * loginEmail sends none, deob-10.0.3.js:130765: one Box Office account answers both, so both are gated).
     * Refused only with captcha.enforce on, before the password is hashed: the error text, as the consumer throws it. */
    CaptchaResult cr;
    if (!captcha_gate(kn_json_str(body, "b", NULL), "login", req->remote_addr, &cr)) {
        cJSON_Delete(body);
        auth_fail(req, 400, captcha_refusal_text(cr));
        return;
    }
    start_login(req, name, pass, NULL, NULL);
    cJSON_Delete(body);
}

static void h_login_username(kn_http_req *req, void *ud) {
    (void) ud;
    login_route(req, 0);
}

static void h_login_email(kn_http_req *req, void *ud) {
    (void) ud;
    login_route(req, 1);
}

/* POST /auth/2fa/challenge/:id {code}: the code of a check_2fa challenge */
static void h_2fa_challenge(kn_http_req *req, void *ud) {
    (void) ud;
    if (!auth_login_limit(req)) return;
    Challenge *ch = challenge_find(kn_http_param(req, "id"));
    cJSON *body = kn_http_body_json(req);
    const char *code = kn_json_str(body, "code", NULL);
    if (!ch) {
        cJSON_Delete(body);
        auth_fail(req, 404, "This sign-in has expired. Sign in again.");
        return;
    }
    if (!code || !*code || strlen(code) >= 16) {
        cJSON_Delete(body);
        auth_fail(req, 400, "Invalid Code");
        return;
    }
    char name[256], chid[40], pass_copy[256] = "";
    snprintf(name, sizeof name, "%s", ch->name);
    snprintf(chid, sizeof chid, "%s", ch->id);
    snprintf(pass_copy, sizeof pass_copy, "%s", ch->pass ? ch->pass : "");
    start_login(req, name, pass_copy, chid, code);
    memset(pass_copy, 0, sizeof pass_copy);
    cJSON_Delete(body);
}

/* ---------------------------------------------------------------- the verification routes (rows 40-42) */

/* the ensure_verified challenge of the URL's id (its account set by the gate); unknown or expired: 404 */
static Challenge *verify_challenge(kn_http_req *req) {
    Challenge *ch = challenge_find(kn_http_param(req, "id"));
    if (!ch || ch->account_id <= 0) {
        auth_fail(req, 404, "This sign-in has expired. Sign in again.");
        return NULL;
    }
    challenge_touch(ch);
    return ch;
}

/* POST /auth/verification/check/:id: the password login again, with the challenge's stored name and password (the
 * gate answers: login_ok once the address is verified, else ensure_verified with the same id; a 2FA account asks its
 * code: check_2fa with the same id). v10.js 26238, 114141. */
static void h_verify_check(kn_http_req *req, void *ud) {
    (void) ud;
    if (!auth_login_limit(req)) return;
    Challenge *ch = verify_challenge(req);
    if (ch) start_login(req, ch->name, ch->pass, ch->id, NULL); /* start_login copies what it keeps */
}

/* the answer of an acct_email_request_http: {message} or the refusal text (the consumer ignores the reply) */
static void verify_mail_done(void *ud, const char *err, const char *msg) {
    kn_http_req *req = ud;
    if (err) {
        auth_fail(req, err_status(err), err);
        return;
    }
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "message", msg ? msg : "");
    auth_reply(req, 200, d, NULL);
}

/* POST /auth/verification/resend/:id: the verify mail again, to the challenge's address (v10.js 26243, 113497) */
static void h_verify_resend(kn_http_req *req, void *ud) {
    (void) ud;
    Challenge *ch = verify_challenge(req);
    if (!ch) return;
    char ip[64];
    snprintf(ip, sizeof ip, "%s", req->remote_addr ? req->remote_addr : "");
    acct_email_request_http(ch->account_id, NULL, ip, verify_mail_done, req);
}

/* POST /auth/verification/change-email/:id {newEmail}: the address, checked as sh_email_set checks it, stored
 * unverified and mailed (v10.js 26248, 113570; the v10.0.3 body is newEmail too). */
static void h_verify_change(kn_http_req *req, void *ud) {
    (void) ud;
    Challenge *ch = verify_challenge(req);
    if (!ch) return;
    cJSON *body = kn_http_body_json(req);
    const char *typed = kn_json_str(body, "newEmail", NULL);
    if (!typed) typed = kn_json_str(body, "email", "");
    char email[256], norm[256], ip[64];
    snprintf(email, sizeof email, "%s", typed ? typed : "");
    cJSON_Delete(body);
    const char *err = acct_validate_email(email, norm, sizeof norm);
    if (err) {
        auth_fail(req, 400, err);
        return;
    }
    snprintf(ip, sizeof ip, "%s", req->remote_addr ? req->remote_addr : "");
    acct_email_request_http(ch->account_id, norm, ip, verify_mail_done, req);
}

static void h_nonce(kn_http_req *req, void *ud) {
    (void) ud;
    auth_reply(req, 200, NULL, NULL); /* no attestation nonce: the consumer signs in without its payload */
}

/* POST /auth/recover-account {email}: the reset mail of the socket's ["rpr"] */
static void recover_done(void *ud, const char *err, const char *msg) {
    kn_http_req *req = ud;
    if (err) {
        auth_fail(req, err_status(err), err);
        return;
    }
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "message", msg ? msg : "");
    auth_reply(req, 200, d, NULL);
}

static void h_recover(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body = kn_http_body_json(req);
    const char *email = kn_json_str(body, "email", NULL);
    if (!email || !*email) {
        cJSON_Delete(body);
        auth_fail(req, 400, "Enter your email.");
        return;
    }
    char who[256], ip[64];
    if (acct_validate_email(email, who, sizeof who)) { /* the field is an email: a username is not one */
        cJSON_Delete(body);
        auth_fail(req, 400, "Invalid Email");
        return;
    }
    snprintf(ip, sizeof ip, "%s", req->remote_addr ? req->remote_addr : "");
    cJSON_Delete(body);
    acct_recover_request(who, ip, recover_done, req);
}

/* ---------------------------------------------------------------- the emailed sign-in code (rows 35-37) */

typedef struct CodeChallenge {
    int used;
    char id[40];                /* kn_random_hex: 32 hex digits, a path segment */
    char email[256];            /* the address typed, normalized: the mail goes there when an account has it */
    int fails;                  /* wrong codes so far (CODE_FAILS ends the sign-in) */
    uint64_t expires_ms;
} CodeChallenge;

static CodeChallenge g_codes[CODE_MAX];

static void code_drop(CodeChallenge *c) {
    if (c) memset(c, 0, sizeof *c);
}

static void codes_purge(uint64_t now) {
    for (size_t i = 0; i < CODE_MAX; i++) {
        if (g_codes[i].used && now >= g_codes[i].expires_ms) code_drop(&g_codes[i]);
    }
}

static CodeChallenge *code_find(const char *id) {
    if (!id || !*id) return NULL;
    codes_purge(kn_now_ms());
    for (size_t i = 0; i < CODE_MAX; i++) {
        if (g_codes[i].used && !strcmp(g_codes[i].id, id)) return &g_codes[i];
    }
    return NULL;
}

static CodeChallenge *code_new(const char *email) {
    const uint64_t now = kn_now_ms();
    codes_purge(now);
    for (size_t i = 0; i < CODE_MAX; i++) {
        CodeChallenge *c = &g_codes[i];
        if (c->used) continue;
        if (kn_random_hex(c->id, sizeof c->id, 16) != 0) return NULL;
        snprintf(c->email, sizeof c->email, "%s", email);
        c->expires_ms = now + CHALLENGE_TTL_MS;
        c->used = 1;
        return c;
    }
    return NULL;
}

/* whether the emailed code is offered: the config, and a mail server to send it through */
static int code_offered(void) { return g_email_code && acct_email_enabled(); }

static void code_state(kn_http_req *req, const char *chid) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "type", "enter_code");
    cJSON_AddStringToObject(d, "challenge_id", chid);
    auth_reply(req, 200, d, NULL);
}

typedef struct CodeSend {
    kn_http_req *req;
    char chid[40];
    int fresh;                  /* a new challenge: dropped when its mail could not be taken */
} CodeSend;

static void code_sent(void *ud, const char *err) {
    CodeSend *s = ud;
    if (err) {
        if (s->fresh) code_drop(code_find(s->chid));
        auth_fail(s->req, 503, "Server error.");
    } else {
        code_state(s->req, s->chid);
    }
    free(s);
}

/* mail the code of chid to email (acct_login_code_send answers the same whether or not an account has it) */
static void code_send(kn_http_req *req, const char *chid, const char *email, int fresh) {
    CodeSend *s = calloc(1, sizeof *s);
    if (!s) {
        if (fresh) code_drop(code_find(chid));
        auth_fail(req, 503, "Server error.");
        return;
    }
    s->req = req;
    s->fresh = fresh;
    snprintf(s->chid, sizeof s->chid, "%s", chid);
    char ip[64];
    snprintf(ip, sizeof ip, "%s", req->remote_addr ? req->remote_addr : "");
    acct_login_code_send(email, chid, ip, code_sent, s);
}

/* POST /auth/login/email-code {email}: a challenge and, for an account with the address, its code by mail */
static void h_email_code(kn_http_req *req, void *ud) {
    (void) ud;
    if (!code_offered()) {
        auth_fail(req, 404, WHY_EMAIL_CODE);
        return;
    }
    if (!auth_login_limit(req)) return;
    cJSON *body = kn_http_body_json(req);
    const char *email = kn_json_str(body, "email", NULL);
    char norm[256] = "";
    const char *err = !email || !*email ? "Enter your email." : acct_validate_email(email, norm, sizeof norm);
    if (err) {
        cJSON_Delete(body);
        auth_fail(req, 400, err);
        return;
    }
    cJSON_Delete(body);
    CodeChallenge *c = code_new(norm);
    if (!c) {
        auth_fail(req, 503, "Too many sign-ins are waiting. Please try again.");
        return;
    }
    code_send(req, c->id, norm, 1);
}

/* a wrong code: the challenge asks again, and the CODE_FAILS-th ends it */
static void code_wrong(kn_http_req *req, const char *chid) {
    CodeChallenge *c = code_find(chid);
    if (c && ++c->fails >= CODE_FAILS) {
        code_drop(c);
        auth_fail(req, 429, "Please Try Later");
        return;
    }
    code_state(req, chid);
}

static void code_session(void *ud, const char *err, const cJSON *account) {
    kn_http_req *req = ud;
    const char *token = !err && account ? kn_json_str(account, "token", NULL) : NULL;
    if (token && *token) auth_login_ok(req, token);
    else if (err) auth_fail(req, err_status(err), err);
    else auth_fail(req, 503, "Server error.");
}

typedef struct CodeCheck {
    kn_http_req *req;
    char chid[40];
} CodeCheck;

static void code_checked(void *ud, const char *err, int64_t account_id) {
    CodeCheck *k = ud;
    kn_http_req *req = k->req;
    if (!err && account_id > 0) {
        code_drop(code_find(k->chid)); /* the challenge is used up: the right code signs in once */
        acct_session_issue(account_id, req->remote_addr, code_session, req);
    } else if (err && !strcmp(err, ACCT_ERR_LOGIN_CODE_BAD)) {
        code_wrong(req, k->chid);
    } else {
        auth_fail(req, 503, "Server error.");
    }
    free(k);
}

/* POST /auth/code-challenge/:id {code} (row 36) */
static void h_code_challenge(kn_http_req *req, void *ud) {
    (void) ud;
    if (!code_offered()) {
        auth_fail(req, 404, WHY_UNOFFERED);
        return;
    }
    if (!auth_login_limit(req)) return;
    CodeChallenge *c = code_find(kn_http_param(req, "id"));
    cJSON *body = kn_http_body_json(req);
    const char *code = kn_json_str(body, "code", NULL);
    if (!c) {
        cJSON_Delete(body);
        auth_fail(req, 404, MSG_EXPIRED);
        return;
    }
    if (!code || !*code) {
        cJSON_Delete(body);
        auth_fail(req, 400, "Invalid Code");
        return;
    }
    char chid[40];
    snprintf(chid, sizeof chid, "%s", c->id);
    if (strlen(code) != 6 || strspn(code, "0123456789") != 6) { /* no code has that shape: a wrong one */
        cJSON_Delete(body);
        code_wrong(req, chid);
        return;
    }
    CodeCheck *k = calloc(1, sizeof *k);
    if (!k) {
        cJSON_Delete(body);
        auth_fail(req, 503, "Server error.");
        return;
    }
    k->req = req;
    snprintf(k->chid, sizeof k->chid, "%s", chid);
    acct_login_code_check(chid, code, code_checked, k);
    cJSON_Delete(body);
}

/* POST /auth/resend-code-challenge/:id (row 37): a new code for the same challenge */
static void h_resend_code(kn_http_req *req, void *ud) {
    (void) ud;
    if (!code_offered()) {
        auth_fail(req, 404, WHY_UNOFFERED);
        return;
    }
    if (!auth_login_limit(req)) return;
    CodeChallenge *c = code_find(kn_http_param(req, "id"));
    if (!c) {
        auth_fail(req, 404, MSG_EXPIRED);
        return;
    }
    char chid[40], email[256];
    snprintf(chid, sizeof chid, "%s", c->id);
    snprintf(email, sizeof email, "%s", c->email);
    code_send(req, chid, email, 0);
}

/* ---------------------------------------------------------------- the account of the Bearer token */

typedef void (*WhoFn)(kn_http_req *req, int64_t id, const char *username, void *ud); /* id 0: not signed in */

typedef struct WhoCtx {
    kn_http_req *req;
    WhoFn fn;
    void *ud;
} WhoCtx;

static void who_done(void *ud, const char *err, const cJSON *acc) {
    WhoCtx *w = ud;
    const int64_t id = !err && acc ? kn_json_int(acc, "id", 0) : 0;
    const char *name = id > 0 ? kn_json_str(acc, "username", "") : "";
    char username[64];
    snprintf(username, sizeof username, "%s", name ? name : "");
    kn_http_req *req = w->req;
    WhoFn fn = w->fn;
    void *fud = w->ud;
    free(w);
    fn(req, id, username, fud);
}

/* the account of ?token= or the Bearer token, through the accounts bridge (as ranked.c's with_account) */
static void with_who(kn_http_req *req, WhoFn fn, void *ud) {
    char token[512] = "";
    if (kn_http_query_get(req, "token", token, sizeof token) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(token, sizeof token, "%s", bearer ? bearer : "");
    }
    WhoCtx *w = calloc(1, sizeof *w);
    if (!token[0] || !w) {
        free(w);
        fn(req, 0, "", ud);
        return;
    }
    w->req = req;
    w->fn = fn;
    w->ud = ud;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(g_brain, "token", args, who_done, w);
    cJSON_Delete(args);
}

static void need_sign_in(kn_http_req *req) { auth_fail(req, 401, "Sign in first."); }

/* ---------------------------------------------------------------- the account's 2FA (account_security.c) */

typedef struct EnableCtx {
    kn_http_req *req;
    char uri[256];
} EnableCtx;

static void enable_done(kn_db_result *res, void *ud) {
    EnableCtx *e = ud;
    if (!kn_db_ok(res)) {
        const char *msg = acct_db_error(res, "a2FA");
        auth_fail(e->req, err_status(msg), msg);
    } else if (kn_db_affected(res) < 1) {
        auth_fail(e->req, 400, "2FA is already enabled.");
    } else {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "qr_code_url", e->uri);
        auth_reply(e->req, 200, d, NULL);
    }
    free(e);
}

/* POST /account/2fa/enable/start: a new secret waits for its first code (the socket's ["a2FA"]) */
static void enable_who(kn_http_req *req, int64_t id, const char *username, void *ud) {
    (void) ud;
    if (id <= 0) {
        need_sign_in(req);
        return;
    }
    uint8_t key[20];
    char secret[40], idbuf[24];
    EnableCtx *e = calloc(1, sizeof *e);
    if (!e || kn_random_bytes(key, sizeof key) != 0 || kn_base32_encode(key, sizeof key, secret, sizeof secret) < 0) {
        free(e);
        memset(key, 0, sizeof key);
        auth_fail(req, 503, "Server error.");
        return;
    }
    memset(key, 0, sizeof key);
    e->req = req;
    acct_twofa_otpauth(username, secret, e->uri, sizeof e->uri);
    const int rc = kn_db_query_sql(g_brain->db,
                                   SQL(.any = "UPDATE accounts SET twofa_pending = $2 WHERE id = {int:$1} AND twofa_secret IS NULL"),
                                   KN_DB_ARGS(acct_itoa(idbuf, id), secret), enable_done, e);
    memset(secret, 0, sizeof secret);
    if (rc != 0) { /* a refused submit calls back on no path */
        free(e);
        auth_fail(req, 503, "Server error.");
    }
}

typedef struct CodeCtx {
    kn_http_req *req;
    char code[16];
    int enable;                 /* 1 confirm the pending secret, 0 disable */
} CodeCtx;

typedef struct TwofaCtx {
    kn_http_req *req;
    int enable;
} TwofaCtx;

static void twofa_done(kn_db_result *res, void *ud) {
    TwofaCtx *t = ud;
    if (!kn_db_ok(res)) {
        const char *msg = acct_db_error(res, t->enable ? "a2FAc" : "d2FA");
        auth_fail(t->req, err_status(msg), msg);
    } else {
        cJSON *o = acct_db_json(res);
        const int ok = (int) kn_json_int(o, "ok", 0);
        cJSON_Delete(o);
        if (!ok) {
            auth_fail(t->req, 400, "Invalid Code");
        } else {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddBoolToObject(d, "enabled", t->enable);
            auth_reply(t->req, 200, d, NULL);
        }
    }
    free(t);
}

static void code_who(kn_http_req *req, int64_t id, const char *username, void *ud) {
    (void) username;
    CodeCtx *k = ud;
    if (id <= 0) {
        need_sign_in(req);
        free(k);
        return;
    }
    TwofaCtx *t = calloc(1, sizeof *t);
    if (!t) {
        free(k);
        auth_fail(req, 503, "Server error.");
        return;
    }
    t->req = req;
    t->enable = k->enable;
    char idbuf[24], nowbuf[24];
    const char *now = acct_itoa(nowbuf, (int64_t) time(NULL));
    const kn_proc *proc = k->enable ? &P_TWOFA_CONFIRM : &P_TWOFA_DISABLE;
    const int rc = kn_db_call(g_brain->db, proc, KN_DB_ARGS(acct_itoa(idbuf, id), k->code, now), twofa_done, t);
    memset(k->code, 0, sizeof k->code);
    free(k);
    if (rc != 0) { /* a refused submit calls back on no path */
        free(t);
        auth_fail(req, 503, "Server error.");
    }
}

/* POST /account/2fa/enable/verify {code} (enable = 1) and POST /account/2fa/disable {code} (enable = 0) */
static void code_route(kn_http_req *req, int enable) {
    cJSON *body = kn_http_body_json(req);
    const char *code = kn_json_str(body, "code", NULL);
    CodeCtx *k = calloc(1, sizeof *k);
    if (!k || !code || !*code || strlen(code) >= sizeof k->code) {
        cJSON_Delete(body);
        free(k);
        auth_fail(req, 400, "Invalid Code");
        return;
    }
    k->req = req;
    k->enable = enable;
    snprintf(k->code, sizeof k->code, "%s", code);
    cJSON_Delete(body);
    with_who(req, code_who, k);
}

static void h_enable_start(kn_http_req *req, void *ud) {
    (void) ud;
    with_who(req, enable_who, NULL);
}

static void h_enable_verify(kn_http_req *req, void *ud) {
    (void) ud;
    code_route(req, 1);
}

static void h_disable(kn_http_req *req, void *ud) {
    (void) ud;
    code_route(req, 0);
}

/* ---------------------------------------------------------------- usernames (the account's one name) */

/* {names: [{name, state: "active"}], cooldown_secs, held, hold_secs} (+ active for a swap) */
static cJSON *names_data(const char *username) {
    cJSON *d = cJSON_CreateObject();
    cJSON *names = cJSON_AddArrayToObject(d, "names");
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "name", username);
    cJSON_AddStringToObject(n, "state", "active");
    cJSON_AddItemToArray(names, n);
    cJSON_AddNumberToObject(d, "cooldown_secs", 0);
    cJSON_AddNullToObject(d, "held");
    cJSON_AddNumberToObject(d, "hold_secs", 0);
    return d;
}

static void names_who(kn_http_req *req, int64_t id, const char *username, void *ud) {
    (void) ud;
    if (id <= 0) {
        need_sign_in(req);
        return;
    }
    v10_reply_data(req, 200, names_data(username));
}

static void h_usernames(kn_http_req *req, void *ud) {
    (void) ud;
    with_who(req, names_who, NULL);
}

typedef struct SwapCtx {
    kn_http_req *req;
    char wanted[64];
} SwapCtx;

static void swap_who(kn_http_req *req, int64_t id, const char *username, void *ud) {
    SwapCtx *s = ud;
    if (id <= 0) {
        need_sign_in(req);
    } else if (!s->wanted[0] || strcasecmp(s->wanted, username)) {
        refuse(req, WHY_SALE);
    } else {
        cJSON *d = names_data(username);
        cJSON_AddStringToObject(d, "active", username);
        v10_reply_data(req, 200, d);
    }
    free(s);
}

static void h_swap(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body = kn_http_body_json(req);
    SwapCtx *s = calloc(1, sizeof *s);
    if (!s) {
        cJSON_Delete(body);
        auth_fail(req, 503, "Server error.");
        return;
    }
    s->req = req;
    snprintf(s->wanted, sizeof s->wanted, "%s", kn_json_str(body, "username", ""));
    cJSON_Delete(body);
    with_who(req, swap_who, s);
}

static void h_reserve(kn_http_req *req, void *ud) {
    (void) ud;
    refuse(req, WHY_SALE);
}

/* ---------------------------------------------------------------- mail preference and Discord */

static void h_email_prefs_get(kn_http_req *req, void *ud) {
    (void) ud;
    auth_reply(req, 200, NULL, NULL); /* no preference stored: the consumer reads null as not chosen */
}

static void h_email_prefs_set(kn_http_req *req, void *ud) {
    (void) ud;
    refuse(req, WHY_PREF);
}

static void h_discord_status(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "linked", 0);
    v10_reply_data(req, 200, d);
}

static void h_discord_refuse(kn_http_req *req, void *ud) {
    (void) ud;
    refuse(req, WHY_DISCORD);
}

/* ---------------------------------------------------------------- routes */

/* the flows Box Office does not have: no challenge of these ids exists, the answer is 404 with no data and the
 * text the consumer throws (MSG_EXPIRED for a challenge id, the sign-up text for the sign-up route) */
static const struct {
    const char *method, *pattern, *why;
} k_not_offered[] = {
    {"POST", "/auth/migrate/:id", MSG_EXPIRED},
    {"POST", "/auth/register/email-code", "Sign up with a username and password on Box Office"},
    {"POST", "/auth/username-challenge/:id", MSG_EXPIRED},
    {"POST", "/auth/password-challenge/:id", MSG_EXPIRED},
    {"POST", "/auth/kid/age-challenge/:id", MSG_EXPIRED},
    {"POST", "/auth/kid/parental-email/:id", MSG_EXPIRED},
    {"GET", "/auth/kid/consent-status/:id", MSG_EXPIRED},
};

int v10_auth_init(Brain *b) {
    g_brain = b;
    g_require_verified = kn_config_bool(b->cfg, "accounts.login.require_verified", 1);
    g_email_code = kn_config_bool(b->cfg, "accounts.login.email_code", 1);
    kn_http_route(b->http, "POST", "/auth/nonce", h_nonce, NULL);
    kn_http_route(b->http, "POST", "/auth/login/username", h_login_username, NULL);
    kn_http_route(b->http, "POST", "/auth/login/email", h_login_email, NULL);
    kn_http_route(b->http, "POST", "/auth/login/email-code", h_email_code, NULL);
    kn_http_route(b->http, "POST", "/auth/code-challenge/:id", h_code_challenge, NULL);
    kn_http_route(b->http, "POST", "/auth/resend-code-challenge/:id", h_resend_code, NULL);
    kn_http_route(b->http, "POST", "/auth/2fa/challenge/:id", h_2fa_challenge, NULL);
    kn_http_route(b->http, "POST", "/auth/verification/check/:id", h_verify_check, NULL);
    kn_http_route(b->http, "POST", "/auth/verification/resend/:id", h_verify_resend, NULL);
    kn_http_route(b->http, "POST", "/auth/verification/change-email/:id", h_verify_change, NULL);
    kn_http_route(b->http, "POST", "/auth/recover-account", h_recover, NULL);
    for (size_t i = 0; i < sizeof k_not_offered / sizeof k_not_offered[0]; i++) {
        kn_http_route(b->http, k_not_offered[i].method, k_not_offered[i].pattern, not_offered,
                      (void *) k_not_offered[i].why);
    }
    kn_http_route(b->http, "POST", "/account/2fa/enable/start", h_enable_start, NULL);
    kn_http_route(b->http, "POST", "/account/2fa/enable/verify", h_enable_verify, NULL);
    kn_http_route(b->http, "POST", "/account/2fa/disable", h_disable, NULL);
    kn_http_route(b->http, "GET", "/account/usernames", h_usernames, NULL);
    kn_http_route(b->http, "POST", "/account/usernames/reserve", h_reserve, NULL);
    kn_http_route(b->http, "POST", "/account/usernames/swap", h_swap, NULL);
    kn_http_route(b->http, "GET", "/email/preferences", h_email_prefs_get, NULL);
    kn_http_route(b->http, "POST", "/email/preferences", h_email_prefs_set, NULL);
    kn_http_route(b->http, "GET", "/discord/status", h_discord_status, NULL);
    kn_http_route(b->http, "POST", "/discord/link", h_discord_refuse, NULL);
    kn_http_route(b->http, "POST", "/discord/join", h_discord_refuse, NULL);
    kn_http_route(b->http, "POST", "/discord/unlink", h_discord_refuse, NULL);
    return 0;
}
