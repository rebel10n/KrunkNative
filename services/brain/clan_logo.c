/*
 * services/brain/clan_logo.c - clan logos (Krunker v10.0.2's clan settings tab, "Clan Logo"):
 * upload, review, serving. Migration 0332_clan_logos (db rows and the procs below).
 *
 *   POST /clans/<id>/logo {file: base64 PNG} (gapi; clan_v10.c dispatches here)
 *       -> 200 {status: "pending", clan_logo_pending: <version>, clan_funds} once the upload is stored
 *          as pending; the review runs after the reply. 20,000 KR come from the clan bank only when
 *          an approval sets the logo (a rejection or a failed review charges nothing).
 *          Errors: the gapi error code is the profile path's name (UnsupportedType 415, FileTooLarge
 *          413, ImageTooSmall 422 / ImageTooLarge 413 for a picture that is not 512 x 512, BadImage
 *          422, AvatarsUnavailable / ModerationUnavailable 503), then the clan's refusals: not the
 *          owner or a Captain, a logo already pending, not enough clan funds (CLAN_ERROR).
 *   GET  /api/clans/<id>/logo  the approved logo: image/webp, ETag, Cache-Control 300 s; 404 otherwise.
 *   DELETE /clans/<id>/logo is clan_v10.c's (it clears the clans row and the row here).
 *
 * The rules are the profile pictures': the same decoder (avatar_image.c), the same review
 * (avatar_review_picture: the moderation profile, the strict verdict of avatar_verdict.c, fail
 * closed), and the picture is stored only with an approval. The clan's own rules: PNG only, exactly
 * CLAN_LOGO_PX square, at most CLAN_LOGO_MAX_BYTES (v10), kept as a lossless WebP; the owner or a
 * Captain (v10's settings access, clans.md); one pending logo per clan. The charge is 20,000 KR from
 * the clan bank with no fee (clan_funds_ledger row source 'clan-logo'), taken only by an approval
 * (the profile picture costs nothing, so a rejected or failed review charges nothing either; nothing
 * is refunded). The approved logo stays shown while a newer one is under review.
 *
 * The clans row keeps v10's version numbers: clan_logo_pending is the version under review, clan_logo
 * the approved one (0 = none); approval moves the first into the second.
 */
#include "clan_logo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accounts.h"
#include "ai.h"
#include "avatar.h"
#include "avatar_image.h"
#include "brain.h"
#include "brain_internal.h"
#include "db_procs.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "clan-logo"
#define LOGO_PRICE 20000 /* v10: "Insufficient clan funds. Need 20,000 krunkies." (the bank pays, no fee) */

static struct {
    Brain *b;
    int enabled;
} L;

static int db_ready(void) { return L.b && L.b->db && L.enabled; }

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }
static const char *nz(const char *s) { return s ? s : ""; }
#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

/* NULL-safe: a missing or unparsable body reads as empty fields */
static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static int64_t jint(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : 0;
}

/* ------------------------------------------------------------------ procs (C on every driver) */

/* 1 when the actor may change the clan's logo: the owner, or a Captain (rank 2) */
static int logo_manager(Dbx *x, const int64_t actor, const int64_t clan) {
    if (dbx_exists(x, SQL(.any = "SELECT 1 FROM clans WHERE clan_id = $1 AND clan_playerid = $2"), KN_DB_ARGS(I64S(clan), I64S(actor))))
        return 1;
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM clan_members WHERE clan_id = $1 AND account_id = $2 AND rank = 2"),
                      KN_DB_ARGS(I64S(clan), I64S(actor)));
}

/* (actor, clan, json {sha256, bytes, width, height}) -> {status, clan_logo_pending, clan_funds}.
 * Nothing is charged here: a profile picture costs nothing, and the clan pays LOGO_PRICE only when
 * the approval sets the logo (run_logo_verdict). The clan must have the funds and no logo pending
 * (one conditional UPDATE: a refusal changes nothing). */
