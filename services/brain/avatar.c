/*
 * services/brain/avatar.c - profile pictures: upload, AI review, storage and serving (migration
 * 0123_account_avatars, db_avatar.c; the picture pipeline is avatar_image.c, the verdict rules are
 * avatar_verdict.c, the review call is ai_moderate_image in ai.c).
 *
 * Routes (public listener):
 *   POST   /api/account/avatar  {token?, image: base64 of a PNG / JPEG / WebP}
 *            -> 202 {success, data: {status: "pending", width, height}} once the picture is stored
 *               as pending. It is decoded, cropped to a square and re-encoded as a 256 px WebP;
 *               the upload's bytes are never stored. The moderation model then reviews it (the
 *               verdict arrives asynchronously; read it with GET). Errors: 400 BadRequest, 401
 *               LoginRequired, 404 NotFound (account), 413 FileTooLarge / ImageTooLarge, 415
 *               UnsupportedType, 422 ImageTooSmall / BadImage, 429 TooManyRequests (avatar.uploads_per_hour
 *               per account, a rolling hour), 503 AvatarsUnavailable / ModerationUnavailable.
 *   GET    /api/account/avatar  the owner's status, never the image: {data: {status: "none" |
 *               "pending" | "approved" | "rejected" | "error", reason, categories[], model, bytes,
 *               width, height, uploaded_ms, moderated_ms, shown, image_url?}}
 *   DELETE /api/account/avatar  the owner removes the picture: {data: {deleted}}
 *   GET    /api/avatars/:id     the picture (image/webp, ETag, Cache-Control 300 s) when it is
 *               APPROVED; otherwise 404 NotFound. Nothing under review or refused is served.
 *
 * Auth: the session token ("token" in the JSON body, Authorization: Bearer, or ?token=), resolved
 * through accounts_internal_call("token") like the Turf Wars routes.
 *
 * Config: avatar.enabled (1), avatar.moderation_model ("gemini-3.8-flash-high": the abulxt
 * moderation model on the /cpa gateway), avatar.moderation_timeout_ms (60000), avatar.uploads_per_hour
 * (5). The endpoint, key, model override and limits come from the moderation provider profile,
 * ai_provider_get("moderation") (ai_provider.h: ai.moderation.provider, default "default" = the
 * legacy ai.upstream_url / ai.upstream_key). The key is read only by ai.c and never logged.
 *
 * Fail closed: a review that errors, times out, or answers anything but the exact approval shape
 * stores status error (or rejected). The image column is written only with an approval, so a
 * picture that did not pass is never stored.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accounts.h"
#include "ai.h"
#include "avatar.h"
#include "avatar_image.h"
#include "avatar_verdict.h"
#include "brain.h"
#include "brain_internal.h"
#include "db_procs.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "avatar"
#define QUOTA_SLOTS 4096
#define HOUR_MS (60u * 60u * 1000u)

static struct {
    Brain *b;
    int enabled;
    char model[96];
    int timeout_ms;
    int per_hour;
    struct {
        int64_t id;
        uint64_t start_ms;
        int count;
    } quota[QUOTA_SLOTS];
    int nquota;
} A;

static int db_ready(void) { return A.b && A.b->db && A.enabled; }

static void reply_error(kn_http_req *req, int status, const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 0);
    cJSON_AddStringToObject(o, "error", name);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

/* the answer's data object is consumed */
static void reply_data(kn_http_req *req, int status, cJSON *data) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    cJSON_AddItemToObject(o, "data", data);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

/* the first row's JSON text (every proc answers one text column); NULL when there is no row */
static cJSON *row_json(kn_db_result *res) {
    if (!kn_db_ok(res) || kn_db_rows(res) < 1 || kn_db_isnull(res, 0, 0)) return NULL;
    return cJSON_Parse(kn_db_get(res, 0, 0));
}

/* ---- the upload quota: avatar.uploads_per_hour per account in a rolling hour (memory only) */

static int quota_take(const int64_t id) {
    const uint64_t now = kn_now_ms();
    int free_slot = -1;
    for (int i = 0; i < A.nquota; i++) {
        if (now - A.quota[i].start_ms >= HOUR_MS) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (A.quota[i].id == id) {
            if (A.quota[i].count >= A.per_hour) return 0;
            A.quota[i].count++;
            return 1;
        }
    }
    if (free_slot < 0 && A.nquota < QUOTA_SLOTS) free_slot = A.nquota++;
    if (free_slot < 0) return 0; /* the table is full of live entries: refuse until one expires */
    A.quota[free_slot].id = id;
    A.quota[free_slot].start_ms = now;
    A.quota[free_slot].count = 1;
    return 1;
}

