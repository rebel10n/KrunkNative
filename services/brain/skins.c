/*
 * services/brain/skins.c - Krunker v10.0.7's Skin Manager records (custom skins, migration 0394_custom_skins).
 *
 * v10.0.7's client (gapi skins.*, deob-10.0.7.js:26551-26604; its service :124350-124511, the window :124611-127559):
 *
 *   GET    /skins?page&page_size[&type][&weapon][&search]   skins.list       {skins, total}
 *   GET    /skins/<id>                                      skins.get        {skin}
 *   POST   /skins {name, type, weapon, rarity, itemConfig}  skins.create     {skin}
 *   PUT    /skins/<id> {...}                                skins.update     {skin}
 *   DELETE /skins/<id>                                      skins.delete     {deleted, skinId}
 *   POST   /skins/<id>/duplicate                            skins.duplicate  {skin}
 *   POST   /skins/<id>/upload (multipart)                   skins.uploadAsset: refused, Box Office has no store for
 *                                                           skin files (models, textures, sounds, thumbnails)
 *
 * A skin is {skinId, name, type, weapon, rarity, itemConfig, created_at, updated_at} (the fields the window reads:
 * :124947-124983 the list row, :124794-124803 the editor's copy, :124651-124663 the save). Every route is the
 * signed-in account's own skins: another account's skin answers 404 like a missing one.
 *
 * Rules v10.0.7 leaves to its server, so Box Office's: a name is 1-64 characters (trimmed), type 0-18 (the General
 * tab's list), weapon 0-64, rarity 0-7 (store.rarities), itemConfig a JSON object of at most 32 KB; up to 200 skins
 * per account; a page is 1-50 rows (the window asks 10), newest first; search matches the name (case-insensitive)
 * or the id. A duplicate is named "<name> (Copy)".
 *
 * Two transports, one proc: the social socket's gapi tunnel (skins_gapi, social_gapi.c), which the native client
 * uses, and HTTP on the brain root with a Bearer session (v10_catalog.c).
 */
#include "acct_internal.h"
#include "db_procs.h"
#include "skins.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hot.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

#define SKIN_NAME_MAX 64
#define SKIN_TYPE_MAX 18
#define SKIN_WEAPON_MAX 64
#define SKIN_RARITY_MAX 7
#define SKIN_CONFIG_MAX 32768
#define SKINS_PER_ACCOUNT 200
#define SKINS_PAGE_MAX 50

static const struct {
    const char *code;
    int status;
    const char *message;
} k_errors[] = {
    {"SKIN_NOT_FOUND", 404, "Skin not found"},
    {"SKIN_NAME_INVALID", 400, "Skin names are 1 to 64 characters"},
    {"SKIN_TYPE_INVALID", 400, "Unknown skin type, weapon or rarity"},
    {"SKIN_CONFIG_INVALID", 400, "The item configuration must be a JSON object of at most 32 KB"},
    {"SKIN_LIMIT", 400, "You can keep up to 200 skins"},
};

int skins_error(const char *code, const char **message) {
    for (size_t i = 0; i < sizeof k_errors / sizeof k_errors[0]; i++)
        if (code && !strcmp(code, k_errors[i].code)) {
            if (message) *message = k_errors[i].message;
            return k_errors[i].status;
        }
    if (message) *message = code ? code : "Server error";
    return 500;
}

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }
static const char *nz(const char *s) { return s ? s : ""; }

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) && v->valuestring ? strtod(v->valuestring, NULL) : 0.0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static kn_db_result *refuse(Dbx *x, const char *code) { return dbx_finish(x, kn_db_result_user_error(code)); }

/* epoch ms -> "2026-11-23T12:00:00Z" */
static cJSON *iso(int64_t ms) {
    if (ms <= 0) return cJSON_CreateNull();
    const time_t t = (time_t) (ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return cJSON_CreateString(buf);
}

#define SKIN_COLS "id AS id__i, name AS name__t, type AS type__i, weapon AS weapon__i, rarity AS rarity__i," \
                  " item_config AS cfg__t, {epoch_ms:created_at} AS created__i, {epoch_ms:updated_at} AS updated__i"

/* a row (SKIN_COLS) -> the skin v10's window reads */
static cJSON *skin_json(const cJSON *r) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "skinId", jnum(r, "id"));
    cJSON_AddStringToObject(o, "name", jstr(r, "name"));
    cJSON_AddNumberToObject(o, "type", jnum(r, "type"));
    cJSON_AddNumberToObject(o, "weapon", jnum(r, "weapon"));
    cJSON_AddNumberToObject(o, "rarity", jnum(r, "rarity"));
    cJSON *cfg = cJSON_Parse(jstr(r, "cfg"));
    cJSON_AddItemToObject(o, "itemConfig", cJSON_IsObject(cfg) ? cfg : (cJSON_Delete(cfg), cJSON_CreateObject()));
    cJSON_AddItemToObject(o, "created_at", iso((int64_t) jnum(r, "created")));
    cJSON_AddItemToObject(o, "updated_at", iso((int64_t) jnum(r, "updated")));
    return o;
}

