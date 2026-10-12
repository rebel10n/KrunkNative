/*
 * services/brain/social_ws.c - the social WebSocket (Krunker v3.1.1
 * matchmaker/socialWs.ts): GET /api/ws and /social/ws on the brain's public
 * listener.  The v3.1.1 social page connects to config.socialURL + "/ws".
 *
 * Frames: msgpack(["label", ...args]) followed by two bytes (the client's
 * anti-hack nibbles; ignored here, 0,0 when sending) exactly like
 * server/io.ts, or JSON text ["label", ...] (KrunkNative clients; replies
 * follow the encoding of the client's latest frame, or ?enc=json).
 * On open: ["pi", null] (client answers "po") and ["io-init", socketId].
 * Labels and shapes: doc/backend.md "Accounts & social socket".
 */
#include "acct_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/buf.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knsvc/msgpack.h"
#include "knsvc/ws.h"

#define MOD "social"

#define RATE_PER_S 20.0
#define RATE_BURST 60.0

static SocialSession *s_online;   /* authenticated sessions */
static int s_open_count;

/* ------------------------------------------------------------------ session lifetime */

SocialSession *social_retain(SocialSession *s) {
    if (s) s->refs++;
    return s;
}

typedef struct SocialFrameQ {
    cJSON *frame;
    struct SocialFrameQ *next;
} SocialFrameQ;

#define QUEUE_MAX 128

static void dispatch(SocialSession *s, const cJSON *frame);

static void queue_clear(SocialSession *s) {
    while (s->q_head) {
        SocialFrameQ *n = s->q_head;
        s->q_head = n->next;
        cJSON_Delete(n->frame);
        free(n);
    }
    s->q_tail = NULL;
    s->q_len = 0;
}

static void drain(SocialSession *s) {
    if (s->draining) return;
    s->draining = 1;
    while (s->busy == 0 && s->q_head && social_open(s)) {
        SocialFrameQ *n = s->q_head;
        s->q_head = n->next;
        if (!s->q_head) s->q_tail = NULL;
        s->q_len--;
        dispatch(s, n->frame);
        cJSON_Delete(n->frame);
        free(n);
    }
    s->draining = 0;
}

static void clear_pending(SocialSession *s) {
    if (s->pending_pass) memset(s->pending_pass, 0, strlen(s->pending_pass));
    free(s->pending_pass);
    free(s->pending_name);
    s->pending_pass = s->pending_name = NULL;
}

void social_release(SocialSession *s) {
    if (!s || --s->refs > 0) return;
    memset(s->token, 0, sizeof s->token);
    clear_pending(s);
    queue_clear(s);
    free(s);
}

SocialSession *social_hold(SocialSession *s) {
    s->busy++;
    return social_retain(s);
}

void social_unhold(SocialSession *s) {
    s->busy--;
    social_retain(s);
    if (s->busy == 0 && social_open(s)) drain(s);
    social_release(s); /* the drain guard */
    social_release(s); /* the hold */
}

int social_open(const SocialSession *s) { return s && s->ws != NULL; }

static void presence_remove(SocialSession *s) {
    if (!s->prev && !s->next && s_online != s) return;
    if (s->prev) s->prev->next = s->next;
    else s_online = s->next;
    if (s->next) s->next->prev = s->prev;
    s->prev = s->next = NULL;
}

static void presence_add(SocialSession *s) {
    presence_remove(s);
    s->next = s_online;
    if (s_online) s_online->prev = s;
    s_online = s;
}

/* ------------------------------------------------------------------ sending */

void social_send(SocialSession *s, cJSON *frame) {
    if (!frame) return;
    if (social_open(s)) {
        if (s->json) {
            char *text = kn_json_print(frame);
            if (text) {
                kn_ws_send_text(s->ws, text, strlen(text));
                free(text);
            }
        } else {
            kn_buf out;
            kn_buf_init(&out);
            if (kn_krunker_packet_encode(frame, &out) == 0) kn_ws_send_binary(s->ws, out.data, out.len);
            kn_buf_free(&out);
        }
    }
    cJSON_Delete(frame);
}

cJSON *social_frame(const char *label) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(label));
    return f;
}

void social_send_n(SocialSession *s, const char *label, int n, ...) {
    cJSON *f = social_frame(label);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        cJSON *arg = va_arg(ap, cJSON *);
        cJSON_AddItemToArray(f, arg ? arg : cJSON_CreateNull());
    }
    va_end(ap);
    social_send(s, f);
}