/* ---- token -> account (the Turf Wars pattern) */

typedef void (*AccountFn)(kn_http_req *req, int64_t id, void *ctx);
typedef struct {
    kn_http_req *req;
    AccountFn fn;
    void *ctx;
} AuthCtx;

static void on_account(void *ud, const char *err, const cJSON *acc) {
    AuthCtx *a = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        reply_error(a->req, 401, "LoginRequired");
    } else {
        a->fn(a->req, id, a->ctx);
    }
    free(a);
}

static void with_account(kn_http_req *req, const char *token, AccountFn fn, void *ctx) {
    AuthCtx *a = calloc(1, sizeof(*a));
    if (!a) {
        reply_error(req, 500, "internal");
        return;
    }
    a->req = req;
    a->fn = fn;
    a->ctx = ctx;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(A.b, "token", args, on_account, a);
    cJSON_Delete(args);
}

/* the session token: the JSON body's "token", else Authorization: Bearer, else ?token= */
static void token_of(kn_http_req *req, const cJSON *body, char *out, const size_t n) {
    out[0] = 0;
    const char *t = kn_json_str(body, "token", "");
    if (t[0]) {
        snprintf(out, n, "%s", t);
        return;
    }
    const char *bearer = kn_http_bearer(req);
    if (bearer && bearer[0]) {
        snprintf(out, n, "%s", bearer);
        return;
    }
    kn_http_query_get(req, "token", out, n);
}

/* ---- POST /api/account/avatar */

/* one upload on its way: the base64 sent by the client */
typedef struct {
    char *b64;
} Upload;

/* the normalized picture, from decode to the stored pending row */
typedef struct {
    AvatarImage img;
    char *b64;            /* base64 of img.webp (what an approval stores) */
    char sha256[65];
} Picture;

/* one review: the pending picture's verdict is written to the row with this sha256 */
typedef struct {
    kn_http_req *req;     /* answered once the pending row is stored (then NULL) */
    int64_t id;
    size_t bytes;
    int src_w, src_h;
    char sha256[65];
    char *b64;
} Review;

static void picture_free(Picture *p) {
    if (!p) return;
    avatar_image_free(&p->img);
    free(p->b64);
    free(p);
}

static void review_free(Review *r) {
    if (!r) return;
    free(r->b64);
    free(r);
}

static void on_verdict_saved(kn_db_result *res, void *ud) {
    Review *r = ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "verdict for account %lld not saved: %s", (long long) r->id, kn_db_error(res));
    review_free(r);
}

#define REVIEW_FAILED "Automatic review failed. Try uploading the picture again later."

/* writes the verdict for this picture (status approved | rejected | error) and frees r */
static void save_verdict(Review *r, const char *status, const char *reason, const char *categories, const char *model) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) r->id);
    cJSON_AddStringToObject(o, "sha256", r->sha256);
    cJSON_AddStringToObject(o, "status", status);
    cJSON_AddStringToObject(o, "reason", reason);
    cJSON_AddStringToObject(o, "categories", categories);
    cJSON_AddStringToObject(o, "model", model);
    if (!strcmp(status, "approved")) cJSON_AddStringToObject(o, "image", r->b64);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) {
        review_free(r);
        return;
    }
    kn_db_call(A.b->db, &P_AVATAR_VERDICT, KN_DB_ARGS(json), on_verdict_saved, r);
    free(json);
}

/* ---- the review: one moderation call per picture, the strict verdict, fail closed (avatar_review_picture) */

typedef struct {
    char nonce[32];
    PictureVerdictFn done;
    void *ud;
} PictureReview;

/* the model the review runs on: the moderation profile's own model when it names one, else
 * avatar.moderation_model (ai_moderate_image makes the same choice) */
static const char *review_model(void) {
    const AiProvider *p = ai_provider_get("moderation");
    return p && p->model[0] ? p->model : A.model;
}

/* hands the verdict to the caller, then frees c (the callback must not keep c) */
static void picture_done(PictureReview *c, const char *status, const char *reason, const char *categories) {
    PictureVerdict v = {status, reason, categories, review_model()};
    c->done(c->ud, &v);
    free(c);
}

