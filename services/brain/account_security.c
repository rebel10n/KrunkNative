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
 *  - Outbound mail through knsvc/smtp.h's kn_mailer (config "smtp"): queued on a worker thread,
 *    every mail in mail_log (status, Message-ID, the server's error; never credentials or tokens).
 */
#include "acct_internal.h"
#include "brain_internal.h" /* mm_settings()->public_url */

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
} MailCtx;

static void log_update_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "mail_log update: %s", kn_db_error(res));
}

static void mail_done(void *ud, int status, const char *message_id, const char *error) {
    MailCtx *m = ud;
    char id[24];
    if (status == KN_SMTP_OK) KN_INFO(MOD, "verify mail #%lld to %s sent %s", (long long) m->log_id, m->masked, message_id ? message_id : "");
    else KN_WARN(MOD, "verify mail #%lld to %s failed: %s", (long long) m->log_id, m->masked, error ? error : "?");
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
    if (r->s && social_open(r->s)) {
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

/* email NULL = resend to the stored address. s (optional) gets the reply. */
static void email_request(SocialSession *s, int64_t account_id, const char *email, const char *ip, const char *label) {
    EmailReq *r = calloc(1, sizeof *r);
    if (!r) return;
    r->account_id = account_id;
    snprintf(r->label, sizeof r->label, "%s", label);
    if (kn_random_token(r->token, sizeof r->token, 32) != 0) {
        free(r);
        if (s) social_send_n(s, label, 1, cJSON_CreateString("Server error."));
        return;
    }
    r->s = s ? social_hold(s) : NULL;
    char id[24], ttl[24], cool[24], d1[24], d2[24], d3[24], now[24];
    const char *args[] = {acct_itoa(id, account_id), email, ip && *ip ? ip : NULL, r->token,
                          acct_itoa(ttl, s_cfg.verify_ttl_s), acct_itoa(cool, s_cfg.resend_cooldown_s),
                          acct_itoa(d1, s_cfg.max_per_account_day), acct_itoa(d2, s_cfg.max_per_address_day),
                          acct_itoa(d3, s_cfg.max_per_ip_hour), s_mailer ? "1" : "0",
                          acct_itoa(now, now_ms())};
    kn_db_call(g_acct_brain->db, &P_EMAIL_REQUEST, (int) (sizeof args / sizeof args[0]), args, email_req_done, r);
}

void acct_email_after_register(int64_t account_id, const char *email, const char *ip) {
    if (account_id > 0 && email && *email && g_acct_brain && g_acct_brain->db) email_request(NULL, account_id, email, ip, "cemail");
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
    email_request(s, s->account_id, norm, s->ip, "cemail");
}

/* ["emailv"] */
void sh_email_resend(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    email_request(s, s->account_id, NULL, s->ip, "emailv");
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

/* ------------------------------------------------------------------ 2FA (TOTP) */

static void otpauth_uri(const char *username, const char *secret, char *out, size_t cap) {
    char issuer[3 * sizeof s_cfg.issuer];
    kn_url_encode(s_cfg.issuer, issuer, sizeof issuer);
    snprintf(out, cap, "otpauth://totp/%s:%s?secret=%s&issuer=%s&algorithm=SHA1&digits=%d&period=%d", issuer, username, secret,
             issuer, KN_TOTP_DIGITS, KN_TOTP_PERIOD);
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
