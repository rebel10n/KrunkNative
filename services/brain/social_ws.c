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
#include <strings.h>

#include "hot.h"
#include "knsvc/buf.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knsvc/msgpack.h"
#include "knsvc/ws.h"

#define MOD "social"

static SocialSession *s_online;   /* authenticated sessions */
static int s_open_count;
static kn_loop *s_loop;           /* the brain's loop: the flush timers of the coalesced pushes */

/* Coalesced pushes leave this long after the first one of a burst (the newest value of each key
 * is what goes out). Outbound pushes are not charged to the client's rate limit (doc). */
#define OUT_FLUSH_MS 100
#define OUT_MAX 128       /* pending keys per connection; a fuller map is flushed at once */
/* a passing frame that finds the socket's queue full is refused with this retry time */
#define QUEUE_RETRY_MS 250

/* ------------------------------------------------------------------ session lifetime */

SocialSession *social_retain(SocialSession *s) {
    if (s) s->refs++;
    return s;
}

/* what an inbound frame is, for the limiter and its answer (see the rate limits section) */
typedef enum {
    KIND_CLASS,      /* a route: charged to its class */
    KIND_UNKNOWN,    /* a label that is no route (or an eb type that is none) */
    KIND_MALFORMED,  /* no label: not an array, or no string first element */
    KIND_EXEMPT      /* "po", the anti-hack reply: no answer, never limited */
} RateKind;

typedef enum {
    QM_DISPATCH,     /* run the frame's handler */
    QM_ANSWER        /* answer the frame (refused, unknown or malformed) */
} QueueMode;

typedef struct SocialFrameQ {
    cJSON *frame;
    QueueMode mode;
    RateKind kind;              /* QM_ANSWER */
    int64_t retry_ms;           /* QM_ANSWER: > 0 when refused */
    struct SocialFrameQ *next;
} SocialFrameQ;

#define QUEUE_MAX 128

static void dispatch(SocialSession *s, const cJSON *frame);
static void answer(SocialSession *s, const cJSON *frame, RateKind kind, int64_t retry_ms);

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
        if (n->mode == QM_ANSWER) answer(s, n->frame, n->kind, n->retry_ms);
        else dispatch(s, n->frame);
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

static void out_clear(SocialSession *s) {
    for (int i = 0; i < s->out_n; i++) cJSON_Delete(s->out[i].frame);
    free(s->out);
    s->out = NULL;
    s->out_n = s->out_cap = 0;
}

