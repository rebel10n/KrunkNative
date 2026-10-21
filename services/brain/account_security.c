/*
 * services/brain/account_security.c - account security on the social socket (doc/backend.md 7.9):
 *
 *  - Krunker v7.1.4 2FA: an authenticator app (TOTP, 6 digits), index.js act2FA / dact2FA /
 *    sub2FA / twoFAres. ["a2FA"] -> ["c2fa", 0, "a2FAc", otpauthUri] (the popup with the QR code);
 *    ["a2FAc", code] -> ["c2fa", 2, null, null, true]. ["d2FA"] -> ["c2fa", 0, "d2FAc"];
 *    ["d2FAc", code] -> ["c2fa", 2, null, null, false]. A wrong code asks again with the reason as
 *    a 6th element (KrunkNative: v7 shows nothing): ["c2fa", 0, label, uri|null, null, "Invalid Code"].
 *    The login step ("2fa") is in social_ws.c / accounts.c.
 *  - KrunkNative email verification (v7.1.4 has none; its packets.json reserves "cemail" and
 *    "emailv"): ["cemail", email] sets the address (unverified) and mails a verification link;
 *    ["emailv"] mails it again (rate limited); ["emailS"] -> ["emailS", state]. Replies
 *    ["cemail" | "emailv", error | null, message | null, state]. The link is
 *    GET <accounts.email.link_base>/auth/verify-email?token=... (an HTML page); verifying pushes
 *    ["emailS", state] to the account's sockets. accounts.email.require_verified gates social
 *    features behind a verified address (v7.1.4 gates nothing: default none).
 *  - KrunkNative password reset ("Forgot password?"): ["rpr"] mails a one-time code to the
 *    account's verified email, ["rpc"] checks it, ["rps"] sets the new password (below), or the
 *    mailed link's page GET/POST /auth/reset-password.
 *  - Outbound mail through knsvc/smtp.h's kn_mailer (config "smtp"): queued on a worker thread,
 *    every mail in mail_log (status, Message-ID, the server's error; never credentials or tokens).
 */
#include "acct_internal.h"
#include "brain_internal.h" /* mm_settings()->public_url */
#include "hot.h"             /* hot_session_forget_account */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "knsvc/buf.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/smtp.h"

#define MOD "mail"

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#define DEFAULT_FROM "Krunker Box Office <noreply@caan.edu>"

/* accounts.email.require_verified feature names -> bits */
static const char *const k_features[] = {"chat", "post", "gift", "market", "trade", "clan", "publish"};
enum { F_CHAT = 1, F_POST = 2, F_GIFT = 4, F_MARKET = 8, F_TRADE = 16, F_CLAN = 32, F_PUBLISH = 64 };

typedef struct SecConfig {
    int verify_ttl_s;
    int resend_cooldown_s;    /* config.accValidationTime (60000 ms), unused by v7.1.4 itself */
    int max_per_account_day;
    int max_per_address_day;
    int max_per_ip_hour;
    int required;             /* registering needs an email */
    unsigned gate;            /* F_* features that need a verified email */
    int reset_ttl_s;          /* password reset code lifetime (accounts.email.reset_ttl_min, 30) */
    char link_base[256];
    char from[256];
    char reply_to[256];
    char issuer[64];
} SecConfig;

static SecConfig s_cfg;
static kn_mailer *s_mailer;

/* ------------------------------------------------------------------ config + mailer */

static void load_config(Brain *b) {
    const kn_config *cfg = b->cfg;
    SecConfig *c = &s_cfg;
    memset(c, 0, sizeof *c);
    c->verify_ttl_s = (int) kn_config_int(cfg, "accounts.email.verify_ttl_h", 24) * 3600;
    if (c->verify_ttl_s <= 0) c->verify_ttl_s = 24 * 3600;
    c->reset_ttl_s = (int) kn_config_int(cfg, "accounts.email.reset_ttl_min", 30) * 60;
    if (c->reset_ttl_s <= 0) c->reset_ttl_s = 30 * 60;
    c->resend_cooldown_s = (int) kn_config_int(cfg, "accounts.email.resend_cooldown_s", 60);
    c->max_per_account_day = (int) kn_config_int(cfg, "accounts.email.max_per_account_day", 5);
    c->max_per_address_day = (int) kn_config_int(cfg, "accounts.email.max_per_address_day", 5);
    c->max_per_ip_hour = (int) kn_config_int(cfg, "accounts.email.max_per_ip_hour", 10);
    c->required = kn_config_bool(cfg, "accounts.email.required", 0);
    const char *names[16];
    const size_t nn = kn_config_str_list(cfg, "accounts.email.require_verified", names, 16);
    for (size_t i = 0; i < nn && i < 16; i++) {
        for (size_t f = 0; f < sizeof k_features / sizeof k_features[0]; f++)
            if (names[i] && !strcasecmp(names[i], k_features[f])) c->gate |= 1u << f;
    }
    const char *pub = mm_settings() ? mm_settings()->public_url : "";
    const char *link = kn_config_str(cfg, "accounts.email.link_base", "");
    snprintf(c->link_base, sizeof c->link_base, "%s", link && *link ? link : pub); /* default: brain.public_url */
    for (size_t n = strlen(c->link_base); n > 0 && c->link_base[n - 1] == '/'; n--) c->link_base[n - 1] = 0;
    snprintf(c->from, sizeof c->from, "%s", kn_config_str(cfg, "smtp.from", DEFAULT_FROM));
    snprintf(c->reply_to, sizeof c->reply_to, "%s", kn_config_str(cfg, "smtp.reply_to", ""));
    snprintf(c->issuer, sizeof c->issuer, "%s", kn_config_str(cfg, "accounts.twofa_issuer", "Krunker Box Office"));
}