/* the actor's skin (NULL: none, or another account's) */
static cJSON *own_row(Dbx *x, int64_t actor, int64_t id) {
    if (id <= 0) return NULL;
    return dbx_row(x, SQL(.any = "SELECT " SKIN_COLS " FROM custom_skins WHERE id = {int:$1} AND owner_id = {int:$2}"),
                   KN_DB_ARGS(I64S(id), I64S(actor)));
}

static kn_db_result *answer_skin(Dbx *x, int64_t actor, int64_t id) {
    cJSON *r = own_row(x, actor, id);
    if (!r) return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "SKIN_NOT_FOUND");
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "skin", skin_json(r));
    cJSON_Delete(r);
    return dbx_json(x, "skin", d);
}

/* The body's fields, checked. Missing fields keep `base`'s (an update) or the editor's defaults (a create:
 * "New Skin", type 0, weapon 1, rarity 0, {}: :124851-124858). 0 + *code on a refusal. */
typedef struct SkinIn {
    char name[SKIN_NAME_MAX * 4 + 1];
    int64_t type, weapon, rarity;
    char *cfg; /* malloc'd JSON text */
} SkinIn;

static int field_int(const cJSON *body, const char *key, int64_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!v || cJSON_IsNull(v)) return 1;
    if (!cJSON_IsNumber(v) || v->valuedouble != (double) (int64_t) v->valuedouble) return 0;
    *out = (int64_t) v->valuedouble;
    return 1;
}

static size_t utf8_chars(const char *s) {
    size_t n = 0;
    for (; *s; s++) n += ((unsigned char) *s & 0xC0) != 0x80;
    return n;
}

static int skin_in(const char *body_text, const cJSON *base, SkinIn *in, const char **code) {
    memset(in, 0, sizeof *in);
    snprintf(in->name, sizeof in->name, "%s", base ? jstr(base, "name") : "New Skin");
    in->type = base ? (int64_t) jnum(base, "type") : 0;
    in->weapon = base ? (int64_t) jnum(base, "weapon") : 1;
    in->rarity = base ? (int64_t) jnum(base, "rarity") : 0;
    cJSON *body = cJSON_Parse(nz(body_text));
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        *code = "SKIN_CONFIG_INVALID";
        return 0;
    }
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(body, "name");
    if (nm && !cJSON_IsNull(nm)) {
        if (!cJSON_IsString(nm) || !nm->valuestring) {
            cJSON_Delete(body);
            *code = "SKIN_NAME_INVALID";
            return 0;
        }
        const char *s = nm->valuestring;
        while (isspace((unsigned char) *s)) s++;
        size_t len = strlen(s);
        while (len > 0 && isspace((unsigned char) s[len - 1])) len--;
        char trimmed[sizeof in->name];
        if (len >= sizeof trimmed) len = sizeof trimmed - 1;
        memcpy(trimmed, s, len);
        trimmed[len] = 0;
        if (!len || utf8_chars(trimmed) > SKIN_NAME_MAX || strlen(s) >= sizeof in->name) {
            cJSON_Delete(body);
            *code = "SKIN_NAME_INVALID";
            return 0;
        }
        snprintf(in->name, sizeof in->name, "%s", trimmed);
    }
    if (!field_int(body, "type", &in->type) || !field_int(body, "weapon", &in->weapon) ||
        !field_int(body, "rarity", &in->rarity) || in->type < 0 || in->type > SKIN_TYPE_MAX || in->weapon < 0 ||
        in->weapon > SKIN_WEAPON_MAX || in->rarity < 0 || in->rarity > SKIN_RARITY_MAX) {
        cJSON_Delete(body);
        *code = "SKIN_TYPE_INVALID";
        return 0;
    }
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(body, "itemConfig");
    if (cfg && !cJSON_IsNull(cfg)) {
        if (!cJSON_IsObject(cfg)) {
            cJSON_Delete(body);
            *code = "SKIN_CONFIG_INVALID";
            return 0;
        }
        in->cfg = cJSON_PrintUnformatted(cfg);
    } else {
        in->cfg = strdup(base ? (jstr(base, "cfg")[0] ? jstr(base, "cfg") : "{}") : "{}");
    }
    cJSON_Delete(body);
    if (!in->cfg || strlen(in->cfg) > SKIN_CONFIG_MAX) {
        free(in->cfg);
        in->cfg = NULL;
        *code = "SKIN_CONFIG_INVALID";
        return 0;
    }
    return 1;
}