void social_release(SocialSession *s) {
    if (!s || --s->refs > 0) return;
    memset(s->token, 0, sizeof s->token);
    clear_pending(s);
    queue_clear(s);
    out_clear(s);
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

int social_account_online(int64_t account_id) {
    for (SocialSession *o = s_online; o; o = o->next)
        if (o->account_id == account_id && account_id > 0) return 1;
    return 0;
}

static void presence_add(SocialSession *s) {
    presence_remove(s);
    const int first = !social_account_online(s->account_id);
    s->next = s_online;
    if (s_online) s_online->prev = s;
    s_online = s;
    hot_presence_add(s->account_id);
    if (first) social_eb_presence(s->account_id, 1); /* v10 friends: presence:online (social_eb.c) */
}

/* Unlink and, when it was the account's last socket here, drop this brain from its presence. */
static void presence_leave(SocialSession *s) {
    const int64_t id = s->account_id;
    const int linked = s->prev || s->next || s_online == s;
    presence_remove(s);
    if (!linked || id <= 0) return;
    for (SocialSession *o = s_online; o; o = o->next)
        if (o->account_id == id) return;
    hot_presence_remove(id);
    social_eb_presence(id, 0); /* v10 friends: presence:offline unless online on another brain */
}

/* hot.h presence refresh: this brain's online account ids */
static size_t presence_ids(int64_t *ids, size_t max) {
    size_t n = 0;
    for (SocialSession *s = s_online; s; s = s->next) {
        if (s->account_id <= 0) continue;
        if (n < max) ids[n] = s->account_id;
        n++;
    }
    return n;
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

/* ------------------------------------------------------------------ coalesced state pushes
 *
 * A state push carries the whole current value of one thing (a friend's presence, a friend's
 * game, the friends page, the mail count, the account header), so while one is waiting on a
 * socket the newer one replaces it: the socket gets each key's newest value once, OUT_FLUSH_MS
 * after the first of the burst. Other pushes (a chat line, a challenge result) are sent at once. */

/* the coalescing key of a state push, or 0 (sent as it is) */
static int state_key(const cJSON *frame, char *key, size_t n) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return 0;
    if (!strcmp(label, "gmc") || !strcmp(label, "ua")) {
        snprintf(key, n, "%s", label);
        return 1;
    }
    if (!strcmp(label, "eb")) {
        const cJSON *o = fa(frame, 1);
        const char *type = kn_json_str(o, "type", "");
        if (!strcmp(type, "presence:online") || !strcmp(type, "presence:offline")) {
            snprintf(key, n, "presence:%lld", (long long) kn_json_int(o, "id", 0));
            return 1;
        }
        if (!strcmp(type, "presence:game")) {
            snprintf(key, n, "game:%lld", (long long) kn_json_int(o, "id", 0));
            return 1;
        }
        if (!strcmp(type, "friends:list")) {
            snprintf(key, n, "friends");
            return 1;
        }
    }
    return 0;
}

static void out_flush(SocialSession *s) {
    SocialOutItem *items = s->out;
    const int n = s->out_n;
    s->out = NULL;
    s->out_n = s->out_cap = 0;
    for (int i = 0; i < n; i++) social_send(s, items[i].frame); /* consumes each frame */
    free(items);
}

static void out_timer_cb(kn_loop *loop, void *ud) {
    (void) loop;
    SocialSession *s = ud;
    s->out_timer = 0;
    out_flush(s);
    social_release(s); /* the timer's reference */
}

/* Queues the newest value of `key` for the socket (consumes frame). */
static void out_put(SocialSession *s, const char *key, cJSON *frame) {
    for (int i = 0; i < s->out_n; i++) {
        if (strcmp(s->out[i].key, key)) continue;
        cJSON_Delete(s->out[i].frame);
        s->out[i].frame = frame; /* the key keeps its place: the first push of the burst */
        return;
    }
    if (s->out_n >= OUT_MAX || !s_loop) {
        out_flush(s);
        if (!s_loop) {
            social_send(s, frame);
            return;
        }
    }
    if (s->out_n == s->out_cap) {
        const int cap = s->out_cap ? s->out_cap * 2 : 8;
        SocialOutItem *grown = realloc(s->out, (size_t) cap * sizeof *grown);
        if (!grown) {
            social_send(s, frame);
            return;
        }
        s->out = grown;
        s->out_cap = cap;
    }
    SocialOutItem *it = &s->out[s->out_n++];
    snprintf(it->key, sizeof it->key, "%s", key);
    it->frame = frame;
    if (!s->out_timer) {
        s->out_timer = kn_loop_add_timer(s_loop, OUT_FLUSH_MS, 0, out_timer_cb, social_retain(s));
        if (!s->out_timer) { /* no timer: send now (drops the reference taken for it) */
            social_release(s);
            out_flush(s);
        }
    }
}

/* This brain's sockets of the account (also what other brains' pushes arrive as, hot.h). */
static void push_local(int64_t account_id, cJSON *frame, int json_only) {
    if (!frame) return;
    char key[48];
    const int coalesce = state_key(frame, key, sizeof key);
    for (SocialSession *s = s_online; s; s = s->next) {
        if (s->account_id != account_id || !social_open(s) || (json_only && !s->json)) continue;
        if (coalesce) out_put(s, key, cJSON_Duplicate(frame, 1));
        else social_send(s, cJSON_Duplicate(frame, 1));
    }
    cJSON_Delete(frame);
}

void social_push_account_ex(int64_t account_id, cJSON *frame, int json_only) {
    if (!frame) return;
    hot_push(account_id, frame, json_only); /* sockets of the account on other brains (Redis presence) */
    push_local(account_id, frame, json_only);
}

void social_push_account(int64_t account_id, cJSON *frame) { social_push_account_ex(account_id, frame, 0); }

/* A frame to every signed-in JSON social socket of this brain (a brain-wide notice, e.g. the
 * ranked queue start): the sockets of other brains get it from their own queue. */
void social_push_all_json(cJSON *frame) {
    if (!frame) return;
    for (SocialSession *s = s_online; s; s = s->next) {
        if (social_open(s) && s->json) social_send(s, cJSON_Duplicate(frame, 1));
    }
    cJSON_Delete(frame);
}

cJSON *social_account_info(const cJSON *account) {
    cJSON *info = cJSON_CreateObject();
    const char *keys[] = {"level", "levelProg", "alias", "premium", "funds", "score", "clan", "dev", "mod", "banned", "badges",
                          "creator_badge", /* v10's creatorBadge (0210_creator_badge) */
                          "dxp",           /* v10 double XP: the 2x XP offer and the weekend schedules (0341_double_xp) */
                          "junk"};         /* the Junk wallet's balance, "12.34" (0347_junk_wallet, doc/v10/junk.md) */
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(account, keys[i]);
        if (v) cJSON_AddItemToObject(info, keys[i], cJSON_Duplicate(v, 1));
    }
    return info;
}