static void start_mailer(Brain *b) {
    const kn_config *cfg = b->cfg;
    const char *host = kn_config_str(cfg, "smtp.host", "");
    if (!host || !*host || !kn_config_bool(cfg, "smtp.enabled", 1)) {
        KN_INFO(MOD, "no smtp.host: email verification mails are off");
        return;
    }
    const int port = (int) kn_config_int(cfg, "smtp.port", 0);
    const int tls = kn_smtp_tls_parse(kn_config_str(cfg, "smtp.tls", "auto"), port);
    char from[256];
    if (tls < 0) {
        KN_ERROR(MOD, "smtp.tls must be none, starttls, implicit or auto: mails are off");
        return;
    }
    if (kn_mail_address(s_cfg.from, from, sizeof from) != 0) {
        KN_ERROR(MOD, "smtp.from is not a valid address: mails are off");
        return;
    }
    kn_smtp_config sc = {
        .host = host,
        .port = port,
        .tls = (kn_smtp_tls) tls,
        .user = kn_config_str(cfg, "smtp.user", NULL),
        .password = kn_config_str(cfg, "smtp.password", NULL),
        .auth = kn_config_str(cfg, "smtp.auth", "auto"),
        .helo = kn_config_str(cfg, "smtp.helo", NULL),
        .timeout_ms = (int) kn_config_int(cfg, "smtp.timeout_ms", 20000),
        .verify_peer = kn_config_bool(cfg, "smtp.verify_peer", 1),
        .ca_file = kn_config_str(cfg, "smtp.ca_file", NULL),
        .allow_plain_auth = kn_config_bool(cfg, "smtp.allow_plain_auth", 0),
    };
    kn_mailer_options opts = {.queue_max = (int) kn_config_int(cfg, "smtp.queue_max", 256),
                              .retries = (int) kn_config_int(cfg, "smtp.retries", 2),
                              .retry_delay_ms = (int) kn_config_int(cfg, "smtp.retry_delay_ms", 5000)};
    s_mailer = kn_mailer_new(b->loop, &sc, &opts);
    if (!s_mailer) {
        KN_ERROR(MOD, "cannot start the mailer");
        return;
    }
    /* host and mode only: never the user or the password */
    KN_INFO(MOD, "smtp %s:%d (%s%s), from %s, links %s/auth/verify-email", host, port ? port : tls == KN_SMTP_TLS_IMPLICIT ? 465 : 587,
            kn_smtp_tls_name((kn_smtp_tls) tls), sc.verify_peer ? "" : ", certificate NOT verified", from, s_cfg.link_base);
}

int acct_email_enabled(void) { return s_mailer != NULL; }

/* ------------------------------------------------------------------ helpers */

/* a***@example.com for logs */
static void mask_addr(const char *addr, char *out, size_t cap) {
    const char *at = addr ? strchr(addr, '@') : NULL;
    if (!at) {
        snprintf(out, cap, "?");
        return;
    }
    snprintf(out, cap, "%c***%s", addr[0], at);
}

const char *acct_validate_email(const char *email, char *norm, size_t cap) {
    if (!email) return "Invalid Email";
    while (*email == ' ') email++;
    size_t n = strlen(email);
    while (n > 0 && email[n - 1] == ' ') n--;
    if (n == 0) return "Invalid Email";
    if (n >= cap || n > 254) return "Email is too long";
    for (size_t i = 0; i < n; i++) norm[i] = (char) tolower((unsigned char) email[i]);
    norm[n] = 0;
    return kn_mail_address_valid(norm) ? NULL : "Invalid Email";
}

static void html_escape(kn_buf *b, const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
        case '&': kn_buf_append_str(b, "&amp;"); break;
        case '<': kn_buf_append_str(b, "&lt;"); break;
        case '>': kn_buf_append_str(b, "&gt;"); break;
        case '"': kn_buf_append_str(b, "&quot;"); break;
        case '\'': kn_buf_append_str(b, "&#39;"); break;
        default: kn_buf_append_byte(b, (uint8_t) *s);
        }
    }
}

/* The client-facing copy of a state object (no internal fields). */
static cJSON *client_state(const cJSON *st) {
    cJSON *o = cJSON_CreateObject();
    const char *keys[] = {"email", "verified", "pending", "wait"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(st, keys[i]);
        cJSON_AddItemToObject(o, keys[i], v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull());
    }
    cJSON_AddNumberToObject(o, "mail", s_mailer != NULL);
    return o;
}

/* ------------------------------------------------------------------ verification mail */

typedef struct MailCtx {
    int64_t log_id;
    char masked[64];
    const char *kind;          /* "verify" / "reset" (logs) */
} MailCtx;

static void log_update_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "mail_log update: %s", kn_db_error(res));
}

static void mail_done(void *ud, int status, const char *message_id, const char *error) {
    MailCtx *m = ud;
    char id[24];
    const char *kind = m->kind ? m->kind : "verify";
    if (status == KN_SMTP_OK) KN_INFO(MOD, "%s mail #%lld to %s sent %s", kind, (long long) m->log_id, m->masked, message_id ? message_id : "");
    else KN_WARN(MOD, "%s mail #%lld to %s failed: %s", kind, (long long) m->log_id, m->masked, error ? error : "?");
    if (g_acct_brain && g_acct_brain->db && m->log_id > 0)
        kn_db_query_sql(g_acct_brain->db,
                        SQL(.any = "UPDATE mail_log SET status = $2, message_id = $3, error = $4, sent_at = {now} WHERE id = {int:$1}"),
                        KN_DB_ARGS(acct_itoa(id, m->log_id), status == KN_SMTP_OK ? "sent" : "failed", message_id,
                                   status == KN_SMTP_OK ? NULL : error),
                        log_update_done, NULL);
    free(m);
}

