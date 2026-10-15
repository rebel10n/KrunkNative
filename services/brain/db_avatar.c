/*
 * services/brain/db_avatar.c - profile pictures on account_avatars (migration 0123). C on every
 * driver (the SQL is written for all three dialects through SQL(...) and the Dbx helpers).
 *
 *   P_AVATAR_PUT     (json {id, sha256, bytes, width, height}) -> {}: a new upload: status pending,
 *                    no image, the account's previous picture is replaced. "Account not found"
 *   P_AVATAR_VERDICT (json {id, sha256, status, reason, categories, model, image?}) -> {n}: the
 *                    review of the picture with this sha256 (n 0 when a newer upload replaced it).
 *                    image is stored only with status approved
 *   P_AVATAR_GET     (id) -> {status, sha256, bytes, width, height, reason, categories, model,
 *                    uploaded_ms, moderated_ms} for the owner (never the image), or {}
 *   P_AVATAR_PUBLIC  (id) -> {sha256, image} when the picture is approved, else {}
 *   P_AVATAR_DELETE  (id) -> {n}: the owner removed the picture
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }
static const char *nz(const char *s) { return s ? s : ""; }

/* both NULL-safe: a missing or unparsable body reads as empty fields */
static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static int64_t jint(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : 0;
}

static kn_db_result *run_avatar_put(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = cJSON_Parse(nz(P(0)));
    const int64_t id = jint(in, "id");
    if (!cJSON_IsObject(in) || id <= 0) {
        cJSON_Delete(in);
        dbx_refuse(&x, "BadRequest");
        return dbx_finish(&x, NULL);
    }
    const char *sha = jstr(in, "sha256");
    if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM accounts WHERE id = {int:$1}"), KN_DB_ARGS(I64S(id)))) {
        cJSON_Delete(in);
        if (!dbx_failed(&x)) dbx_refuse(&x, "Account not found");
        return dbx_finish(&x, NULL);
    }
    const int64_t upd = dbx_exec(
        &x,
        SQL(.any = "UPDATE account_avatars SET status = 'pending', sha256 = $2, image = NULL, bytes = {int:$3}, width = {int:$4},"
                   " height = {int:$5}, reason = '', categories = '', model = '', uploaded_at = {now}, moderated_at = NULL"
                   " WHERE account_id = {int:$1}"),
        KN_DB_ARGS(I64S(id), sha, I64S(jint(in, "bytes")), I64S(jint(in, "width")), I64S(jint(in, "height"))));
    if (upd == 0 && !dbx_failed(&x))
        dbx_exec(&x,
                 SQL(.any = "INSERT INTO account_avatars (account_id, status, sha256, bytes, width, height, uploaded_at)"
                            " VALUES ({int:$1}, 'pending', $2, {int:$3}, {int:$4}, {int:$5}, {now})"),
                 KN_DB_ARGS(I64S(id), sha, I64S(jint(in, "bytes")), I64S(jint(in, "width")), I64S(jint(in, "height"))));
    cJSON_Delete(in);
    return dbx_json(&x, "avatar_put", cJSON_CreateObject());
}

static kn_db_result *run_avatar_verdict(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = cJSON_Parse(nz(P(0)));
    const int64_t id = jint(in, "id");
    const char *status = jstr(in, "status");
    const char *image = jstr(in, "image");
    const int approved = !strcmp(status, "approved");
    const int64_t upd = id > 0 && (approved || !strcmp(status, "rejected") || !strcmp(status, "error"))
        ? dbx_exec(&x,
                   SQL(.any = "UPDATE account_avatars SET status = $3, reason = $4, categories = $5, model = $6, moderated_at = {now},"
                              " image = $7 WHERE account_id = {int:$1} AND sha256 = $2 AND status = 'pending'"),
                   KN_DB_ARGS(I64S(id), jstr(in, "sha256"), status, jstr(in, "reason"), jstr(in, "categories"), jstr(in, "model"),
                              approved && image[0] ? image : NULL))
        : 0;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", (double) (upd > 0 ? upd : 0));
    cJSON_Delete(in);
    return dbx_json(&x, "avatar_verdict", o);
}

static kn_db_result *run_avatar_get(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(
        &x,
        SQL(.any = "SELECT status, sha256, bytes, width, height, reason, categories, model,"
                   " {epoch_ms:uploaded_at} AS uploaded_ms, {epoch_ms:moderated_at} AS moderated_ms"
                   " FROM account_avatars WHERE account_id = {int:$1}"),
        KN_DB_ARGS(I64S(pint(P(0)))));
    return dbx_json(&x, "avatar_get", row ? row : cJSON_CreateObject());
}

static kn_db_result *run_avatar_public(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(
        &x,
        SQL(.any = "SELECT sha256, image FROM account_avatars WHERE account_id = {int:$1} AND status = 'approved'"
                   " AND image IS NOT NULL"),
        KN_DB_ARGS(I64S(pint(P(0)))));
    return dbx_json(&x, "avatar_public", row ? row : cJSON_CreateObject());
}

static kn_db_result *run_avatar_delete(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t del = dbx_exec(&x, SQL(.any = "DELETE FROM account_avatars WHERE account_id = {int:$1}"),
                                 KN_DB_ARGS(I64S(pint(P(0)))));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", (double) (del > 0 ? del : 0));
    return dbx_json(&x, "avatar_delete", o);
}

const kn_proc P_AVATAR_PUT = {"avatar_put", NULL, run_avatar_put};
const kn_proc P_AVATAR_VERDICT = {"avatar_verdict", NULL, run_avatar_verdict};
const kn_proc P_AVATAR_GET = {"avatar_get", NULL, run_avatar_get};
const kn_proc P_AVATAR_PUBLIC = {"avatar_public", NULL, run_avatar_public};
const kn_proc P_AVATAR_DELETE = {"avatar_delete", NULL, run_avatar_delete};