void social_error(SocialSession *s, const char *msg) {
    social_send_n(s, "po", 2, cJSON_CreateNull(), cJSON_CreateString(msg ? msg : "Error"));
}

int social_require_auth(SocialSession *s) {
    if (s->account_id > 0) return 1;
    social_error(s, "Must be logged in");
    return 0;
}

void social_push_account_ex(int64_t account_id, cJSON *frame, int json_only) {
    if (!frame) return;
    for (SocialSession *s = s_online; s; s = s->next) {
        if (s->account_id == account_id && social_open(s) && (!json_only || s->json))
            social_send(s, cJSON_Duplicate(frame, 1));
    }
    cJSON_Delete(frame);
}

void social_push_account(int64_t account_id, cJSON *frame) { social_push_account_ex(account_id, frame, 0); }

cJSON *social_account_info(const cJSON *account) {
    cJSON *info = cJSON_CreateObject();
    const char *keys[] = {"level", "levelProg", "alias", "premium", "funds", "score", "clan", "dev", "mod", "banned", "badges"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(account, keys[i]);
        if (v) cJSON_AddItemToObject(info, keys[i], cJSON_Duplicate(v, 1));
    }
    return info;
}

void social_push_account_update(const cJSON *account) {
    const int64_t id = kn_json_int(account, "id", 0);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(account, "data");
    if (id <= 0 || !cJSON_IsArray(data)) return;
    cJSON *f = social_frame("ua");
    cJSON_AddItemToArray(f, cJSON_Duplicate(data, 1));
    cJSON_AddItemToArray(f, social_account_info(account));
    social_push_account_ex(id, f, 1);
}

void social_email_state_push(int64_t account_id, const cJSON *state, SocialSession *except) {
    const int verified = (int) kn_json_int(state, "verified", 0);
    for (SocialSession *s = s_online; s; s = s->next) {
        if (s->account_id != account_id || !social_open(s)) continue;
        s->email_verified = verified;
        s->email_state_known = 1;
        if (s != except && s->json) social_send_n(s, "emailS", 1, cJSON_Duplicate(state, 1));
    }
}

int social_ws_online(void) {
    int n = 0;
    for (SocialSession *s = s_online; s; s = s->next) n++;
    return n;
}

const char *social_viewer(const SocialSession *s, char buf[24]) { return acct_itoa(buf, s ? s->account_id : 0); }

/* ------------------------------------------------------------------ frame args */

const cJSON *fa(const cJSON *frame, int i) { return cJSON_GetArrayItem(frame, i); }

int64_t fa_int(const cJSON *frame, int i, int64_t def) {
    const cJSON *v = fa(frame, i);
    if (cJSON_IsNumber(v)) return (int64_t) v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end = NULL;
        const long long x = strtoll(v->valuestring, &end, 10);
        if (end && *end == 0) return x;
    }
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return def;
}