static int send_verify_mail(int64_t log_id, const char *username, const char *to, const char *token) {
    if (!s_mailer) return -1;
    char link[512];
    snprintf(link, sizeof link, "%s/auth/verify-email?token=%s", s_cfg.link_base, token);
    const int hours = s_cfg.verify_ttl_s / 3600;
    kn_buf text, html;
    kn_buf_init(&text);
    kn_buf_init(&html);
    kn_buf_appendf(&text,
                   "Hi %s,\n\n"
                   "Please confirm that this is the email address of your Krunker Box Office account by opening this link:\n\n"
                   "%s\n\n"
                   "The link expires in %d hours. If you did not add this address to a Krunker Box Office account, you can ignore this email.\n\n"
                   "Krunker Box Office\n",
                   username, link, hours);
    kn_buf_append_str(&html, "<!DOCTYPE html><html><body style=\"margin:0;padding:24px;background:#f2f2f2;font-family:Arial,sans-serif\">"
                             "<div style=\"max-width:520px;margin:0 auto;background:#fff;border-radius:6px;padding:24px;color:#333\">"
                             "<h2 style=\"margin:0 0 16px;color:#222\">Verify your email</h2><p>Hi ");
    html_escape(&html, username);
    kn_buf_append_str(&html, ",</p><p>Please confirm that this is the email address of your Krunker Box Office account.</p>"
                             "<p style=\"text-align:center;margin:28px 0\"><a href=\"");
    html_escape(&html, link);
    kn_buf_append_str(&html, "\" style=\"background:#fc0;color:#000;text-decoration:none;font-weight:bold;padding:12px 28px;"
                             "border-radius:4px;display:inline-block\">Verify Email</a></p><p style=\"font-size:13px;color:#777\">"
                             "Or open this link: ");
    html_escape(&html, link);
    kn_buf_appendf(&html, "</p><p style=\"font-size:13px;color:#777\">The link expires in %d hours. If you did not add this address "
                          "to a Krunker Box Office account, you can ignore this email.</p></div></body></html>",
                   hours);
    kn_mail m = {.from = s_cfg.from, .to = to, .reply_to = s_cfg.reply_to[0] ? s_cfg.reply_to : NULL,
                 .subject = "Verify your Krunker Box Office email", .text = text.data, .html = html.data};
    MailCtx *ctx = calloc(1, sizeof *ctx);
    int rc = -1;
    if (ctx) {
        ctx->log_id = log_id;
        ctx->kind = "verify";
        mask_addr(to, ctx->masked, sizeof ctx->masked);
        rc = kn_mailer_send(s_mailer, &m, mail_done, ctx);
        if (rc != 0) {
            KN_WARN(MOD, "mail queue full: verify mail #%lld to %s dropped", (long long) log_id, ctx->masked);
            char id[24];
            kn_db_query_sql(g_acct_brain->db, SQL(.any = "UPDATE mail_log SET status = 'failed', error = 'queue full' WHERE id = {int:$1}"),
                            KN_DB_ARGS(acct_itoa(id, log_id)), log_update_done, NULL);
            free(ctx);
        }
    }
    kn_buf_free(&text);
    kn_buf_free(&html);
    return rc;
}

/* ------------------------------------------------------------------ email requests */

typedef struct EmailReq {
    SocialSession *s;         /* held while the request runs; NULL = no reply (register) */
    int64_t account_id;
    char label[8];            /* reply label: "cemail" / "emailv" */
    char token[64];
    AcctEmailFn done;         /* the HTTP route's answer (acct_email_request_http); NULL on the socket */
    void *ud;
} EmailReq;

static void email_req_done(kn_db_result *res, void *ud) {
    EmailReq *r = ud;
    cJSON *st = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const char *err = kn_db_ok(res) ? (st ? NULL : "Server error.") : acct_db_error(res, "email");
    const char *msg = NULL;
    char sent_msg[320];
    if (st) {
        const char *limited = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st, "limited"));
        const char *to = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st, "email"));
        const char *user = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(st, "username"));
        if (kn_json_int(st, "send", 0) && to) {
            if (send_verify_mail(kn_json_int(st, "logId", 0), user ? user : "", to, r->token) == 0) {
                snprintf(sent_msg, sizeof sent_msg, "Verification email sent to %s", to);
                msg = sent_msg;
            } else {
                err = "Email is unavailable on this server";
            }
        } else if (limited) {
            err = limited;
        }
    }
    memset(r->token, 0, sizeof r->token);
    cJSON *cs = st ? client_state(st) : NULL;
    if (r->done) {
        r->done(r->ud, err, msg);
    } else if (r->s && social_open(r->s)) {
        cJSON *f = social_frame(r->label);
        cJSON_AddItemToArray(f, err ? cJSON_CreateString(err) : cJSON_CreateNull());
        cJSON_AddItemToArray(f, msg ? cJSON_CreateString(msg) : cJSON_CreateNull());
        cJSON_AddItemToArray(f, cs ? cJSON_Duplicate(cs, 1) : cJSON_CreateNull());
        social_send(r->s, f);
    }
    if (cs) social_email_state_push(r->account_id, cs, r->s); /* the account's other sockets */
    cJSON_Delete(cs);
    cJSON_Delete(st);
    if (r->s) social_unhold(r->s);
    free(r);
}

/* A refused request: the HTTP route's done, or the socket's error reply (label). */
static void email_refused(SocialSession *s, const char *label, AcctEmailFn done, void *ud, const char *err) {
    if (done) done(ud, err, NULL);
    else if (s) social_send_n(s, label, 1, cJSON_CreateString(err));
}

/* email NULL = resend to the stored address. s (optional) gets the reply; done (optional) the HTTP route's. */
static void email_request(SocialSession *s, int64_t account_id, const char *email, const char *ip, const char *label,
                          AcctEmailFn done, void *ud) {
    EmailReq *r = calloc(1, sizeof *r);
    if (!r) {
        if (done) done(ud, "Server error.", NULL);
        return;
    }
    r->account_id = account_id;
    snprintf(r->label, sizeof r->label, "%s", label);
    r->done = done;
    r->ud = ud;
    if (kn_random_token(r->token, sizeof r->token, 32) != 0) {
        free(r);
        email_refused(s, label, done, ud, "Server error.");
        return;
    }
    r->s = s ? social_hold(s) : NULL;
    char id[24], ttl[24], cool[24], d1[24], d2[24], d3[24], now[24];
    const char *args[] = {acct_itoa(id, account_id), email, ip && *ip ? ip : NULL, r->token,
                          acct_itoa(ttl, s_cfg.verify_ttl_s), acct_itoa(cool, s_cfg.resend_cooldown_s),
                          acct_itoa(d1, s_cfg.max_per_account_day), acct_itoa(d2, s_cfg.max_per_address_day),
                          acct_itoa(d3, s_cfg.max_per_ip_hour), s_mailer ? "1" : "0",
                          acct_itoa(now, now_ms())};
    if (kn_db_call(g_acct_brain->db, &P_EMAIL_REQUEST, (int) (sizeof args / sizeof args[0]), args, email_req_done, r) != 0) {
        /* a refused submit calls back on no path: answer the refusal here */
        if (r->s) social_unhold(r->s);
        memset(r->token, 0, sizeof r->token);
        free(r);
        email_refused(s, label, done, ud, "Server error.");
    }
}

void acct_email_after_register(int64_t account_id, const char *email, const char *ip) {
    if (account_id > 0 && email && *email && g_acct_brain && g_acct_brain->db)
        email_request(NULL, account_id, email, ip, "cemail", NULL, NULL);
}

/* v10 POST /auth/verification/resend and change-email (v10_auth.c): the socket's "emailv" / "cemail" without a
 * socket. email NULL = resend to the stored address; otherwise the address (validated by the caller) is stored
 * unverified and mailed. done(ud, err, msg) runs once from the DB reply: err is the user-facing text. */