void social_push_junk(const int64_t account_id, const char *junk) {
    if (account_id <= 0) return;
    cJSON *f = social_frame("uj");
    cJSON_AddItemToArray(f, cJSON_CreateString(junk && *junk ? junk : "0.00"));
    social_push_account(account_id, f);
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
    const int registered = !err && account && w->action == 0;
    auth_wait_free(w);
    if (err || !account) {
        send_auth_error(s, err ? err : "Invalid login.");
        social_unhold(s);
        return;
    }
    if (s->account_id != kn_json_int(account, "id", 0)) s->email_verified = s->email_state_known = 0;
    /* the Account Manager's switch signs in another account on this socket with no logout first: the account
     * left goes offline for its friends (v10: presence:offline when its event bus socket closes). Its session
     * token stays good; only logout revokes it. */
    if (s->account_id > 0 && s->account_id != kn_json_int(account, "id", 0)) presence_leave(s);
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
    if (registered) rewards_after_register(s); /* v10.0.2: the signup perks right after the registration ("prr", rewards.c) */
    social_eb_after_login(s); /* v10 friends: unread:summary */
    if (reg_email[0]) acct_email_after_register(s->account_id, reg_email, s->ip); /* pushes "emailS" when done */
    else sh_email_state(s, NULL);
    social_unhold(s);
}

static void logout_revoked(void *ud) { social_unhold(ud); }