const char *fa_str(const cJSON *frame, int i) {
    const cJSON *v = fa(frame, i);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ------------------------------------------------------------------ async queries */

typedef struct SocialQuery {
    SocialSession *s;
    SocialDbFn fn;
    void *extra;
} SocialQuery;

static void social_query_done(kn_db_result *res, void *ud) {
    SocialQuery *q = ud;
    if (social_open(q->s)) q->fn(q->s, res, q->extra);
    SocialSession *s = q->s;
    free(q->extra);
    free(q);
    social_unhold(s);
}

static int social_db(SocialSession *s, const kn_sql *sql, const kn_proc *proc, int nparams,
                     const char *const *params, SocialDbFn fn, void *extra) {
    if (!g_acct_brain || !g_acct_brain->db) {
        social_error(s, "Server unavailable");
        free(extra);
        return -1;
    }
    SocialQuery *q = calloc(1, sizeof *q);
    if (!q) {
        free(extra);
        return -1;
    }
    q->s = social_hold(s);
    q->fn = fn;
    q->extra = extra;
    int rc = proc ? kn_db_call(g_acct_brain->db, proc, nparams, params, social_query_done, q)
                  : kn_db_query_sql(g_acct_brain->db, sql, nparams, params, social_query_done, q);
    /* the db calls back on every path but a refused submit; a non-zero return means no callback */
    if (rc != 0) {
        free(q->extra);
        free(q);
        social_unhold(s);
        return -1;
    }
    return 0;
}

int social_query(SocialSession *s, const kn_sql *sql, int nparams, const char *const *params, SocialDbFn fn,
                 void *extra) {
    return social_db(s, sql, NULL, nparams, params, fn, extra);
}

int social_call(SocialSession *s, const kn_proc *proc, int nparams, const char *const *params, SocialDbFn fn,
                void *extra) {
    return social_db(s, NULL, proc, nparams, params, fn, extra);
}

/* ------------------------------------------------------------------ auth ("a") */

typedef struct AuthWait {
    SocialSession *s;
    char *name, *pass;    /* password logins: kept for the 2FA step */
    int twofa;            /* this request carried a 2FA code */
    int action;
    char email[256];      /* register: the optional address (KrunkNative), "" none */
} AuthWait;

static void auth_wait_free(AuthWait *w) {
    if (w->pass) memset(w->pass, 0, strlen(w->pass));
    free(w->pass);
    free(w->name);
    free(w);
}

/* index.js twoFAres(0, "2fa", null, null, message): the code prompt of a password login */
static void twofa_login_prompt(SocialSession *s, const char *msg) {
    social_send_n(s, "c2fa", 5, cJSON_CreateNumber(0), cJSON_CreateString("2fa"), cJSON_CreateNull(), cJSON_CreateNull(),
                  msg ? cJSON_CreateString(msg) : cJSON_CreateNull());
}

static void send_auth_error(SocialSession *s, const char *err) {
    social_send_n(s, "a", 5, cJSON_CreateString(err), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNull(),
                  cJSON_CreateNull());
}

static void auth_reply(void *ud, const char *err, const cJSON *account) {
    AuthWait *w = ud;
    SocialSession *s = w->s;
    s->auth_busy = 0;
    if (!social_open(s)) {
        auth_wait_free(w);
        social_unhold(s);
        return;
    }
    if (err && (!strcmp(err, ACCT_ERR_2FA) || (w->twofa && !strcmp(err, ACCT_ERR_2FA_BAD))) && w->name && w->pass) {
        /* v7.1.4 twoFAres(0, "2fa"): the code popup; ["2fa", code] continues the login */
        clear_pending(s);
        s->pending_name = w->name;
        s->pending_pass = w->pass;
        w->name = w->pass = NULL;
        auth_wait_free(w);
        twofa_login_prompt(s, !strcmp(err, ACCT_ERR_2FA_BAD) ? err : NULL);
        social_unhold(s);
        return;
    }
    if (w->twofa) social_send_n(s, "c2fa", 1, cJSON_CreateNumber(1)); /* twoFAres(1): hide the popup */
    char reg_email[256];
    snprintf(reg_email, sizeof reg_email, "%s", !err && account && w->action == 0 ? w->email : "");
    auth_wait_free(w);
    if (err || !account) {
        send_auth_error(s, err ? err : "Invalid login.");
        social_unhold(s);
        return;
    }
    if (s->account_id != kn_json_int(account, "id", 0)) s->email_verified = s->email_state_known = 0;
    s->account_id = kn_json_int(account, "id", 0);
    snprintf(s->username, sizeof s->username, "%s", kn_json_str(account, "username", ""));
    snprintf(s->token, sizeof s->token, "%s", kn_json_str(account, "token", ""));
    presence_add(s);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(account, "data");
    cJSON *info = social_account_info(account);
    social_send_n(s, "a", 6, cJSON_CreateNull(), cJSON_CreateNumber((double) s->account_id),
                  cJSON_CreateString(s->username), data ? cJSON_Duplicate(data, 1) : cJSON_CreateArray(),
                  s->token[0] ? cJSON_CreateString(s->token) : cJSON_CreateNull(), info);
    KN_DEBUG(MOD, "%s logged in as %s", s->socket_id, s->username);
    social_after_login(s);
    if (reg_email[0]) acct_email_after_register(s->account_id, reg_email, s->ip); /* pushes "emailS" when done */
    else sh_email_state(s, NULL);
    social_unhold(s);
}

static void revoke_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) s;
    (void) extra;
    if (!kn_db_ok(res)) KN_WARN(MOD, "logout: %s", kn_db_error(res));
}