void acct_email_request_http(int64_t account_id, const char *email, const char *ip, AcctEmailFn done, void *ud) {
    email_request(NULL, account_id, email, ip, email ? "cemail" : "emailv", done, ud);
}

/* ["cemail", email] */
void sh_email_set(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    char norm[256];
    const char *err = acct_validate_email(fa_str(f, 1), norm, sizeof norm);
    if (err) {
        social_send_n(s, "cemail", 1, cJSON_CreateString(err));
        return;
    }
    email_request(s, s->account_id, norm, s->ip, "cemail", NULL, NULL);
}

/* ["emailv"] */
void sh_email_resend(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    email_request(s, s->account_id, NULL, s->ip, "emailv", NULL, NULL);
}

static void email_state_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *st = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!st) return;
    s->email_verified = (int) kn_json_int(st, "verified", 0);
    s->email_state_known = 1;
    if (s->json) social_send_n(s, "emailS", 1, client_state(st));
    cJSON_Delete(st);
}

/* ["emailS"] -> ["emailS", state]; also right after every login */
void sh_email_state(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) return;
    char id[24], cool[24], now[24];
    social_call(s, &P_EMAIL_STATE,
                KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(cool, s_cfg.resend_cooldown_s),
                           acct_itoa(now, now_ms())),
                email_state_done, NULL);
}

/* ------------------------------------------------------------------ feature gates */

/* Feature bit of a frame that needs a verified email when gated (0 = none). */
static unsigned frame_feature(const char *label, const cJSON *f) {
    if (!strcmp(label, "ict")) return F_CHAT;
    if (!strcmp(label, "ps") || !strcmp(label, "com")) return F_POST;
    if (!strcmp(label, "kr")) return F_GIFT;
    if (!strcmp(label, "li") || !strcmp(label, "pi")) return F_MARKET;
    if (!strcmp(label, "ctr") || !strcmp(label, "trA")) return F_TRADE;
    if (!strcmp(label, "cln") && cJSON_IsNumber(fa(f, 1)) && fa_int(f, 1, 0) == 1) return F_CLAN; /* create */
    if (!strcmp(label, "uploadM")) return F_PUBLISH;
    return 0;
}

int social_email_gate(SocialSession *s, const char *label, const cJSON *f) {
    if (!s_cfg.gate || s->account_id <= 0 || s->email_verified) return 1;
    const unsigned feat = frame_feature(label, f);
    if (!(feat & s_cfg.gate)) return 1;
    social_error(s, "Verify your email to use this feature");
    return 0;
}

int acct_email_required(void) { return s_cfg.required; }

/* ------------------------------------------------------------------ verify link */

static void verify_page(kn_http_req *req, int status, const char *title, const char *text) {
    kn_buf b;
    kn_buf_init(&b);
    kn_buf_append_str(&b, "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Krunker Box Office - ");
    html_escape(&b, title);
    kn_buf_append_str(&b, "</title><style>body{margin:0;background:#1d1d1d;color:#fff;font-family:Arial,sans-serif;text-align:center}"
                          "div{max-width:560px;margin:15vh auto 0;padding:32px;background:rgba(0,0,0,.35);border-radius:6px}"
                          "h1{color:#fc0;margin:0 0 16px}p{color:rgba(255,255,255,.75);font-size:18px}</style></head><body><div><h1>");
    html_escape(&b, title);
    kn_buf_append_str(&b, "</h1><p>");
    html_escape(&b, text);
    kn_buf_append_str(&b, "</p></div></body></html>");
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_set_header(req, "Referrer-Policy", "no-referrer");
    kn_http_set_header(req, "X-Content-Type-Options", "nosniff");
    kn_http_respond(req, status, "text/html; charset=utf-8", b.data, b.len);
    kn_buf_free(&b);
}

static void verify_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) {
        const char *msg = kn_db_user_message(res);
        verify_page(req, 400, "Email not verified", msg && *msg ? msg : (KN_WARN(MOD, "verify: %s", kn_db_error(res)), "Server error."));
        return;
    }
    char masked[64];
    const char *email = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "email"));
    mask_addr(email, masked, sizeof masked);
    const int64_t id = kn_json_int(o, "id", 0);
    KN_INFO(MOD, "account %lld verified %s", (long long) id, masked);
    char text[400];
    snprintf(text, sizeof text, "%s is now the verified email of your Krunker Box Office account %s. You can close this page.",
             email ? email : "Your email", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "username")));
    verify_page(req, 200, "Email verified", text);
    cJSON *st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "email", email ? email : "");
    cJSON_AddNumberToObject(st, "verified", 1);
    cJSON_AddNumberToObject(st, "pending", 0);
    cJSON_AddNumberToObject(st, "wait", 0);
    cJSON_AddNumberToObject(st, "mail", s_mailer != NULL);
    social_email_state_push(id, st, NULL);
    cJSON_Delete(st);
    cJSON_Delete(o);
}

static void h_verify(kn_http_req *req, void *ud) {
    (void) ud;
    char token[160];
    if (kn_http_query_get(req, "token", token, sizeof token) <= 0 || !g_acct_brain || !g_acct_brain->db) {
        verify_page(req, 400, "Email not verified", "This link is invalid.");
        return;
    }
    kn_db_call(g_acct_brain->db, &P_EMAIL_VERIFY, KN_DB_ARGS(token), verify_done, req);
}

/* ------------------------------------------------------------------ password reset (KrunkNative) */

/* "Forgot password": v7.1.4 only says "For lost Passwords/Accounts contact recovery@yendis.ch".
 * KrunkNative mails a reset code to the account's verified email:
 *   ["rpr", name or email]        -> ["rpr", null, msg]  (the same text whether or not an account
 *                                    matched: no account / address enumeration) | ["rpr", err]
 *   ["rpc", code]                 -> ["rpc", null, username] | ["rpc", err]   (check before asking
 *                                    for the new password)
 *   ["rps", code, new, repeat]    -> ["rps", null, msg] | ["rps", err]; every session of the
 *                                    account is revoked, the client logs in with the new password.
 * The mail carries the code (typed into the client) and a link to <link_base>/auth/reset-password
 * (a small page with the same form, for players who read their mail elsewhere). */

#define RESET_SENT_MSG "If that account has a verified email, a reset code is on its way. Check your inbox."
#define RESET_CODE_CHARS 12