static void on_picture_answer(void *ud, const char *err, const char *text) {
    PictureReview *c = ud;
    if (err) {
        KN_WARN(MOD, "picture review failed: %.200s", err);
        picture_done(c, "error", REVIEW_FAILED, "");
        return;
    }
    AvatarVerdict v;
    const int verdict = avatar_verdict_parse(text, c->nonce, &v);
    if (verdict == AVATAR_VERDICT_APPROVED) picture_done(c, "approved", v.reason, "");
    else if (verdict == AVATAR_VERDICT_REJECTED) picture_done(c, "rejected", v.reason, v.categories);
    else picture_done(c, "error", v.reason, "");
}

void avatar_review_picture(const char *b64, PictureVerdictFn done, void *ud) {
    PictureReview *c = calloc(1, sizeof(*c));
    if (!c) {
        PictureVerdict v = {"error", REVIEW_FAILED, "", review_model()};
        done(ud, &v);
        return;
    }
    c->done = done;
    c->ud = ud;
    char prompt[4096];
    if (avatar_verdict_nonce(c->nonce, sizeof(c->nonce)) != 0 || avatar_verdict_prompt(prompt, sizeof(prompt), c->nonce) != 0) {
        KN_WARN(MOD, "moderation prompt does not fit");
        picture_done(c, "error", REVIEW_FAILED, "");
        return;
    }
    const size_t n = strlen(b64) + 32;
    char *url = malloc(n);
    if (!url) {
        picture_done(c, "error", REVIEW_FAILED, "");
        return;
    }
    snprintf(url, n, "data:image/webp;base64,%s", b64);
    ai_moderate_image(A.model, prompt, avatar_verdict_question(), url, A.timeout_ms, on_picture_answer, c);
    free(url);
}

/* the review of a profile picture: its verdict is written to the account's row */
static void on_picture_verdict(void *ud, const PictureVerdict *v) {
    Review *r = ud;
    KN_INFO(MOD, "picture review for account %lld: %s", (long long) r->id, v->status);
    save_verdict(r, v->status, v->reason, v->categories, v->model);
}

static void start_review(Review *r) { avatar_review_picture(r->b64, on_picture_verdict, r); }