static void sh_auth(SocialSession *s, const cJSON *f) {
    const int64_t action = fa_int(f, 1, 1);
    if (action == -1) {
        /* logout: forget the account; the client drops its saved token, so revoke it */
        presence_remove(s);
        if (s->token[0])
            social_query(s, SQL(.pg = "DELETE FROM sessions WHERE token_hash = sha256(convert_to($1, 'UTF8'))",
                                .any = "DELETE FROM sessions WHERE token_hash = {hash:$1}"), KN_DB_ARGS(s->token),
                         revoke_done, NULL);
        s->account_id = 0;
        s->username[0] = 0;
        s->email_verified = s->email_state_known = 0;
        clear_pending(s);
        memset(s->token, 0, sizeof s->token);
        return;
    }
    if (s->auth_busy) return;
    const cJSON *payload = fa(f, 2);
    const cJSON *p0 = cJSON_GetArrayItem(payload, 0), *p1 = cJSON_GetArrayItem(payload, 1),
                *p2 = cJSON_GetArrayItem(payload, 2);
    const char *name = cJSON_IsString(p0) ? p0->valuestring : NULL;
    const char *pass = cJSON_IsString(p1) ? p1->valuestring : NULL;
    const char *third = cJSON_IsString(p2) ? p2->valuestring : NULL;
    if (!cJSON_IsArray(payload) || !name) {
        send_auth_error(s, "Invalid credentials.");
        return;
    }
    if (action != 0 && action != 1) {
        send_auth_error(s, "Unknown auth action.");
        return;
    }
    if (action == 0 && !pass) {
        send_auth_error(s, "Invalid credentials.");
        return;
    }
    /* KrunkNative: register takes an optional email as the payload's 4th element (v7.1.4: none) */
    const cJSON *p3 = cJSON_GetArrayItem(payload, 3);
    char email[256] = "";
    if (action == 0 && cJSON_IsString(p3) && p3->valuestring[0]) {
        const char *eerr = acct_validate_email(p3->valuestring, email, sizeof email);
        if (eerr) {
            send_auth_error(s, eerr);
            return;
        }
    } else if (action == 0 && acct_email_required()) {
        send_auth_error(s, "Email Required");
        return;
    }
    AuthWait *w = calloc(1, sizeof *w);
    if (!w) return;
    w->s = social_hold(s);
    w->action = (int) action;
    snprintf(w->email, sizeof w->email, "%s", email);
    if (action == 1 && pass) {
        w->name = strdup(name);
        w->pass = strdup(pass);
    }
    s->auth_busy = 1;
    AcctAuthReq rq = {
        .action = (int) action,
        .name = name,
        .pass = pass,
        .token = action == 1 && !pass ? third : NULL,
        .ref = action == 0 ? third : NULL,
        .ip = s->ip,
        .origin = "social",
    };
    if (action == 0 && cJSON_IsNumber(p2)) rq.ref = NULL;
    acct_auth(&rq, auth_reply, w);
}

/* ["2fa", code]: the authenticator code of a pending password login (v7.1.4 sub2FA with
 * nextLabel "2fa") -> ["c2fa", 1] + the "a" reply, or the prompt again with the reason. */
static void sh_twofa(SocialSession *s, const cJSON *f) {
    if (!s->pending_name || !s->pending_pass || s->auth_busy) return;
    char code[16] = "";
    const char *str = fa_str(f, 1);
    if (str) snprintf(code, sizeof code, "%s", str);
    else if (cJSON_IsNumber(fa(f, 1))) snprintf(code, sizeof code, "%06lld", (long long) fa_int(f, 1, 0));
    AuthWait *w = calloc(1, sizeof *w);
    if (!w) return;
    w->s = social_hold(s);
    w->name = s->pending_name;
    w->pass = s->pending_pass;
    w->twofa = 1;
    w->action = 1;
    s->pending_name = s->pending_pass = NULL;
    s->auth_busy = 1;
    AcctAuthReq rq = {.action = 1, .name = w->name, .pass = w->pass, .ip = s->ip, .origin = "social",
                      .twofa = code[0] ? code : "-"};
    acct_auth(&rq, auth_reply, w);
}

static void las_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) social_error(s, acct_db_error(res, "las"));
}

/* ["las"] log out every other session of the account (index.js logoutSessions) */
static void sh_logout_all(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    char me[24];
    social_query(s, SQL(.pg = "DELETE FROM sessions WHERE account_id = $1 AND token_hash <> sha256(convert_to($2, 'UTF8'))",
                        .any = "DELETE FROM sessions WHERE account_id = $1 AND token_hash <> {hash:$2}"),
                 KN_DB_ARGS(social_viewer(s, me), s->token), las_done, NULL);
}