/* 12 random base32 characters (60 bits) as XXXX-XXXX-XXXX; the stored hash is of the bare 12. */
static int reset_code_new(char bare[RESET_CODE_CHARS + 1], char shown[RESET_CODE_CHARS + 3]) {
    uint8_t raw[8];
    char b32[32];
    if (kn_random_bytes(raw, sizeof raw) != 0 || kn_base32_encode(raw, sizeof raw, b32, sizeof b32) < RESET_CODE_CHARS) return -1;
    memcpy(bare, b32, RESET_CODE_CHARS);
    bare[RESET_CODE_CHARS] = 0;
    snprintf(shown, RESET_CODE_CHARS + 3, "%.4s-%.4s-%.4s", bare, bare + 4, bare + 8);
    memset(raw, 0, sizeof raw);
    memset(b32, 0, sizeof b32);
    return 0;
}

/* A typed or linked code -> the bare upper-case form (dashes / spaces dropped, 0/1/8 read as
 * O/I/B, which base32 lacks). 0 when it cannot be a code. */
static int reset_code_norm(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; in && *in && n + 1 < cap; in++) {
        int ch = toupper((unsigned char) *in);
        if (ch == '-' || ch == ' ') continue;
        if (ch == '0') ch = 'O';
        else if (ch == '1') ch = 'I';
        else if (ch == '8') ch = 'B';
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '7'))) return 0;
        out[n++] = (char) ch;
    }
    out[n] = 0;
    return n == RESET_CODE_CHARS;
}

static int send_reset_mail(int64_t log_id, const char *to, const cJSON *accounts) {
    if (!s_mailer) return -1;
    const int minutes = s_cfg.reset_ttl_s / 60;
    kn_buf text, html;
    kn_buf_init(&text);
    kn_buf_init(&html);
    kn_buf_append_str(&text, "Someone asked to reset the password of the Krunker Box Office account");
    kn_buf_append_str(&text, cJSON_GetArraySize(accounts) > 1 ? "s" : "");
    kn_buf_append_str(&text, " that use this email address.\n\n");
    kn_buf_append_str(&html, "<!DOCTYPE html><html><body style=\"margin:0;padding:24px;background:#f2f2f2;font-family:Arial,sans-serif\">"
                             "<div style=\"max-width:520px;margin:0 auto;background:#fff;border-radius:6px;padding:24px;color:#333\">"
                             "<h2 style=\"margin:0 0 16px;color:#222\">Reset your password</h2><p>Someone asked to reset the password of "
                             "your Krunker Box Office account. Enter this code in the game's Forgot Password window:</p>");
    const cJSON *a;
    cJSON_ArrayForEach(a, accounts) {
        const char *user = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "username"));
        const char *bare = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "code"));
        if (!user || !bare) continue;
        char shown[RESET_CODE_CHARS + 3], link[512];
        snprintf(shown, sizeof shown, "%.4s-%.4s-%.4s", bare, bare + 4, bare + 8);
        snprintf(link, sizeof link, "%s/auth/reset-password?token=%s", s_cfg.link_base, bare);
        kn_buf_appendf(&text, "Account %s: code %s\nOr open: %s\n\n", user, shown, link);
        kn_buf_append_str(&html, "<p style=\"margin:20px 0 4px\">Account <b>");
        html_escape(&html, user);
        kn_buf_append_str(&html, "</b></p><p style=\"text-align:center;font-size:28px;letter-spacing:4px;font-weight:bold;"
                                 "background:#f6f6f6;padding:12px;border-radius:4px;margin:0\">");
        html_escape(&html, shown);
        kn_buf_append_str(&html, "</p><p style=\"font-size:13px;color:#777\">Or open <a href=\"");
        html_escape(&html, link);
        kn_buf_append_str(&html, "\">this link</a> to choose a new password in the browser.</p>");
    }
    kn_buf_appendf(&text, "The code expires in %d minutes and works once. If you did not ask for this, ignore this email: "
                          "your password stays the same.\n\nKrunker Box Office\n", minutes);
    kn_buf_appendf(&html, "<p style=\"font-size:13px;color:#777\">The code expires in %d minutes and works once. If you did not "
                          "ask for this, ignore this email: your password stays the same.</p></div></body></html>", minutes);
    kn_mail m = {.from = s_cfg.from, .to = to, .reply_to = s_cfg.reply_to[0] ? s_cfg.reply_to : NULL,
                 .subject = "Reset your Krunker Box Office password", .text = text.data, .html = html.data};
    MailCtx *ctx = calloc(1, sizeof *ctx);
    int rc = -1;
    if (ctx) {
        ctx->log_id = log_id;
        ctx->kind = "reset";
        mask_addr(to, ctx->masked, sizeof ctx->masked);
        rc = kn_mailer_send(s_mailer, &m, mail_done, ctx);
        if (rc != 0) {
            KN_WARN(MOD, "mail queue full: reset mail #%lld to %s dropped", (long long) log_id, ctx->masked);
            free(ctx);
        }
    }
    /* the bodies held the codes */
    if (text.data) memset(text.data, 0, text.len);
    if (html.data) memset(html.data, 0, html.len);
    kn_buf_free(&text);
    kn_buf_free(&html);
    return rc;
}
typedef struct ResetReq {
    SocialSession *s;          /* held; NULL for the web page and the HTTP route */
    kn_http_req *req;          /* the web page's POST, NULL on the socket */
    AcctRecoverFn done;        /* the HTTP route's answer (v10 POST /auth/recover-account); NULL on the socket */
    void *ud;
    char code[RESET_CODE_CHARS + 1];
    char token[40];            /* the session issued by P_RESET_APPLY */
    char *pass;
    char *codes[3];
} ResetReq;

static void reset_free(ResetReq *r) {
    if (!r) return;
    if (r->pass) {
        memset(r->pass, 0, strlen(r->pass));
        free(r->pass);
    }
    for (int i = 0; i < 3; i++) {
        if (r->codes[i]) memset(r->codes[i], 0, strlen(r->codes[i]));
        free(r->codes[i]);
    }
    memset(r->code, 0, sizeof r->code);
    memset(r->token, 0, sizeof r->token);
    if (r->s) social_unhold(r->s);
    free(r);
}