static kn_db_result *run_logo_put(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t acc = PI(0), clan = PI(1);
    cJSON *in = cJSON_Parse(nz(P(2)));
    if (!logo_manager(&x, acc, clan)) {
        cJSON_Delete(in);
        if (!dbx_failed(&x)) dbx_refuse(&x, "Only the clan owner or a Captain can change the clan logo");
        return dbx_finish(&x, NULL);
    }
    if (dbx_exec(&x,
                 SQL(.any = "UPDATE clans SET clan_logo_pending = clan_logo + 1"
                            " WHERE clan_id = {int:$1} AND clan_logo_pending = 0 AND clan_funds >= {int:$2}"),
                 KN_DB_ARGS(I64S(clan), I64S(LOGO_PRICE))) != 1) {
        cJSON_Delete(in);
        if (!dbx_failed(&x)) {
            const int pending = dbx_exists(&x, SQL(.any = "SELECT 1 FROM clans WHERE clan_id = {int:$1} AND clan_logo_pending <> 0"),
                                           KN_DB_ARGS(I64S(clan)));
            dbx_refuse(&x, pending ? "A logo is already pending review. Please wait before uploading a new one."
                                   : "Insufficient clan funds. Need 20,000 krunkies.");
        }
        return dbx_finish(&x, NULL);
    }
    const int64_t version = dbx_int(&x, SQL(.any = "SELECT clan_logo_pending FROM clans WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)), 0);
    const char *sha = jstr(in, "sha256");
    const int64_t bytes = jint(in, "bytes"), w = jint(in, "width"), h = jint(in, "height");
    const int64_t upd = dbx_exec(
        &x,
        SQL(.any = "UPDATE clan_logos SET status = 'pending', version = {int:$2}, sha256 = $3, bytes = {int:$4}, width = {int:$5},"
                   " height = {int:$6}, account_id = {int:$7}, reason = '', categories = '', model = '', uploaded_at = {now},"
                   " moderated_at = NULL WHERE clan_id = {int:$1}"),
        KN_DB_ARGS(I64S(clan), I64S(version), sha, I64S(bytes), I64S(w), I64S(h), I64S(acc)));
    if (upd == 0 && !dbx_failed(&x))
        dbx_exec(&x,
                 SQL(.any = "INSERT INTO clan_logos (clan_id, status, version, sha256, bytes, width, height, account_id, uploaded_at)"
                            " VALUES ({int:$1}, 'pending', {int:$2}, $3, {int:$4}, {int:$5}, {int:$6}, {int:$7}, {now})"),
                 KN_DB_ARGS(I64S(clan), I64S(version), sha, I64S(bytes), I64S(w), I64S(h), I64S(acc)));
    cJSON_Delete(in);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "pending");
    cJSON_AddNumberToObject(o, "clan_logo_pending", (double) version);
    cJSON_AddNumberToObject(o, "clan_funds",
                            (double) dbx_int(&x, SQL(.any = "SELECT clan_funds FROM clans WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)), 0));
    return dbx_json(&x, "clan_logo", o);
}
const kn_proc P_CLAN_LOGO_PUT = {"clan_logo_put", NULL, run_logo_put};

/* (json {clan, sha256, status, reason, categories, model, image?}) -> {n}: the review of the upload
 * with this sha256 (n 0 when a newer upload or a delete replaced it). An approval is the one step
 * that charges: the clan pays LOGO_PRICE and the version under review becomes clan_logo, in the same
 * statement, only while the clan still has the funds. Without the funds the logo is not set (status
 * error, the reason says why) and nothing is charged. A rejection or a failed review charges nothing. */
static kn_db_result *run_logo_verdict(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = cJSON_Parse(nz(P(0)));
    const int64_t clan = jint(in, "clan");
    const char *status = jstr(in, "status");
    const char *sha = jstr(in, "sha256");
    const char *image = jstr(in, "image");
    const int approved = !strcmp(status, "approved");
    int64_t upd = 0;
    if (clan > 0 && (approved || !strcmp(status, "rejected") || !strcmp(status, "error"))) {
        if (approved) {
            const int paid = dbx_exec(
                &x,
                SQL(.any = "UPDATE clans SET clan_funds = clan_funds - {int:$2}, clan_logo = clan_logo_pending, clan_logo_pending = 0"
                           " WHERE clan_id = {int:$1} AND clan_logo_pending > 0 AND clan_funds >= {int:$2}"
                           " AND EXISTS (SELECT 1 FROM clan_logos WHERE clan_id = {int:$1} AND sha256 = $3 AND status = 'pending')"),
                KN_DB_ARGS(I64S(clan), I64S(LOGO_PRICE), sha)) == 1;
            if (paid) {
                upd = dbx_exec(&x,
                               SQL(.any = "UPDATE clan_logos SET status = 'approved', reason = $3, categories = '', model = $4,"
                                          " moderated_at = {now}, image = $5 WHERE clan_id = {int:$1} AND sha256 = $2 AND status = 'pending'"),
                               KN_DB_ARGS(I64S(clan), sha, jstr(in, "reason"), jstr(in, "model"), image[0] ? image : NULL));
                if (upd > 0) {
                    const int64_t acc = dbx_int(&x, SQL(.any = "SELECT account_id FROM clan_logos WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)), 0);
                    dbx_exec(&x,
                             SQL(.any = "INSERT INTO clan_funds_ledger (clan_id, account_id, amount, source, ref) VALUES ({int:$1}, {int:$2}, {int:$3}, 'clan-logo', NULL)"),
                             KN_DB_ARGS(I64S(clan), I64S(acc), I64S(-LOGO_PRICE)));
                }
            } else {
                /* the clan has not the funds (any more): the logo is not set, nothing is charged */
                upd = dbx_exec(&x,
                               SQL(.any = "UPDATE clan_logos SET status = 'error', reason = $3, categories = '', model = $4,"
                                          " moderated_at = {now} WHERE clan_id = {int:$1} AND sha256 = $2 AND status = 'pending'"),
                               KN_DB_ARGS(I64S(clan), sha, "Not enough clan funds: a logo costs 20,000 KR from the clan bank.", jstr(in, "model")));
                if (upd > 0) dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_logo_pending = 0 WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)));
            }
        } else {
            upd = dbx_exec(&x,
                           SQL(.any = "UPDATE clan_logos SET status = $3, reason = $4, categories = $5, model = $6, moderated_at = {now}"
                                      " WHERE clan_id = {int:$1} AND sha256 = $2 AND status = 'pending'"),
                           KN_DB_ARGS(I64S(clan), sha, status, jstr(in, "reason"), jstr(in, "categories"), jstr(in, "model")));
            if (upd > 0) dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_logo_pending = 0 WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)));
        }
    }
    cJSON_Delete(in);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", (double) (upd > 0 ? upd : 0));
    return dbx_json(&x, "clan_logo_verdict", o);
}
const kn_proc P_CLAN_LOGO_VERDICT = {"clan_logo_verdict", NULL, run_logo_verdict};