static int64_t owned_count(Dbx *x, int64_t actor) {
    return dbx_int(x, SQL(.any = "SELECT count(*) FROM custom_skins WHERE owner_id = {int:$1}"), KN_DB_ARGS(I64S(actor)), 0);
}

static int64_t insert_skin(Dbx *x, int64_t actor, const SkinIn *in) {
    return dbx_insert_id(x,
                         SQL(.pg = "INSERT INTO custom_skins (owner_id, name, type, weapon, rarity, item_config)"
                                   " VALUES ($1::int, $2, $3::int, $4::int, $5::int, $6) RETURNING id",
                             .sqlite = "INSERT INTO custom_skins (owner_id, name, type, weapon, rarity, item_config)"
                                       " VALUES ({int:$1}, $2, {int:$3}, {int:$4}, {int:$5}, $6) RETURNING id",
                             .any = "INSERT INTO custom_skins (owner_id, name, type, weapon, rarity, item_config)"
                                    " VALUES ({int:$1}, $2, {int:$3}, {int:$4}, {int:$5}, $6)"),
                         KN_DB_ARGS(I64S(actor), in->name, I64S(in->type), I64S(in->weapon), I64S(in->rarity), in->cfg));
}

/* ------------------------------------------------------------------ the ops */

/* "%" and "_" are literal in a search ("!" escapes them) */
static void like_pattern(const char *s, char *out, size_t cap) {
    size_t k = 0;
    if (cap < 3) return;
    out[k++] = '%';
    for (; *s && k + 3 < cap; s++) {
        if (*s == '%' || *s == '_' || *s == '!') out[k++] = '!';
        out[k++] = *s;
    }
    out[k++] = '%';
    out[k] = 0;
}

static kn_db_result *op_list(Dbx *x, int64_t actor, int64_t page, int64_t size, const char *type_s, const char *weapon_s,
                             const char *search) {
    if (page < 1) page = 1;
    if (size < 1) size = 10;
    if (size > SKINS_PAGE_MAX) size = SKINS_PAGE_MAX;
    const int64_t type = type_s && *type_s ? pint(type_s) : -1, weapon = weapon_s && *weapon_s ? pint(weapon_s) : -1;
    char term[192] = "", pattern[400] = "";
    const char *s = nz(search);
    while (isspace((unsigned char) *s)) s++;
    snprintf(term, sizeof term, "%s", s);
    for (size_t len = strlen(term); len > 0 && isspace((unsigned char) term[len - 1]);) term[--len] = 0;
    like_pattern(term, pattern, sizeof pattern);
    int64_t id = -1;
    if (term[0]) {
        const char *t = term[0] == '#' ? term + 1 : term;
        char *end = NULL;
        const long long v = strtoll(t, &end, 10);
        if (*t && end && !*end && v > 0) id = v;
    }
#define LIST_WHERE " FROM custom_skins WHERE owner_id = {int:$1} AND ({int:$2} < 0 OR type = {int:$2})" \
                   " AND ({int:$3} < 0 OR weapon = {int:$3}) AND ($4 = '' OR lower(name) {ilike} lower($5) ESCAPE '!'" \
                   " OR id = {int:$6})"
    const char *offset = I64S((page - 1) * size);
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT " SKIN_COLS LIST_WHERE " ORDER BY id DESC LIMIT $7 OFFSET $8"),
                           KN_DB_ARGS(I64S(actor), I64S(type), I64S(weapon), term, pattern, I64S(id), I64S(size), offset));
    const int64_t total = dbx_int(x, SQL(.any = "SELECT count(*)" LIST_WHERE),
                                  KN_DB_ARGS(I64S(actor), I64S(type), I64S(weapon), term, pattern, I64S(id)), 0);
#undef LIST_WHERE
    if (!rows) return dbx_finish(x, NULL);
    cJSON *skins = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddItemToArray(skins, skin_json(r));
    cJSON_Delete(rows);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "skins", skins);
    cJSON_AddNumberToObject(d, "total", (double) total);
    cJSON_AddNumberToObject(d, "page", (double) page);
    cJSON_AddNumberToObject(d, "page_size", (double) size);
    return dbx_json(x, "skins", d);
}