static void reset_reply(ResetReq *r, const char *label, const char *err, const char *msg) {
    if (r->done) {
        r->done(r->ud, err, err ? NULL : msg ? msg : "");
        return;
    }
    if (r->s && social_open(r->s)) {
        if (err) social_send_n(r->s, label, 1, cJSON_CreateString(err));
        else social_send_n(r->s, label, 2, cJSON_CreateNull(), cJSON_CreateString(msg ? msg : ""));
    }
}

static void reset_request_done(kn_db_result *res, void *ud) {
    ResetReq *r = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) {
        reset_reply(r, "rpr", acct_db_error(res, "reset"), NULL);
    } else if (kn_json_int(o, "ipLimited", 0)) {
        reset_reply(r, "rpr", "Too many emails. Please Try Later", NULL);
    } else {
        const cJSON *accounts = cJSON_GetObjectItemCaseSensitive(o, "accounts");
        const char *to = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "to"));
        if (kn_json_int(o, "send", 0) && to && send_reset_mail(kn_json_int(o, "logId", 0), to, accounts) != 0)
            KN_WARN(MOD, "reset mail #%lld not queued", (long long) kn_json_int(o, "logId", 0));
        reset_reply(r, "rpr", NULL, RESET_SENT_MSG);
        /* the codes are in o: wipe them before freeing */
        const cJSON *a;
        cJSON_ArrayForEach(a, accounts) {
            cJSON *c = cJSON_GetObjectItemCaseSensitive(a, "code");
            if (cJSON_IsString(c)) memset(c->valuestring, 0, strlen(c->valuestring));
        }
    }
    cJSON_Delete(o);
    reset_free(r);
}

/* A refused ["rpr"] (the socket's) or a refused recover-account (the HTTP route's answer). */
static void reset_refuse(SocialSession *s, AcctRecoverFn done, void *ud, const char *err) {
    if (done) done(ud, err, NULL);
    else social_send_n(s, "rpr", 1, cJSON_CreateString(err));
}

/* ["rpr", username or email] (s) and POST /auth/recover-account (s NULL, done set): the same mail */
static void reset_request_start(SocialSession *s, const char *who, const char *ip, AcctRecoverFn done, void *ud) {
    char norm[256];
    size_t n = 0;
    while (who && *who == ' ') who++;
    for (; who && who[n] && n + 1 < sizeof norm; n++) norm[n] = who[n];
    while (n > 0 && norm[n - 1] == ' ') n--;
    norm[n] = 0;
    if (!norm[0]) {
        reset_refuse(s, done, ud, "Enter your username or email");
        return;
    }
    if (strchr(norm, '@') && acct_validate_email(norm, norm, sizeof norm)) {
        reset_refuse(s, done, ud, "Invalid Email");
        return;
    }
    if (!s_mailer) {
        reset_refuse(s, done, ud, "Email is unavailable on this server");
        return;
    }
    ResetReq *r = calloc(1, sizeof *r);
    if (!r) return;
    for (int i = 0; i < 3; i++) {
        char shown[RESET_CODE_CHARS + 3];
        r->codes[i] = malloc(RESET_CODE_CHARS + 1);
        if (!r->codes[i] || reset_code_new(r->codes[i], shown) != 0) {
            reset_free(r);
            reset_refuse(s, done, ud, "Server error.");
            return;
        }
    }
    r->s = s ? social_hold(s) : NULL;
    r->done = done;
    r->ud = ud;
    char ttl[24], cool[24], d1[24], d2[24], d3[24], now[24];
    const char *args[] = {norm, ip && ip[0] ? ip : NULL, r->codes[0], r->codes[1], r->codes[2],
                          acct_itoa(ttl, s_cfg.reset_ttl_s), acct_itoa(cool, s_cfg.resend_cooldown_s),
                          acct_itoa(d1, s_cfg.max_per_account_day), acct_itoa(d2, s_cfg.max_per_address_day),
                          acct_itoa(d3, s_cfg.max_per_ip_hour), "1", acct_itoa(now, now_ms())};
    kn_db_call(g_acct_brain->db, &P_RESET_REQUEST, (int) (sizeof args / sizeof args[0]), args, reset_request_done, r);
}

/* ["rpr", username or email] */
void sh_reset_request(SocialSession *s, const cJSON *f) { reset_request_start(s, fa_str(f, 1), s->ip, NULL, NULL); }

void acct_recover_request(const char *who, const char *ip, AcctRecoverFn done, void *ud) {
    reset_request_start(NULL, who, ip, done, ud);
}

static void reset_check_done(kn_db_result *res, void *ud) {
    ResetReq *r = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) reset_reply(r, "rpc", acct_db_error(res, "reset"), NULL);
    else reset_reply(r, "rpc", NULL, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "username")));
    cJSON_Delete(o);
    reset_free(r);
}

/* ["rpc", code] */
void sh_reset_check(SocialSession *s, const cJSON *f) {
    char code[RESET_CODE_CHARS + 1];
    if (!reset_code_norm(fa_str(f, 1), code, sizeof code)) {
        social_send_n(s, "rpc", 1, cJSON_CreateString("Invalid or expired code."));
        return;
    }
    ResetReq *r = calloc(1, sizeof *r);
    if (!r) return;
    r->s = social_hold(s);
    kn_db_call(g_acct_brain->db, &P_RESET_CHECK, KN_DB_ARGS(code), reset_check_done, r);
    memset(code, 0, sizeof code);
}

static void reset_page(kn_http_req *req, int status, const char *title, const char *text, const char *form_code);

static void reset_apply_done(kn_db_result *res, void *ud) {
    ResetReq *r = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const char *err = o ? NULL : acct_db_error(res, "reset");
    if (o) {
        const int64_t id = kn_json_int(o, "id", 0);
        hot_session_forget_account(id); /* every older session was revoked */
        KN_INFO(MOD, "account %lld: password reset by email code", (long long) id);
    }
    const char *user = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "username")) : NULL;
    if (r->req) {
        if (err) reset_page(r->req, 400, "Password not changed", err, r->code);
        else reset_page(r->req, 200, "Password changed", "You can log in with your new password now. You can close this page.", NULL);
    } else if (r->s && social_open(r->s)) {
        /* ["rps", null, msg, username, token]: the client signs in with the new session */
        if (err) social_send_n(r->s, "rps", 1, cJSON_CreateString(err));
        else social_send_n(r->s, "rps", 4, cJSON_CreateNull(), cJSON_CreateString("Password updated. You are now logged in."),
                           cJSON_CreateString(user ? user : ""), cJSON_CreateString(r->token));
    }
    cJSON_Delete(o);
    reset_free(r);
}