/* (clan) -> {sha256, image} when a logo is approved, else {} */
static kn_db_result *run_logo_public(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT sha256, image FROM clan_logos WHERE clan_id = {int:$1} AND image IS NOT NULL"),
                         KN_DB_ARGS(I64S(PI(0))));
    return dbx_json(&x, "clan_logo_public", row ? row : cJSON_CreateObject());
}
const kn_proc P_CLAN_LOGO_PUBLIC = {"clan_logo_public", NULL, run_logo_public};

/* ------------------------------------------------------------------ upload */

/* one upload from the decode to its verdict: the normalized WebP (base64) is kept for an approval.
 * s is the socket the request came from, held (social_hold) until its gapi answer is sent: the
 * stored pending row is reviewed even when that socket has gone meanwhile. */
typedef struct {
    SocialSession *s;
    int64_t rid;
    int64_t clan;
    char sha256[65];
    char *b64;
} LogoUpload;

static void logo_free(LogoUpload *u) {
    if (!u) return;
    free(u->b64);
    free(u);
}

static void on_verdict_saved(kn_db_result *res, void *ud) {
    LogoUpload *u = ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "logo verdict for clan %lld not saved: %s", (long long) u->clan, kn_db_error(res));
    logo_free(u);
}

static void on_logo_verdict(void *ud, const PictureVerdict *v) {
    LogoUpload *u = ud;
    KN_INFO(MOD, "logo review for clan %lld: %s", (long long) u->clan, v->status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "clan", (double) u->clan);
    cJSON_AddStringToObject(o, "sha256", u->sha256);
    cJSON_AddStringToObject(o, "status", v->status);
    cJSON_AddStringToObject(o, "reason", v->reason);
    cJSON_AddStringToObject(o, "categories", v->categories);
    cJSON_AddStringToObject(o, "model", v->model);
    if (!strcmp(v->status, "approved")) cJSON_AddStringToObject(o, "image", u->b64);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) {
        logo_free(u);
        return;
    }
    kn_db_call(L.b->db, &P_CLAN_LOGO_VERDICT, KN_DB_ARGS(json), on_verdict_saved, u);
    free(json);
}