static void sh_pong(SocialSession *s, const cJSON *f) {
    (void) s;
    (void) f;
}

/* ------------------------------------------------------------------ dispatch */

typedef struct SocialRoute {
    const char *label;
    SocialHandler fn;
} SocialRoute;

static const SocialRoute k_routes[] = {
    {"a", sh_auth},
    {"po", sh_pong},
    {"2fa", sh_twofa},
    {"a2FA", sh_twofa_enable},          /* v7.1.4 act2FA (account_security.c) */
    {"a2FAc", sh_twofa_enable_confirm},
    {"d2FA", sh_twofa_disable},         /* dact2FA */
    {"d2FAc", sh_twofa_disable_confirm},
    {"cemail", sh_email_set},           /* KrunkNative email verification */
    {"emailv", sh_email_resend},
    {"emailS", sh_email_state},
    {"las", sh_logout_all},
    {"r", sh_page},
    {"sp", sh_search},
    {"f", sh_follow},
    {"gfs", sh_followers},
    {"gfg", sh_following},
    {"pst", sh_profile_stats},
    {"guf", sh_user_feed},
    {"grp", sh_reports},
    {"rpt", sh_report},
    {"flg", sh_flag},
    {"ver", sh_verify},
    {"ps", sh_post},
    {"com", sh_comment},
    {"gps", sh_get_post},
    {"lps", sh_like_post},
    {"dps", sh_delete_post},
    {"chtb", sh_chat_blobs},
    {"ctd", sh_chat_thread},
    {"ict", sh_chat_send},
    {"kr", sh_gift},
    {"cln", sh_clan},
    {"clnR", sh_clan_request},
    {"uploadS", sh_settings_up},
    {"getS", sh_settings_get},
    {"nme", sh_alias},
    {"upp", sh_password},
    {"kml", sh_mail_list},
    {"rml", sh_mail_read},
    {"raml", sh_mail_read_all},
    {"li", sh_market_list},
    {"uli", sh_market_unlist},
    {"pi", sh_market_buy},
    {"fil", sh_market_filter},
    {"srt", sh_inventory_sort},
    {"qsi", sh_quick_sell},
    {"st", sh_item_stats},
    {"ri", sh_trade_inv},
    {"ctr", sh_trade_create},
    {"trA", sh_trade_accept},
    {"trC", sh_trade_cancel},
    {"trCA", sh_trade_clear},
    {"uploadM", sh_upload_map},
    {"spin", sh_spin},
    {"fkr", sh_free_kr},
    {"clm", sh_claim},
    {"chg", sh_challenges},   /* WP-E part 2c (progress.c) */
    {"bdg", sh_badge_select},
    {"cbdg", sh_badge_claim},
    {"mdl", sh_medals},
    {"bpdl", sh_bp_data},     /* v7.1.4 seasons / Battle Pass (season.c) */
    {"bpclm", sh_bp_claim},
    {"xstbp", sh_bp_buy},
    {"xstbpl", sh_bp_skip},
    {"sst", sh_season_stats},
    {"unbx", sh_noop},
    {"edMp", sh_edit_map},
    {"fvM", sh_fav_creation},
    {"ftC", sh_feature_creation},
    {"delC", sh_delete_creation},
    {"uploadMo", sh_upload_files},
    {"uploadA", sh_upload_files},
};

static int rate_ok(SocialSession *s) {
    const uint64_t now = kn_now_ms();
    if (!s->bucket_ms) {
        s->bucket = RATE_BURST;
        s->bucket_ms = now;
    }
    s->bucket += (double) (now - s->bucket_ms) * RATE_PER_S / 1000.0;
    if (s->bucket > RATE_BURST) s->bucket = RATE_BURST;
    s->bucket_ms = now;
    if (s->bucket < 1.0) return 0;
    s->bucket -= 1.0;
    return 1;
}