static void reset_hashed(void *ud, int ok, const char *new_hash) {
    ResetReq *r = ud;
    if (!ok || !new_hash || kn_random_hex(r->token, sizeof r->token, 16) != 0) {
        if (r->req) reset_page(r->req, 500, "Password not changed", "Server error.", r->code);
        else reset_reply(r, "rps", "Server error.", NULL);
        reset_free(r);
        return;
    }
    char ttl[24];
    kn_db_call(g_acct_brain->db, &P_RESET_APPLY, KN_DB_ARGS(r->code, new_hash, r->token, acct_itoa(ttl, g_acct.session_ttl_s)),
               reset_apply_done, r);
}

/* Validates and starts a reset; NULL = started (r owned by the callbacks), else the message. */
static const char *reset_start(ResetReq *r, const char *code, const char *pass, const char *repeat) {
    if (!reset_code_norm(code, r->code, sizeof r->code)) return "Invalid or expired code.";
    if (!pass || !repeat || strcmp(pass, repeat) != 0) return "Passwords do not match.";
    const char *err = acct_validate_password(pass);
    if (err) return err;
    r->pass = strdup(pass);
    if (!r->pass) return "Server error.";
    if (acct_hash_async(r->pass, NULL, reset_hashed, r) != 0) return "Server busy, try again.";
    return NULL;
}

/* ["rps", code, new, repeat] */
void sh_reset_set(SocialSession *s, const cJSON *f) {
    ResetReq *r = calloc(1, sizeof *r);
    if (!r) return;
    r->s = social_hold(s);
    const char *err = reset_start(r, fa_str(f, 1), fa_str(f, 2), fa_str(f, 3));
    if (err) {
        reset_reply(r, "rps", err, NULL);
        reset_free(r);
    }
}

/* The emailed link: GET shows the form (the code prefilled), POST (form-urlencoded token,
 * password, repeat) sets the password. The code is the only credential (no cookies), so a
 * cross-site POST gains nothing it did not already hold. */