static void on_pending_stored(kn_db_result *res, void *ud) {
    Review *r = ud;
    kn_http_req *req = r->req;
    r->req = NULL;
    if (!kn_db_ok(res)) {
        const char *msg = kn_db_error(res);
        KN_WARN(MOD, "picture for account %lld not stored: %s", (long long) r->id, msg);
        reply_error(req, msg && strstr(msg, "Account not found") ? 404 : 500, msg && strstr(msg, "Account not found") ? "NotFound" : "internal");
        review_free(r);
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status", "pending");
    cJSON_AddNumberToObject(data, "width", AVATAR_PX);
    cJSON_AddNumberToObject(data, "height", AVATAR_PX);
    cJSON_AddNumberToObject(data, "source_width", r->src_w);
    cJSON_AddNumberToObject(data, "source_height", r->src_h);
    reply_data(req, 202, data);
    start_review(r);
}

static void store_pending(kn_http_req *req, const int64_t id, Picture *p) {
    Review *r = calloc(1, sizeof(*r));
    cJSON *put = cJSON_CreateObject();
    char *json = NULL;
    if (r && put) {
        r->req = req;
        r->id = id;
        r->bytes = p->img.webp_len;
        r->src_w = p->img.src_w;
        r->src_h = p->img.src_h;
        snprintf(r->sha256, sizeof(r->sha256), "%s", p->sha256);
        r->b64 = p->b64;
        p->b64 = NULL;
        cJSON_AddNumberToObject(put, "id", (double) id);
        cJSON_AddStringToObject(put, "sha256", r->sha256);
        cJSON_AddNumberToObject(put, "bytes", (double) r->bytes);
        cJSON_AddNumberToObject(put, "width", r->src_w);
        cJSON_AddNumberToObject(put, "height", r->src_h);
        json = cJSON_PrintUnformatted(put);
    }
    cJSON_Delete(put);
    picture_free(p);
    if (!r || !json) {
        review_free(r);
        free(json);
        reply_error(req, 500, "internal");
        return;
    }
    kn_db_call(A.b->db, &P_AVATAR_PUT, KN_DB_ARGS(json), on_pending_stored, r);
    free(json);
}

/* the decode / normalize / encode step, after the account is known and the quota taken */
static void upload_with_account(kn_http_req *req, const int64_t id, void *ctx) {
    Upload *u = ctx;
    if (!quota_take(id)) {
        free(u->b64);
        free(u);
        reply_error(req, 429, "TooManyRequests");
        return;
    }
    uint8_t *raw = malloc(AVATAR_MAX_BYTES);
    const int n = raw ? kn_base64_decode(u->b64, strlen(u->b64), raw, AVATAR_MAX_BYTES) : -1;
    free(u->b64);
    free(u);
    if (!raw) {
        reply_error(req, 500, "internal");
        return;
    }
    if (n <= 0) {
        free(raw);
        reply_error(req, 422, "BadImage");
        return;
    }
    AvatarImage img;
    const char *err = avatar_image_normalize(raw, (size_t) n, &img);
    free(raw);
    if (err) {
        int status = 422;
        if (!strcmp(err, "UnsupportedType")) status = 415;
        else if (!strcmp(err, "FileTooLarge") || !strcmp(err, "ImageTooLarge")) status = 413;
        else if (!strcmp(err, "AvatarsUnavailable")) status = 503;
        reply_error(req, status, err);
        return;
    }
    Picture *p = calloc(1, sizeof(*p));
    if (!p) {
        avatar_image_free(&img);
        reply_error(req, 500, "internal");
        return;
    }
    p->img = img;
    uint8_t digest[KN_SHA256_LEN];
    kn_sha256(img.webp, img.webp_len, digest);
    kn_hex_encode(digest, sizeof(digest), p->sha256, sizeof(p->sha256));
    const size_t cap = 4 * ((img.webp_len + 2) / 3) + 1;
    p->b64 = malloc(cap);
    if (!p->b64 || kn_base64_encode(img.webp, img.webp_len, p->b64, cap, 0, 1) < 0) {
        picture_free(p);
        reply_error(req, 500, "internal");
        return;
    }
    store_pending(req, id, p);
}

static void h_upload(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready() || !avatar_image_available()) return reply_error(req, 503, "AvatarsUnavailable");
    if (!ai_moderation_available()) return reply_error(req, 503, "ModerationUnavailable"); /* ai.enabled + the moderation profile */
    cJSON *body = kn_http_body_json(req);
    char token[512];
    token_of(req, body, token, sizeof(token));
    const char *b64 = kn_json_str(body, "image", "");
    if (!token[0]) {
        cJSON_Delete(body);
        return reply_error(req, 401, "LoginRequired");
    }
    if (!b64[0]) {
        cJSON_Delete(body);
        return reply_error(req, 400, "BadRequest");
    }
    if (strlen(b64) > AVATAR_MAX_BYTES / 3 * 4 + 8) {
        cJSON_Delete(body);
        return reply_error(req, 413, "FileTooLarge");
    }
    Upload *u = calloc(1, sizeof(*u));
    char *copy = u ? strdup(b64) : NULL;
    cJSON_Delete(body);
    if (!u || !copy) {
        free(copy);
        free(u);
        return reply_error(req, 500, "internal");
    }
    u->b64 = copy;
    with_account(req, token, upload_with_account, u);
}

/* ---- GET / DELETE /api/account/avatar */

typedef struct {
    kn_http_req *req;
    int64_t id;
} Owned;

static void on_status_row(kn_db_result *res, void *ud) {
    Owned *o = ud;
    kn_http_req *req = o->req;
    const int64_t id = o->id;
    free(o);
    if (!kn_db_ok(res)) return reply_error(req, 500, "internal");
    cJSON *row = row_json(res);
    const char *status = row ? kn_json_str(row, "status", "none") : "none";
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status", status);
    const int shown = !strcmp(status, "approved");
    cJSON_AddBoolToObject(data, "shown", shown);
    if (row) {
        cJSON_AddStringToObject(data, "reason", kn_json_str(row, "reason", ""));
        cJSON_AddNumberToObject(data, "bytes", (double) kn_json_int(row, "bytes", 0));
        cJSON_AddNumberToObject(data, "width", (double) kn_json_int(row, "width", 0));
        cJSON_AddNumberToObject(data, "height", (double) kn_json_int(row, "height", 0));
        cJSON_AddStringToObject(data, "model", kn_json_str(row, "model", ""));
        cJSON_AddNumberToObject(data, "uploaded_ms", (double) kn_json_int(row, "uploaded_ms", 0));
        cJSON_AddNumberToObject(data, "moderated_ms", (double) kn_json_int(row, "moderated_ms", 0));
        cJSON *cats = cJSON_AddArrayToObject(data, "categories");
        char list[256];
        snprintf(list, sizeof(list), "%s", kn_json_str(row, "categories", ""));
        for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) cJSON_AddItemToArray(cats, cJSON_CreateString(tok));
        if (shown) {
            char url[160];
            snprintf(url, sizeof(url), "/api/avatars/%lld?v=%.64s", (long long) id, kn_json_str(row, "sha256", ""));
            cJSON_AddStringToObject(data, "image_url", url);
        }
    }
    cJSON_Delete(row);
    reply_data(req, 200, data);
}