static void dispatch(SocialSession *s, const cJSON *frame) {
    const cJSON *label = cJSON_GetArrayItem(frame, 0);
    if (!cJSON_IsArray(frame) || !cJSON_IsString(label)) return;
    for (size_t i = 0; i < sizeof k_routes / sizeof k_routes[0]; i++) {
        if (!strcmp(k_routes[i].label, label->valuestring)) {
            if (!social_email_gate(s, label->valuestring, frame)) return; /* accounts.email.require_verified */
            k_routes[i].fn(s, frame);
            return;
        }
    }
    KN_DEBUG(MOD, "%s: unknown label \"%s\"", s->socket_id, label->valuestring); /* dropped, like socialWs.ts */
}

/* ------------------------------------------------------------------ WebSocket callbacks */

static void on_open(kn_ws *ws, void *ud) {
    SocialSession *s = ud;
    s->ws = ws; /* on_open may run inside kn_ws_accept, before it returns */
    social_send_n(s, "pi", 1, cJSON_CreateNull());
    social_send_n(s, "io-init", 1, cJSON_CreateString(s->socket_id));
}

static void on_message(kn_ws *ws, const void *data, size_t len, int is_binary, void *ud) {
    (void) ws;
    SocialSession *s = ud;
    cJSON *frame = NULL;
    if (is_binary) {
        frame = kn_krunker_packet_decode(data, len);
        if (!frame) frame = kn_msgpack_to_json(data, len, NULL);
        if (frame) s->json = 0;
    } else {
        frame = kn_json_parse(data, len);
        if (frame) s->json = 1;
    }
    if (!cJSON_IsArray(frame)) {
        cJSON_Delete(frame);
        return;
    }
    if (!rate_ok(s)) {
        cJSON_Delete(frame);
        return;
    }
    if (s->busy > 0 || s->q_head) {
        /* the previous request is still running: keep the socket's order */
        if (s->q_len >= QUEUE_MAX) {
            KN_WARN(MOD, "%s: request queue full, dropping a frame", s->socket_id);
            cJSON_Delete(frame);
            return;
        }
        SocialFrameQ *n = calloc(1, sizeof *n);
        if (!n) {
            cJSON_Delete(frame);
            return;
        }
        n->frame = frame;
        if (s->q_tail) s->q_tail->next = n;
        else s->q_head = n;
        s->q_tail = n;
        s->q_len++;
        return;
    }
    social_retain(s);
    dispatch(s, frame);
    social_release(s);
    cJSON_Delete(frame);
}

static void on_close(kn_ws *ws, int code, const char *reason, void *ud) {
    (void) ws;
    (void) code;
    (void) reason;
    SocialSession *s = ud;
    presence_remove(s);
    clear_pending(s);
    queue_clear(s);
    s->ws = NULL;
    s_open_count--;
    social_release(s);
}

static void ws_route(kn_http_req *req, void *ud) {
    (void) ud;
    if (!kn_http_is_ws_upgrade(req)) {
        kn_http_respond_error(req, 426, "WebSocket upgrade required");
        return;
    }
    SocialSession *s = calloc(1, sizeof *s);
    if (!s) {
        kn_http_respond_error(req, 500, "out of memory");
        return;
    }
    s->refs = 1;
    char enc[16];
    if (kn_http_query_get(req, "enc", enc, sizeof enc) > 0 && !strcmp(enc, "json")) s->json = 1;
    snprintf(s->ip, sizeof s->ip, "%s", req->remote_addr ? req->remote_addr : "");
    if (kn_random_hex(s->socket_id, sizeof s->socket_id, 8) != 0) snprintf(s->socket_id, sizeof s->socket_id, "%p", (void *) s);
    static const kn_ws_callbacks cbs = {.on_open = on_open, .on_message = on_message, .on_close = on_close};
    kn_ws_options opts = {0};
    opts.max_message_bytes = 2u << 20;
    opts.ping_interval_ms = 30000;
    kn_ws *ws = kn_ws_accept(req, &cbs, &opts, s);
    if (!ws) {
        free(s);
        return;
    }
    s->ws = ws;
    s_open_count++;
}

int social_ws_init(Brain *b) {
    if (!b->http) return 0;
    kn_http_route(b->http, "GET", "/api/ws", ws_route, NULL);
    kn_http_route(b->http, "GET", "/social/ws", ws_route, NULL);
    KN_INFO(MOD, "social socket on /api/ws and /social/ws%s", b->db ? "" : " (no database: guests only)");
    return 0;
}

void social_ws_shutdown(Brain *b) {
    (void) b;
    /* knsvc closes every connection when the HTTP server is freed (on_close runs). */
}