static void reset_page(kn_http_req *req, int status, const char *title, const char *text, const char *form_code) {
    kn_buf b;
    kn_buf_init(&b);
    kn_buf_append_str(&b, "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Krunker Box Office - ");
    html_escape(&b, title);
    kn_buf_append_str(&b, "</title><style>body{margin:0;background:#1d1d1d;color:#fff;font-family:Arial,sans-serif;text-align:center}"
                          "div{max-width:560px;margin:12vh auto 0;padding:32px;background:rgba(0,0,0,.35);border-radius:6px}"
                          "h1{color:#fc0;margin:0 0 16px}p{color:rgba(255,255,255,.75);font-size:18px}"
                          "label{display:block;text-align:left;margin:14px 0 4px;color:rgba(255,255,255,.75)}"
                          "input{box-sizing:border-box;width:100%;padding:10px;font-size:16px;border:0;border-radius:4px}"
                          "button{margin-top:20px;padding:12px 28px;font-size:16px;font-weight:bold;background:#fc0;border:0;"
                          "border-radius:4px;cursor:pointer}</style></head><body><div><h1>");
    html_escape(&b, title);
    kn_buf_append_str(&b, "</h1><p>");
    html_escape(&b, text);
    kn_buf_append_str(&b, "</p>");
    if (form_code) {
        kn_buf_append_str(&b, "<form method=\"post\" action=\"/auth/reset-password\"><input type=\"hidden\" name=\"token\" value=\"");
        html_escape(&b, form_code);
        kn_buf_append_str(&b, "\"><label for=\"p1\">New password</label><input id=\"p1\" type=\"password\" name=\"password\" "
                              "autocomplete=\"new-password\" required><label for=\"p2\">Repeat password</label><input id=\"p2\" "
                              "type=\"password\" name=\"repeat\" autocomplete=\"new-password\" required>"
                              "<button type=\"submit\">Set Password</button></form>");
    }
    kn_buf_append_str(&b, "</div></body></html>");
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_set_header(req, "Referrer-Policy", "no-referrer");
    kn_http_set_header(req, "X-Content-Type-Options", "nosniff");
    kn_http_set_header(req, "X-Frame-Options", "DENY");
    kn_http_set_header(req, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");
    kn_http_respond(req, status, "text/html; charset=utf-8", b.data, b.len);
    kn_buf_free(&b);
}

static void h_reset_get(kn_http_req *req, void *ud) {
    (void) ud;
    char token[160], code[RESET_CODE_CHARS + 1];
    if (kn_http_query_get(req, "token", token, sizeof token) <= 0 || !reset_code_norm(token, code, sizeof code)) {
        reset_page(req, 400, "Reset password", "This link is invalid. Request a new code in Krunker Box Office.", NULL);
        return;
    }
    reset_page(req, 200, "Reset password", "Choose a new password for your Krunker Box Office account.", code);
}

/* application/x-www-form-urlencoded field `key` of the body into out (decoded); 0 when absent. */
static int form_get(const kn_http_req *req, const char *key, char *out, size_t cap) {
    const char *p = req->body ? req->body : "";
    const size_t klen = strlen(key);
    while (*p) {
        const char *amp = strchr(p, '&');
        const size_t len = amp ? (size_t) (amp - p) : strlen(p);
        if (len > klen && !strncmp(p, key, klen) && p[klen] == '=')
            return kn_url_decode(p + klen + 1, len - klen - 1, out, cap, 1) >= 0;
        p += len + (amp ? 1 : 0);
    }
    return 0;
}

static void h_reset_post(kn_http_req *req, void *ud) {
    (void) ud;
    char token[160] = "", pass[256] = "", repeat[256] = "";
    form_get(req, "token", token, sizeof token);
    form_get(req, "password", pass, sizeof pass);
    form_get(req, "repeat", repeat, sizeof repeat);
    ResetReq *r = calloc(1, sizeof *r);
    const char *err = !r ? "Server error." : !g_acct_brain || !g_acct_brain->db ? "Server unavailable" : NULL;
    if (r) r->req = req;
    if (!err) err = reset_start(r, token, pass, repeat);
    if (err) {
        char code[RESET_CODE_CHARS + 1];
        reset_page(req, 400, "Password not changed", err, reset_code_norm(token, code, sizeof code) ? code : NULL);
        reset_free(r);
    }
    memset(pass, 0, sizeof pass);
    memset(repeat, 0, sizeof repeat);
}

/* ------------------------------------------------------------------ 2FA (TOTP) */

static void otpauth_uri(const char *username, const char *secret, char *out, size_t cap) {
    char issuer[3 * sizeof s_cfg.issuer];
    kn_url_encode(s_cfg.issuer, issuer, sizeof issuer);
    snprintf(out, cap, "otpauth://totp/%s:%s?secret=%s&issuer=%s&algorithm=SHA1&digits=%d&period=%d", issuer, username, secret,
             issuer, KN_TOTP_DIGITS, KN_TOTP_PERIOD);
}

void acct_twofa_otpauth(const char *username, const char *secret, char *out, size_t cap) {
    otpauth_uri(username, secret, out, cap);
}

typedef struct TwofaReq {
    SocialSession *s;
    char label[8];            /* the confirm label to ask again with */
    char uri[256];            /* a2FA: the otpauth:// URI */
} TwofaReq;

static void twofa_prompt(SocialSession *s, const char *label, const char *uri, const char *msg) {
    /* twoFAres(status 0, nextLabel, otpUri, twoFAVal, KrunkNative message) */
    social_send_n(s, "c2fa", 5, cJSON_CreateNumber(0), cJSON_CreateString(label), uri ? cJSON_CreateString(uri) : cJSON_CreateNull(),
                  cJSON_CreateNull(), msg ? cJSON_CreateString(msg) : cJSON_CreateNull());
}

static void twofa_state(SocialSession *s, int on) {
    social_send_n(s, "c2fa", 4, cJSON_CreateNumber(2), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateBool(on));
}

static void twofa_pending_done(SocialSession *s, kn_db_result *res, void *extra) {
    TwofaReq *t = extra;
    if (!kn_db_ok(res)) social_error(s, acct_db_error(res, "a2FA"));
    else if (kn_db_affected(res) < 1) twofa_state(s, 1); /* already on: refresh the button */
    else twofa_prompt(s, "a2FAc", t->uri, NULL);
}

/* ["a2FA"]: a new secret waits for its first code */
void sh_twofa_enable(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    uint8_t key[20];
    char secret[40], id[24];
    TwofaReq *t = calloc(1, sizeof *t);
    if (!t || kn_random_bytes(key, sizeof key) != 0 || kn_base32_encode(key, sizeof key, secret, sizeof secret) < 0) {
        free(t);
        social_error(s, "Server error.");
        return;
    }
    memset(key, 0, sizeof key);
    otpauth_uri(s->username, secret, t->uri, sizeof t->uri);
    social_query(s, SQL(.any = "UPDATE accounts SET twofa_pending = $2 WHERE id = {int:$1} AND twofa_secret IS NULL"),
                 KN_DB_ARGS(acct_itoa(id, s->account_id), secret), twofa_pending_done, t);
    memset(secret, 0, sizeof secret);
}

static void twofa_code_done(SocialSession *s, kn_db_result *res, void *extra) {
    TwofaReq *t = extra;
    const int disable = !strcmp(t->label, "d2FAc");
    if (!kn_db_ok(res)) {
        const char *msg = acct_db_error(res, t->label);
        if (!strcmp(msg, "2FA is not enabled")) twofa_state(s, 0);
        else if (!strncmp(msg, "2FA setup expired", 17)) {
            twofa_state(s, 0);
            social_error(s, msg);
        } else twofa_prompt(s, t->label, NULL, msg);
        return;
    }
    cJSON *o = acct_db_json(res);
    const int ok = (int) kn_json_int(o, "ok", 0);
    cJSON_Delete(o);
    if (!ok) twofa_prompt(s, t->label, NULL, "Invalid Code");
    else twofa_state(s, !disable);
}

static void twofa_code(SocialSession *s, const cJSON *f, const kn_proc *proc, const char *label) {
    if (!social_require_auth(s)) return;
    const char *code = fa_str(f, 1);
    char digits[16];
    if (!code && cJSON_IsNumber(fa(f, 1))) { /* tolerate a number (leading zeros lost: padded) */
        snprintf(digits, sizeof digits, "%06lld", (long long) fa_int(f, 1, 0));
        code = digits;
    }
    TwofaReq *t = calloc(1, sizeof *t);
    if (!t) return;
    snprintf(t->label, sizeof t->label, "%s", label);
    char id[24], now[24];
    social_call(s, proc, KN_DB_ARGS(acct_itoa(id, s->account_id), code ? code : "", acct_itoa(now, (int64_t) time(NULL))),
                twofa_code_done, t);
}

void sh_twofa_enable_confirm(SocialSession *s, const cJSON *f) { twofa_code(s, f, &P_TWOFA_CONFIRM, "a2FAc"); }
void sh_twofa_disable_confirm(SocialSession *s, const cJSON *f) { twofa_code(s, f, &P_TWOFA_DISABLE, "d2FAc"); }

static void twofa_on_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) social_error(s, acct_db_error(res, "d2FA"));
    else if (kn_db_rows(res) > 0) twofa_prompt(s, "d2FAc", NULL, NULL);
    else twofa_state(s, 0);
}

/* ["d2FA"]: ask for a code first */
void sh_twofa_disable(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    char id[24];
    social_query(s, SQL(.any = "SELECT 1 FROM accounts WHERE id = {int:$1} AND twofa_secret IS NOT NULL"),
                 KN_DB_ARGS(acct_itoa(id, s->account_id)), twofa_on_done, NULL);
}

/* ------------------------------------------------------------------ module */

void acct_security_init(Brain *b) {
    load_config(b);
    start_mailer(b);
    if (b->http) {
        kn_http_route(b->http, "GET", "/auth/verify-email", h_verify, NULL);
        kn_http_route(b->http, "GET", "/api/verify-email", h_verify, NULL);
        kn_http_route(b->http, "GET", "/auth/reset-password", h_reset_get, NULL);
        kn_http_route(b->http, "POST", "/auth/reset-password", h_reset_post, NULL);
    }
    if (s_cfg.gate) {
        char list[128] = "";
        for (size_t f = 0; f < sizeof k_features / sizeof k_features[0]; f++)
            if (s_cfg.gate & (1u << f)) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? "," : "", k_features[f]);
        KN_INFO(MOD, "verified email required for: %s", list);
    }
}

void acct_security_shutdown(Brain *b) {
    (void) b;
    kn_mailer_free(s_mailer);
    s_mailer = NULL;
}