static void logo_put_done(kn_db_result *res, void *ud) {
    LogoUpload *u = ud;
    SocialSession *s = u->s;
    if (!kn_db_ok(res)) {
        gapi_error(s, u->rid, 400, "CLAN_ERROR", acct_db_error(res, "clan logo"));
        social_unhold(s);
        logo_free(u);
        return;
    }
    gapi_reply(s, u->rid, acct_db_json(res));
    social_unhold(s);
    u->s = NULL;
    /* the upload is stored as pending and paid: the review runs now, its verdict lands in the row */
    avatar_review_picture(u->b64, on_logo_verdict, u);
}

/* the gapi error for a picture the intake refused: the profile path's error name, its status and
 * the settings tab's text (v10 windows.clans.settings.logo) */
static void logo_refuse(SocialSession *s, const int64_t rid, const char *name) {
    int status = 422;
    const char *text = "Invalid image file";
    if (!strcmp(name, "UnsupportedType")) {
        status = 415;
        text = "File must be a PNG image";
    } else if (!strcmp(name, "FileTooLarge")) {
        status = 413;
        text = "File size must be 256KB or less";
    } else if (!strcmp(name, "ImageTooSmall")) {
        text = "Image must be exactly 512x512 pixels";
    } else if (!strcmp(name, "ImageTooLarge")) {
        status = 413;
        text = "Image must be exactly 512x512 pixels";
    } else if (!strcmp(name, "AvatarsUnavailable") || !strcmp(name, "ModerationUnavailable")) {
        status = 503;
        text = "Clan logos are not available right now.";
    }
    gapi_error(s, rid, status, name, text);
}

void clan_logo_upload(SocialSession *s, const int64_t rid, const char *me, const int64_t clan, const cJSON *body) {
    if (!db_ready() || !avatar_image_available()) return gapi_error(s, rid, 503, "AvatarsUnavailable", "Clan logos are not available right now.");
    if (!ai_moderation_available()) return logo_refuse(s, rid, "ModerationUnavailable");
    const char *b64 = kn_json_str(body, "file", "");
    if (!b64[0]) return gapi_error(s, rid, 400, "BadRequest", "Choose a PNG file to upload.");
    if (strlen(b64) > CLAN_LOGO_MAX_BYTES / 3 * 4 + 8) return logo_refuse(s, rid, "FileTooLarge");
    /* room for the padding of a base64 text at the length cap: an oversize file reads as FileTooLarge, not BadImage */
    uint8_t *raw = malloc(CLAN_LOGO_MAX_BYTES + 8);
    const int n = raw ? kn_base64_decode(b64, strlen(b64), raw, CLAN_LOGO_MAX_BYTES + 8) : -1;
    if (!raw) return gapi_error(s, rid, 500, "internal", "Internal error");
    if (n > (int) CLAN_LOGO_MAX_BYTES) {
        free(raw);
        return logo_refuse(s, rid, "FileTooLarge");
    }
    if (n <= 0) {
        free(raw);
        return logo_refuse(s, rid, "BadImage");
    }
    AvatarImage img;
    const char *err = avatar_logo_normalize(raw, (size_t) n, &img);
    free(raw);
    if (err) return logo_refuse(s, rid, err);

    LogoUpload *u = calloc(1, sizeof(*u));
    const size_t cap = 4 * ((img.webp_len + 2) / 3) + 1;
    if (u) u->b64 = malloc(cap);
    if (!u || !u->b64 || kn_base64_encode(img.webp, img.webp_len, u->b64, cap, 0, 1) < 0) {
        avatar_image_free(&img);
        logo_free(u);
        return gapi_error(s, rid, 500, "internal", "Internal error");
    }
    uint8_t digest[KN_SHA256_LEN];
    kn_sha256(img.webp, img.webp_len, digest);
    kn_hex_encode(digest, sizeof(digest), u->sha256, sizeof(u->sha256));
    u->s = s;
    u->rid = rid;
    u->clan = clan;
    cJSON *put = cJSON_CreateObject();
    cJSON_AddStringToObject(put, "sha256", u->sha256);
    cJSON_AddNumberToObject(put, "bytes", (double) img.webp_len);
    cJSON_AddNumberToObject(put, "width", img.src_w);
    cJSON_AddNumberToObject(put, "height", img.src_h);
    char *json = cJSON_PrintUnformatted(put);
    cJSON_Delete(put);
    avatar_image_free(&img);
    char cl[24];
    snprintf(cl, sizeof(cl), "%lld", (long long) clan);
    if (!json) {
        logo_free(u);
        return gapi_error(s, rid, 500, "internal", "Internal error");
    }
    social_hold(s);
    const int rc = kn_db_call(L.b->db, &P_CLAN_LOGO_PUT, KN_DB_ARGS(me, cl, json), logo_put_done, u);
    free(json);
    if (rc != 0) {
        gapi_error(s, rid, 500, "internal", "Internal error");
        social_unhold(s);
        logo_free(u);
    }
}