static void sh_auth(SocialSession *s, const cJSON *f) {
    const int64_t action = fa_int(f, 1, 1);
    if (action == -1) {
        /* logout: forget the account; the client drops its saved token, so revoke it
         * (acct_revoke_token also drops it from the hot tier's session cache once deleted) */
        presence_leave(s);
        /* the socket's next frames wait for the delete, as with every other database write */
        if (s->token[0]) acct_revoke_token_then(s->token, logout_revoked, social_hold(s));
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
    /* KrunkNative: register takes an email as the payload's 4th element (v7.1.4: none; the v10 modal
     * asks for it, accounts.email.required makes the brain insist). Login: the name may be an email. */
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
        .email = email[0] ? email : NULL, /* register: refused when another account has it */
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

typedef struct LasCtx {
    SocialSession *s;
    int64_t account_id;
} LasCtx;

/* not social_query: the cached sessions must be dropped even when the socket closed meanwhile */
static void las_done(kn_db_result *res, void *ud) {
    LasCtx *c = ud;
    if (kn_db_ok(res)) hot_session_forget_account(c->account_id);
    else if (social_open(c->s)) social_error(c->s, acct_db_error(res, "las"));
    social_unhold(c->s);
    free(c);
}

/* ["las"] log out every other session of the account (index.js logoutSessions) */
static void sh_logout_all(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    LasCtx *c = calloc(1, sizeof *c);
    if (!c || !g_acct_brain || !g_acct_brain->db) {
        free(c);
        social_error(s, "Server unavailable");
        return;
    }
    c->s = social_hold(s);
    c->account_id = s->account_id;
    char me[24];
    if (kn_db_query_sql(g_acct_brain->db,
                        SQL(.pg = "DELETE FROM sessions WHERE account_id = $1 AND token_hash <> sha256(convert_to($2, 'UTF8'))",
                            .any = "DELETE FROM sessions WHERE account_id = $1 AND token_hash <> {hash:$2}"),
                        KN_DB_ARGS(social_viewer(s, me), s->token), las_done, c) != 0) {
        social_unhold(s);
        free(c);
    }
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
    {"rpr", sh_reset_request},          /* KrunkNative forgot password (account_security.c) */
    {"rpc", sh_reset_check},
    {"rps", sh_reset_set},
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
    {"crt", sh_creator},               /* KrunkNative: v10's creator badge on a profile (profiles.c) */
    {"sbdg", sh_staff_badge},          /* KrunkNative: Give Badge / Remove on a profile (profiles.c) */
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
    {"ttl", sh_titles},                 /* v10: the account's titles (v10_customize.c) */
    {"dsp", sh_daily_spin},             /* v10 Daily Spin progress (daily_spin.c) */
    {"dsl", sh_daily_spin_claim},       /* v10 DAILY_SPIN_CLAIM -> "dslr" */
    {"vr", sh_redeem_voucher},          /* v10 REDEEM_VOUCHER -> "vr" (redeem.c) */
    {"recps", sh_craft_recipes},        /* v10 CRAFT_DATA_REQUEST -> "recps" (craft.c, the Junkyard) */
    {"crft", sh_craft},                 /* v10 CRAFT_REQUEST -> "crftr" (craft.c) */
    {"wo", sh_wild_offer},              /* v10 shop.getWildOffer (wild_offer.c) */
    {"wob", sh_wild_offer_buy},         /* v10 shop.purchaseWildOffer */
    {"wor", sh_wild_offer_refresh},     /* v10 shop.refreshWildOffer */
    {"rqdxp", sh_double_xp_claim},      /* v10 2x XP claim after the end-screen offer -> "rrqdxp" (double_xp_social.c) */
    {"clnRA", sh_clan_decline_all},     /* v10 clan window: Decline All */
    {"vcln", sh_clan_viewed},           /* v10 OPEN_CLAN_TAB */
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
    {"ecfg", sh_economy_config},        /* KrunkNative: the economy rules (economy.c) */
    {"krh", sh_kr_history},             /* KrunkNative: the account's KR ledger */
    {"eb", sh_eb},                      /* v10 friends event bus (social_eb.c) */
    {"gapi", sh_gapi},                  /* v10 gapi requests (social_gapi.c, clan_v10.c) */
    {"uml", sh_listings_of},            /* v7.1.4 profile "Listings" tab -> "ul" */
    {"pbm", sh_blackm_buy},             /* v7.1.4 Black Market purchase -> "bmr" */
    {"uploadM", sh_upload_map},
    {"spin", sh_spin},
    {"fkr", sh_free_kr},
    {"spinH", sh_spin_history},
    {"sfkr", sh_noop},        /* v7.1.4 Free KR ad: started / loaded / failed (no ads here) */
    {"lfkr", sh_noop},
    {"fkrf", sh_noop},
    {"clm", sh_claim},
    {"chg", sh_challenges},   /* WP-E part 2c (progress.c) */
    {"bdg", sh_badge_select},
    {"cbdg", sh_badge_claim},
    {"mdl", sh_medals},
    {"bpdl", sh_bp_data},     /* v7.1.4 seasons / Battle Pass (season.c) */
    {"bpclm", sh_bp_claim},
    {"bpclmAll", sh_bp_claim_all}, /* v10.0.2 Claim All */
    {"xstbp", sh_bp_buy},
    {"xstbpl", sh_bp_skip},
    {"sst", sh_season_stats},
    {"unbx", sh_noop},
    {"edMp", sh_edit_map},
    {"ulb", sh_update_map_lb}, /* v10 updateMapLB: the Leaderboard Display of an own map */
    {"fvM", sh_fav_creation},
    {"ftC", sh_feature_creation},
    {"delC", sh_delete_creation},
    {"uploadMo", sh_upload_files},
    {"uploadA", sh_upload_files},
};

/* ------------------------------------------------------------------ rate limits
 *
 * Every inbound frame but "po" (the anti-hack reply) takes one token from the bucket of its
 * class on this connection. The limits, the classes and the answers are documented in
 * doc/social-rate-limit.md: keep that table and k_rate / k_label_class / k_eb_class in step.
 *
 * A refused frame is answered, never silently dropped, with one exception: a NOTICE frame
 * (no reply slot) refused while a notice for its class is already out gets no answer of its own.
 * A refused frame with a reply slot is answered in order, behind the work in flight. */

typedef enum {
    RC_AUTH,    /* login, register, 2FA, password and e-mail codes, logout everywhere */
    RC_READ,    /* gapi GET, search, feed, profiles, lists, eb friends:list and msg:history */
    RC_WRITE,   /* gapi POST/PUT/DELETE, eb msg:send / msg:read / game:invite, posts, follows, market, clans, gifts */
    RC_LIVE,    /* eb presence:ping */
    RC_NOTICE   /* fire-and-forget frames: the client's analytics events and unknown labels */
} RateClass;

typedef enum {
    ANS_REPLY,  /* every refused frame gets its own reply (gapi 429, eb error, "po" or "a" error) */
    ANS_NOTICE  /* no reply slot: one notice per refusal window */
} RateAnswer;

typedef struct RateSpec {
    double per_s;   /* tokens refilled per second */
    double burst;   /* bucket size: the most frames that pass back to back */
} RateSpec;

/* indexed by RateClass; the same numbers as doc/social-rate-limit.md */
static const RateSpec k_rate[SOCIAL_RATE_CLASSES] = {
    {0.5, 10},   /* auth */
    {50.0, 200}, /* read */
    {20.0, 100}, /* write */
    {2.0, 20},   /* live */
    {5.0, 20},   /* notice */
};

typedef struct LabelClass {
    const char *label;
    RateClass cls;
    RateAnswer ans;
} LabelClass;

/* socket labels (k_routes) that are not gapi / eb; a route missing here is a write with replies,
 * and a label that is no route at all is a notice */
static const LabelClass k_label_class[] = {
    {"a", RC_AUTH, ANS_REPLY}, {"2fa", RC_AUTH, ANS_REPLY}, {"a2FA", RC_AUTH, ANS_REPLY},
    {"a2FAc", RC_AUTH, ANS_REPLY}, {"d2FA", RC_AUTH, ANS_REPLY}, {"d2FAc", RC_AUTH, ANS_REPLY},
    {"cemail", RC_AUTH, ANS_REPLY}, {"emailv", RC_AUTH, ANS_REPLY}, {"emailS", RC_AUTH, ANS_REPLY},
    {"rpr", RC_AUTH, ANS_REPLY}, {"rpc", RC_AUTH, ANS_REPLY}, {"rps", RC_AUTH, ANS_REPLY},
    {"upp", RC_AUTH, ANS_REPLY}, {"las", RC_AUTH, ANS_REPLY},
    {"r", RC_READ, ANS_REPLY}, {"sp", RC_READ, ANS_REPLY}, {"gfs", RC_READ, ANS_REPLY},
    {"gfg", RC_READ, ANS_REPLY}, {"pst", RC_READ, ANS_REPLY}, {"guf", RC_READ, ANS_REPLY},
    {"grp", RC_READ, ANS_REPLY}, {"gps", RC_READ, ANS_REPLY}, {"chtb", RC_READ, ANS_REPLY},
    {"ctd", RC_READ, ANS_REPLY}, {"getS", RC_READ, ANS_REPLY}, {"ttl", RC_READ, ANS_REPLY},
    {"dsp", RC_READ, ANS_REPLY}, {"krh", RC_READ, ANS_REPLY}, {"spinH", RC_READ, ANS_REPLY},
    {"wo", RC_READ, ANS_REPLY},
    {"sst", RC_READ, ANS_REPLY}, {"chg", RC_READ, ANS_REPLY}, {"mdl", RC_READ, ANS_REPLY},
    {"bpdl", RC_READ, ANS_REPLY}, {"ri", RC_READ, ANS_REPLY}, {"ecfg", RC_READ, ANS_REPLY},
    {"st", RC_READ, ANS_REPLY}, {"kml", RC_READ, ANS_REPLY}, {"fil", RC_READ, ANS_REPLY},
    {"uml", RC_READ, ANS_REPLY},
    {"f", RC_WRITE, ANS_REPLY}, {"ps", RC_WRITE, ANS_REPLY}, {"com", RC_WRITE, ANS_REPLY},
    {"lps", RC_WRITE, ANS_REPLY}, {"dps", RC_WRITE, ANS_REPLY}, {"ict", RC_WRITE, ANS_REPLY},
    {"kr", RC_WRITE, ANS_REPLY}, {"cln", RC_WRITE, ANS_REPLY}, {"clnR", RC_WRITE, ANS_REPLY},
    {"clnRA", RC_WRITE, ANS_REPLY}, {"rpt", RC_WRITE, ANS_REPLY}, {"flg", RC_WRITE, ANS_REPLY},
    {"ver", RC_WRITE, ANS_REPLY}, {"crt", RC_WRITE, ANS_REPLY}, {"sbdg", RC_WRITE, ANS_REPLY},
    {"nme", RC_WRITE, ANS_REPLY}, {"uploadS", RC_WRITE, ANS_REPLY}, {"uploadM", RC_WRITE, ANS_REPLY},
    {"uploadMo", RC_WRITE, ANS_REPLY}, {"uploadA", RC_WRITE, ANS_REPLY}, {"edMp", RC_WRITE, ANS_REPLY}, {"ulb", RC_WRITE, ANS_REPLY},
    {"fvM", RC_WRITE, ANS_REPLY}, {"ftC", RC_WRITE, ANS_REPLY}, {"delC", RC_WRITE, ANS_REPLY},
    {"li", RC_WRITE, ANS_REPLY}, {"uli", RC_WRITE, ANS_REPLY}, {"pi", RC_WRITE, ANS_REPLY},
    {"qsi", RC_WRITE, ANS_REPLY}, {"ctr", RC_WRITE, ANS_REPLY}, {"trA", RC_WRITE, ANS_REPLY},
    {"trC", RC_WRITE, ANS_REPLY}, {"trCA", RC_WRITE, ANS_REPLY}, {"srt", RC_WRITE, ANS_REPLY},
    {"pbm", RC_WRITE, ANS_REPLY}, {"spin", RC_WRITE, ANS_REPLY}, {"fkr", RC_WRITE, ANS_REPLY},
    {"clm", RC_WRITE, ANS_REPLY}, {"bdg", RC_WRITE, ANS_REPLY}, {"cbdg", RC_WRITE, ANS_REPLY},
    {"bpclm", RC_WRITE, ANS_REPLY}, {"bpclmAll", RC_WRITE, ANS_REPLY}, {"xstbp", RC_WRITE, ANS_REPLY},
    {"xstbpl", RC_WRITE, ANS_REPLY}, {"dsl", RC_WRITE, ANS_REPLY}, {"wob", RC_WRITE, ANS_REPLY}, {"wor", RC_WRITE, ANS_REPLY}, {"rml", RC_WRITE, ANS_REPLY},
    {"vr", RC_WRITE, ANS_REPLY}, /* v10 REDEEM_VOUCHER (redeem.c): a refusal is answered "vr" with the error */
    {"raml", RC_WRITE, ANS_REPLY},
    {"rqdxp", RC_WRITE, ANS_REPLY}, /* v10 2x XP claim (it doubles the score: a write, answered "rrqdxp") */
    {"wo", RC_READ, ANS_REPLY}, {"wob", RC_WRITE, ANS_REPLY}, {"wor", RC_WRITE, ANS_REPLY}, /* v10 wild offers (wild_offer.c) */
    /* fire-and-forget: no reply, one notice per refusal window */
    {"sfkr", RC_NOTICE, ANS_NOTICE}, {"lfkr", RC_NOTICE, ANS_NOTICE}, {"fkrf", RC_NOTICE, ANS_NOTICE},
    {"unbx", RC_NOTICE, ANS_NOTICE}, {"vcln", RC_NOTICE, ANS_NOTICE},
};

/* ["eb", {type}] by type; an unknown type is a notice */
static const LabelClass k_eb_class[] = {
    {"presence:ping", RC_LIVE, ANS_REPLY},
    {"friends:list", RC_READ, ANS_REPLY},
    {"msg:history", RC_READ, ANS_REPLY},
    {"msg:send", RC_WRITE, ANS_REPLY},
    {"msg:read", RC_WRITE, ANS_NOTICE},
    {"game:invite", RC_WRITE, ANS_REPLY}, /* answered game:invite:sent or an eb error (social_eb.c) */
};

static const LabelClass *find_class(const LabelClass *t, size_t n, const char *key) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(t[i].label, key)) return &t[i];
    return NULL;
}