static void status_with_account(kn_http_req *req, const int64_t id, void *ctx) {
    (void) ctx;
    Owned *o = calloc(1, sizeof(*o));
    if (!o) return reply_error(req, 500, "internal");
    o->req = req;
    o->id = id;
    char idtxt[24];
    snprintf(idtxt, sizeof(idtxt), "%lld", (long long) id);
    kn_db_call(A.b->db, &P_AVATAR_GET, KN_DB_ARGS(idtxt), on_status_row, o);
}

static void h_status(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "AvatarsUnavailable");
    char token[512];
    token_of(req, NULL, token, sizeof(token));
    if (!token[0]) return reply_error(req, 401, "LoginRequired");
    with_account(req, token, status_with_account, NULL);
}

static void on_delete_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) return reply_error(req, 500, "internal");
    cJSON *row = row_json(res);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "deleted", (double) kn_json_int(row, "n", 0));
    cJSON_Delete(row);
    reply_data(req, 200, data);
}

static void delete_with_account(kn_http_req *req, const int64_t id, void *ctx) {
    (void) ctx;
    char idtxt[24];
    snprintf(idtxt, sizeof(idtxt), "%lld", (long long) id);
    kn_db_call(A.b->db, &P_AVATAR_DELETE, KN_DB_ARGS(idtxt), on_delete_done, req);
}

static void h_delete(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "AvatarsUnavailable");
    char token[512];
    token_of(req, NULL, token, sizeof(token));
    if (!token[0]) return reply_error(req, 401, "LoginRequired");
    with_account(req, token, delete_with_account, NULL);
}

/* ---- GET /api/avatars/:id: an approved picture only */

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
    uint8_t *bytes = malloc(AVATAR_MAX_BYTES);
    const int n = bytes ? kn_base64_decode(b64, strlen(b64), bytes, AVATAR_MAX_BYTES) : -1;
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
    kn_db_call(A.b->db, &P_AVATAR_PUBLIC, KN_DB_ARGS(idtxt), on_public_row, req);
}

/* ---- module */

int avatar_init(Brain *b) {
    memset(&A, 0, sizeof(A));
    A.b = b;
    A.enabled = kn_config_bool(b->cfg, "avatar.enabled", 1);
    snprintf(A.model, sizeof(A.model), "%s", kn_config_str(b->cfg, "avatar.moderation_model", "gemini-3.8-flash-high"));
    A.timeout_ms = (int) kn_config_int(b->cfg, "avatar.moderation_timeout_ms", 60000);
    A.per_hour = (int) kn_config_int(b->cfg, "avatar.uploads_per_hour", 5);
    if (A.timeout_ms < 5000) A.timeout_ms = 5000;
    if (A.per_hour < 1) A.per_hour = 1;
    kn_http_route(b->http, "POST", "/api/account/avatar", h_upload, NULL);
    kn_http_route(b->http, "GET", "/api/account/avatar", h_status, NULL);
    kn_http_route(b->http, "DELETE", "/api/account/avatar", h_delete, NULL);
    kn_http_route(b->http, "GET", "/api/avatars/:id", h_public, NULL);
    KN_INFO(MOD, "profile pictures: /api/account/avatar, /api/avatars/:id (%s, review model %s)",
            avatar_image_available() ? (A.enabled ? "on" : "off") : "unavailable: built without WebP", A.model);
    return 0;
}

void avatar_shutdown(Brain *b) {
    (void) b;
    A.enabled = 0;
}