static kn_db_result *op_create(Dbx *x, int64_t actor, const char *body) {
    SkinIn in;
    const char *code = NULL;
    if (!skin_in(body, NULL, &in, &code)) return refuse(x, code);
    if (owned_count(x, actor) >= SKINS_PER_ACCOUNT) {
        free(in.cfg);
        return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "SKIN_LIMIT");
    }
    const int64_t id = insert_skin(x, actor, &in);
    free(in.cfg);
    return answer_skin(x, actor, id);
}

static kn_db_result *op_update(Dbx *x, int64_t actor, int64_t id, const char *body) {
    cJSON *base = own_row(x, actor, id);
    if (!base) return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "SKIN_NOT_FOUND");
    SkinIn in;
    const char *code = NULL;
    const int ok = skin_in(body, base, &in, &code);
    cJSON_Delete(base);
    if (!ok) return refuse(x, code);
    dbx_exec(x, SQL(.any = "UPDATE custom_skins SET name = $1, type = {int:$2}, weapon = {int:$3}, rarity = {int:$4},"
                           " item_config = $5, updated_at = {now} WHERE id = {int:$6} AND owner_id = {int:$7}"),
             KN_DB_ARGS(in.name, I64S(in.type), I64S(in.weapon), I64S(in.rarity), in.cfg, I64S(id), I64S(actor)));
    free(in.cfg);
    return answer_skin(x, actor, id);
}

static kn_db_result *op_delete(Dbx *x, int64_t actor, int64_t id) {
    if (id <= 0) return refuse(x, "SKIN_NOT_FOUND");
    const int64_t gone = dbx_exec(x, SQL(.any = "DELETE FROM custom_skins WHERE id = {int:$1} AND owner_id = {int:$2}"),
                                  KN_DB_ARGS(I64S(id), I64S(actor)));
    if (gone < 0) return dbx_finish(x, NULL);
    if (gone == 0) return refuse(x, "SKIN_NOT_FOUND");
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "deleted", 1);
    cJSON_AddNumberToObject(d, "skinId", (double) id);
    return dbx_json(x, "deleted", d);
}

static kn_db_result *op_duplicate(Dbx *x, int64_t actor, int64_t id) {
    cJSON *base = own_row(x, actor, id);
    if (!base) return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "SKIN_NOT_FOUND");
    if (owned_count(x, actor) >= SKINS_PER_ACCOUNT) {
        cJSON_Delete(base);
        return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "SKIN_LIMIT");
    }
    SkinIn in;
    memset(&in, 0, sizeof in);
    /* "<name> (Copy)", cut to the name limit on a character boundary */
    char name[sizeof in.name - 8]; // room for " (Copy)"
    snprintf(name, sizeof name, "%s", jstr(base, "name"));
    while (utf8_chars(name) > SKIN_NAME_MAX - 7) {
        size_t len = strlen(name);
        do len--; while (len > 0 && ((unsigned char) name[len] & 0xC0) == 0x80);
        name[len] = 0;
    }
    snprintf(in.name, sizeof in.name, "%s (Copy)", name);
    in.type = (int64_t) jnum(base, "type");
    in.weapon = (int64_t) jnum(base, "weapon");
    in.rarity = (int64_t) jnum(base, "rarity");
    in.cfg = strdup(jstr(base, "cfg")[0] ? jstr(base, "cfg") : "{}");
    cJSON_Delete(base);
    const int64_t copy = in.cfg ? insert_skin(x, actor, &in) : 0;
    free(in.cfg);
    return answer_skin(x, actor, copy);
}

static kn_db_result *run_custom_skin(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *op = nz(P(0));
    const int64_t actor = PI(1);
    if (actor <= 0) return refuse(&x, "SKIN_NOT_FOUND");
    if (!strcmp(op, "list")) return op_list(&x, actor, PI(2), PI(3), P(4), P(5), P(6));
    if (!strcmp(op, "create")) return op_create(&x, actor, P(2));
    if (!strcmp(op, "get")) return answer_skin(&x, actor, PI(2));
    if (!strcmp(op, "update")) return op_update(&x, actor, PI(2), P(3));
    if (!strcmp(op, "delete")) return op_delete(&x, actor, PI(2));
    if (!strcmp(op, "duplicate")) return op_duplicate(&x, actor, PI(2));
    return refuse(&x, "SKIN_NOT_FOUND");
}
const kn_proc P_CUSTOM_SKIN = {"custom_skin", NULL, run_custom_skin};