static int is_route(const char *label) {
    for (size_t i = 0; i < sizeof k_routes / sizeof k_routes[0]; i++)
        if (!strcmp(k_routes[i].label, label)) return 1;
    return 0;
}

/* the label of an inbound frame, NULL unless it is an array whose first element is a string */
static const char *frame_label(const cJSON *frame) {
    return cJSON_IsArray(frame) ? cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0)) : NULL;
}

typedef struct RateDecision {
    RateClass cls;
    RateAnswer ans;
    RateKind kind;
    int64_t retry_ms;   /* when refused: the time until the bucket holds a token */
} RateDecision;

static void classify(const cJSON *frame, RateDecision *d) {
    const char *label = frame_label(frame);
    d->cls = RC_NOTICE;
    d->ans = ANS_NOTICE;
    d->kind = KIND_CLASS;
    d->retry_ms = 0;
    if (!label) {
        d->kind = KIND_MALFORMED;
        return;
    }
    if (!strcmp(label, "po")) {
        d->kind = KIND_EXEMPT;
        return;
    }
    if (!strcmp(label, "gapi")) {
        const char *method = fa_str(frame, 2);
        d->cls = method && !strcasecmp(method, "GET") ? RC_READ : RC_WRITE;
        d->ans = ANS_REPLY;
        return;
    }
    if (!strcmp(label, "eb")) {
        const LabelClass *e = find_class(k_eb_class, sizeof k_eb_class / sizeof k_eb_class[0], kn_json_str(fa(frame, 1), "type", ""));
        if (e) {
            d->cls = e->cls;
            d->ans = e->ans;
        } else {
            d->kind = KIND_UNKNOWN;
        }
        return;
    }
    const LabelClass *l = find_class(k_label_class, sizeof k_label_class / sizeof k_label_class[0], label);
    if (l) {
        d->cls = l->cls;
        d->ans = l->ans;
    } else if (is_route(label)) {
        d->cls = RC_WRITE;
        d->ans = ANS_REPLY;
    } else {
        d->kind = KIND_UNKNOWN;
    }
}