/* ------------------------------------------------------------------ GET /api/clans/:id/logo */

static void reply_error(kn_http_req *req, int status, const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 0);
    cJSON_AddStringToObject(o, "error", name);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

/* the first row's JSON text (every proc answers one text column); NULL when there is no row */
static cJSON *row_json(kn_db_result *res) {
    if (!kn_db_ok(res) || kn_db_rows(res) < 1 || kn_db_isnull(res, 0, 0)) return NULL;
    return cJSON_Parse(kn_db_get(res, 0, 0));
}

static void on_public_row(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) return reply_error(req, 500, "internal");
    cJSON *row = row_json(res);
    const char *b64 = row ? kn_json_str(row, "image", "") : "";
    if (!row || !b64[0]) {
        cJSON_Delete(row);
        return reply_error(req, 404, "NotFound");
    }
    char etag[96];
    snprintf(etag, sizeof(etag), "\"%.80s\"", kn_json_str(row, "sha256", ""));
    uint8_t *bytes = malloc(CLAN_LOGO_MAX_BYTES * 2);
    const int n = bytes ? kn_base64_decode(b64, strlen(b64), bytes, CLAN_LOGO_MAX_BYTES * 2) : -1;
    cJSON_Delete(row);
    if (n <= 0) {
        free(bytes);
        return reply_error(req, 500, "internal");
    }
    const char *inm = kn_http_header_get(req, "If-None-Match");
    brain_cors_headers(req);
    kn_http_set_header(req, "ETag", etag);
    kn_http_set_header(req, "Cache-Control", "public, max-age=300");
    if (inm && !strcmp(inm, etag)) kn_http_respond(req, 304, "image/webp", NULL, 0);
    else kn_http_respond(req, 200, "image/webp", bytes, (size_t) n);
    free(bytes);
}

static void h_public(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "AvatarsUnavailable");
    const char *raw = kn_http_param(req, "id");
    char *end = NULL;
    const long long id = raw && raw[0] ? strtoll(raw, &end, 10) : 0;
    if (id <= 0 || (end && *end)) return reply_error(req, 404, "NotFound");
    char idtxt[24];
    snprintf(idtxt, sizeof(idtxt), "%lld", id);
    kn_db_call(L.b->db, &P_CLAN_LOGO_PUBLIC, KN_DB_ARGS(idtxt), on_public_row, req);
}

/* ------------------------------------------------------------------ module */

int clan_logo_init(Brain *b) {
    memset(&L, 0, sizeof(L));
    L.b = b;
    L.enabled = kn_config_bool(b->cfg, "avatar.enabled", 1); /* the picture pipeline's switch (avatar.c) */
    kn_http_route(b->http, "GET", "/api/clans/:id/logo", h_public, NULL);
    KN_INFO(MOD, "clan logos: POST /clans/<id>/logo (gapi), GET /api/clans/<id>/logo (%s)",
            avatar_image_available() && L.enabled ? "on" : "off");
    return 0;
}

void clan_logo_shutdown(Brain *b) {
    (void) b;
    L.enabled = 0;
}