/* ------------------------------------------------------------------ the gapi tunnel (social socket) */

/* per-account write limiter (60 writes / minute), shared with Redis when it is up (as studios.c) */
static int write_ok(int64_t account) {
    static struct {
        int64_t id;
        uint64_t t0;
        int n;
    } slots[128];
    const uint64_t now = kn_now_ms();
    int local = 1;
    const size_t i = (size_t) (account % 128);
    if (slots[i].id != account || now - slots[i].t0 > 60000) {
        slots[i].id = account;
        slots[i].t0 = now;
        slots[i].n = 0;
    }
    if (++slots[i].n > 60) local = 0;
    char key[24];
    snprintf(key, sizeof key, "%lld", (long long) account);
    return hot_limit("skins", key, 60, 60, local);
}

static void done(SocialSession *s, kn_db_result *res, void *extra) {
    const int64_t rid = *(int64_t *) extra;
    if (!kn_db_ok(res)) {
        const char *code = acct_db_error(res, "skins"), *message = NULL;
        const int status = skins_error(code, &message);
        gapi_error(s, rid, status, status == 500 ? "SKIN_ERROR" : code, message);
        return;
    }
    cJSON *v = acct_db_json(res);
    gapi_reply(s, rid, v ? v : cJSON_CreateObject());
}

static void call(SocialSession *s, const GapiReq *r, const char *op, const char *a, const char *b, const char *c,
                 const char *d, const char *e) {
    int64_t *rid = malloc(sizeof *rid);
    if (!rid) return;
    *rid = r->rid;
    char me[24];
    social_viewer(s, me);
    social_call(s, &P_CUSTOM_SKIN, KN_DB_ARGS(op, me, nz(a), nz(b), nz(c), nz(d), nz(e)), done, rid);
}

static int is_id(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

int skins_gapi(SocialSession *s, const GapiReq *r) {
    if (r->nseg < 1 || strcmp(r->seg[0], "skins")) return 0;
    if (s->account_id <= 0) {
        gapi_error(s, r->rid, 401, "UNAUTHORIZED", "You must be signed in.");
        return 1;
    }
    if (strcmp(r->method, "GET") && !write_ok(s->account_id)) {
        gapi_error(s, r->rid, 429, "RATE_LIMITED", "Rate limit exceeded. Try again later.");
        return 1;
    }
    if (gapi_is(r, "GET", "/skins")) {
        char page[16] = "", size[16] = "", type[16] = "", weapon[16] = "", search[192] = "";
        gapi_query(r, "page", page, sizeof page);
        gapi_query(r, "page_size", size, sizeof size);
        gapi_query(r, "type", type, sizeof type);
        gapi_query(r, "weapon", weapon, sizeof weapon);
        gapi_query(r, "search", search, sizeof search);
        call(s, r, "list", page, size, type, weapon, search);
        return 1;
    }
    if (gapi_is(r, "POST", "/skins")) {
        char *body = r->body ? cJSON_PrintUnformatted(r->body) : NULL;
        call(s, r, "create", body ? body : "{}", NULL, NULL, NULL, NULL);
        free(body);
        return 1;
    }
    if (r->nseg >= 2 && !is_id(r->seg[1])) {
        gapi_error(s, r->rid, 404, "SKIN_NOT_FOUND", "Skin not found");
    } else if (gapi_is(r, "GET", "/skins/:id")) {
        call(s, r, "get", r->seg[1], NULL, NULL, NULL, NULL);
    } else if (gapi_is(r, "PUT", "/skins/:id")) {
        char *body = r->body ? cJSON_PrintUnformatted(r->body) : NULL;
        call(s, r, "update", r->seg[1], body ? body : "{}", NULL, NULL, NULL);
        free(body);
    } else if (gapi_is(r, "DELETE", "/skins/:id")) {
        call(s, r, "delete", r->seg[1], NULL, NULL, NULL, NULL);
    } else if (gapi_is(r, "POST", "/skins/:id/duplicate")) {
        call(s, r, "duplicate", r->seg[1], NULL, NULL, NULL, NULL);
    } else if (gapi_is(r, "POST", "/skins/:id/upload")) {
        gapi_error(s, r->rid, 403, "SKIN_UPLOAD_REFUSED", "Box Office has no asset store for skin files");
    } else {
        gapi_error(s, r->rid, 404, "NOT_FOUND", "Not found.");
    }
    return 1;
}