/* 1 = admitted (one token spent, or exempt), 0 = refused with d->retry_ms */
static int rate_admit(SocialSession *s, const cJSON *frame, RateDecision *d) {
    classify(frame, d);
    if (d->kind == KIND_EXEMPT) return 1;
    SocialRateBucket *b = &s->rate[d->cls];
    const RateSpec *r = &k_rate[d->cls];
    const uint64_t now = kn_now_ms();
    if (!b->started) {
        b->started = 1;
        b->tokens = r->burst;
        b->at_ms = now;
    }
    b->tokens += (double) (now - b->at_ms) * r->per_s / 1000.0;
    if (b->tokens > r->burst) b->tokens = r->burst;
    b->at_ms = now;
    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        b->window = 0; /* a passing frame ends the refusal window of its class */
        return 1;
    }
    d->retry_ms = (int64_t) ((1.0 - b->tokens) * 1000.0 / r->per_s) + 1;
    return 0;
}

static void queue_push(SocialSession *s, SocialFrameQ *n) {
    if (s->q_tail) s->q_tail->next = n;
    else s->q_head = n;
    s->q_tail = n;
    s->q_len++;
}

/* The explicit answer to a frame. Refused (retry_ms > 0): "Rate limited, retry in N ms." with the
 * retry time; otherwise "Unknown request." / "Malformed request.". gapi: 429 with its rid; eb: an
 * error with `of` (EB_RATE_LIMITED, or EB_UNKNOWN_TYPE for an eb type that is none); "a": the
 * auth error; anything else: the page's error popup (social_error). */
static void answer(SocialSession *s, const cJSON *frame, RateKind kind, int64_t retry_ms) {
    const char *label = frame_label(frame);
    const int refused = retry_ms > 0;
    char msg[96];
    if (refused) snprintf(msg, sizeof msg, "Rate limited, retry in %lld ms.", (long long) retry_ms);
    else snprintf(msg, sizeof msg, "%s", kind == KIND_MALFORMED ? "Malformed request." : "Unknown request.");
    if (label && !strcmp(label, "gapi")) {
        cJSON *e = cJSON_CreateObject(), *o = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "code", refused ? "RATE_LIMITED" : "UNKNOWN");
        cJSON_AddStringToObject(e, "message", msg);
        if (refused) cJSON_AddNumberToObject(e, "retry_ms", (double) retry_ms);
        cJSON_AddItemToObject(o, "error", e);
        social_send_n(s, "gapi", 3, cJSON_CreateNumber((double) fa_int(frame, 1, 0)), cJSON_CreateNumber(refused ? 429 : 404), o);
    } else if (label && !strcmp(label, "eb")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "error");
        cJSON_AddStringToObject(o, "code", refused ? "EB_RATE_LIMITED" : "EB_UNKNOWN_TYPE");
        cJSON_AddStringToObject(o, "message", msg);
        if (refused) cJSON_AddNumberToObject(o, "retry_ms", (double) retry_ms);
        cJSON_AddStringToObject(o, "of", kn_json_str(fa(frame, 1), "type", "")); /* the client fails its oldest message only for msg:send */
        social_send_n(s, "eb", 1, o);
    } else if (label && !strcmp(label, "a") && refused) {
        send_auth_error(s, msg);
    } else if (label && !strcmp(label, "vr")) { /* v10 REDEEM_VOUCHER_RESPONSE: (funds, error) */
        social_send_n(s, "vr", 2, cJSON_CreateNull(), cJSON_CreateString(msg));
    } else {
        social_error(s, msg);
    }
}

/* Answers a frame in order: now, or queued behind the work in flight so the socket's replies keep
 * their order. Consumes frame. */
static void reply_in_order(SocialSession *s, cJSON *frame, RateKind kind, int64_t retry_ms) {
    if ((s->busy > 0 || s->q_head) && s->q_len < QUEUE_MAX) {
        SocialFrameQ *n = calloc(1, sizeof *n);
        if (n) {
            n->frame = frame;
            n->mode = QM_ANSWER;
            n->kind = kind;
            n->retry_ms = retry_ms;
            queue_push(s, n);
            return;
        }
    }
    answer(s, frame, kind, retry_ms);
    cJSON_Delete(frame);
}

/* A refused frame: a notice-answered class sends its notice once per window, a reply-answered
 * one always gets its reply. Consumes frame. */
static void refuse(SocialSession *s, cJSON *frame, const RateDecision *d) {
    if (d->ans == ANS_NOTICE) {
        if (s->rate[d->cls].window) {
            cJSON_Delete(frame);
            return;
        }
        s->rate[d->cls].window = 1;
    }
    reply_in_order(s, frame, d->kind, d->retry_ms);
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
    /* every frame is decided here: charged to its class, then answered or dispatched (a frame
     * that does not parse has no label and gets the malformed answer) */
    RateDecision d;
    if (!rate_admit(s, frame, &d)) {
        refuse(s, frame, &d); /* consumes frame */
        return;
    }
    if (d.kind == KIND_EXEMPT) { /* "po": the client's answer to our "pi"; nothing to answer */
        cJSON_Delete(frame);
        return;
    }
    if (d.kind != KIND_CLASS) { /* unknown or malformed, within its burst: its explicit answer */
        reply_in_order(s, frame, d.kind, 0);
        return;
    }
    if (s->busy > 0 || s->q_head) {
        /* the previous request is still running: keep the socket's order */
        if (s->q_len < QUEUE_MAX) {
            SocialFrameQ *n = calloc(1, sizeof *n);
            if (n) {
                n->frame = frame;
                n->mode = QM_DISPATCH;
                queue_push(s, n);
                return;
            }
        }
        d.retry_ms = QUEUE_RETRY_MS; /* the queue is full: refused, and answered */
        refuse(s, frame, &d);
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
    presence_leave(s);
    clear_pending(s);
    queue_clear(s);
    if (s->out_timer) {
        kn_loop_cancel_timer(s_loop, s->out_timer);
        s->out_timer = 0;
        social_release(s); /* the flush timer's reference */
    }
    out_clear(s);
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

/* GET /mm/presence: signed-in social sockets here and, with the Redis hot tier, on every brain */
typedef struct PresenceReq {
    kn_http_req *req;
} PresenceReq;

static void presence_reply(kn_http_req *req, int64_t global) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "local", social_ws_online());
    if (global >= 0) cJSON_AddNumberToObject(o, "global", (double) global);
    else cJSON_AddNullToObject(o, "global");
    cJSON_AddStringToObject(o, "redis", hot_state());
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

static void presence_counted(void *ud, int64_t count) {
    PresenceReq *p = ud;
    presence_reply(p->req, count);
    free(p);
}

static void h_presence(kn_http_req *req, void *ud) {
    (void) ud;
    PresenceReq *p = calloc(1, sizeof *p);
    if (p) {
        p->req = req;
        if (hot_presence_count(presence_counted, p) == 0) return;
        free(p);
    }
    presence_reply(req, -1);
}

int social_ws_init(Brain *b) {
    s_loop = b->loop;
    if (!b->http) return 0;
    kn_http_route(b->http, "GET", "/api/ws", ws_route, NULL);
    kn_http_route(b->http, "GET", "/social/ws", ws_route, NULL);
    kn_http_route(b->http, "GET", "/mm/presence", h_presence, NULL);
    hot_presence_source(presence_ids);
    hot_push_handler(push_local);
    social_gapi_init(b); /* v10 Social hub: giveaway draws (social_gapi.c) */
    KN_INFO(MOD, "social socket on /api/ws and /social/ws%s", b->db ? "" : " (no database: guests only)");
    return 0;
}

void social_ws_shutdown(Brain *b) {
    (void) b;
    /* knsvc closes every connection when the HTTP server is freed (on_close runs). */
}
