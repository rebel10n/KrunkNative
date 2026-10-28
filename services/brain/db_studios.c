/*
 * services/brain/db_studios.c - Krunker v10.0.2's Studios (migration 0330_studios): the one proc
 * P_STUDIO behind every /studios route of studios.c. C on every driver, run inside a transaction.
 *
 *   P_STUDIO (op, actor, a, b, c) -> one JSON column: the answer's data
 *
 * Refusals are kn_db_result_user_error(code) with a STUDIO_* / MONETIZATION_* code; studios.c turns
 * the code into v10's {error: {code, message}} with its HTTP status. The actor is always the
 * session's own account (never a client-supplied id). Shapes are the fields v10's Studios window
 * (v10.js 138200-138440) and studio view (137011-137610) read; a few KrunkNative extras ride along
 * (member creator_badge, map map_videolink: the creator data of 0210_creator_badge / map_video).
 *
 *   mine                         -> [{id, name, owner, member_count, role, monetization}]
 *   create (name)                -> {id, name}
 *   get (studio)                 -> {id, name, owner, my_role, monetization, members [{name, role,
 *                                   creator_badge}], invites [{invite_id, name, invited_by,
 *                                   expires_at}], maps [{map_id, map_name, map_videolink}],
 *                                   currencies [{currency_id, name, symbol, decimals, active}]}
 *   delete (studio)              -> {}                        owner
 *   invite (studio, name)        -> {invite_id, name, expires_at}   owner; 20 members + invites
 *   invite_cancel (studio, id)   -> {}                        owner
 *   invites                      -> [{invite_id, studio_id, studio_name, invited_by, expires_at}]
 *   invite_answer (id, 1|0)      -> {studio_id, studio_name, accepted}
 *   transfer (studio, name)      -> {owner}                   owner; the old owner stays a member
 *   member_remove (studio, name) -> {}                        owner, or a member leaving
 *   map_add (studio, map)        -> {map_id, map_name}        owner; the owner's own published map
 *   map_remove (studio, map)     -> {}                        owner
 *   currency_create (studio, {name, symbol, decimals}) -> the currency          owner
 *   currency_active (studio, currency, 1|0)            -> {currency_id, active} owner
 *   analytics (studio)           -> {kr_earned, kr_charged, currencies [{currency_id, name, symbol,
 *                                   circulating, holders}]}
 *   skins (studio)               -> []
 *   monetization (studio)        -> the status (below)
 *   monetization_apply (studio, {description, map_ids})  -> the status   owner, can_apply
 *   monetization_cancel (studio) -> the status                           owner, a pending request
 *
 * Monetization status: {enabled, blocked, eligible, can_apply, next_apply_at, request: null |
 * {id, status, description, map_ids, review_note, created_at, reviewed_at}, requirements: [{key,
 * ok, required?, value?}]} for account_age (90 days), level (20), twofa, no_recent_ban (180 days)
 * and has_map (a published game in the studio), all of the studio owner. Times are ISO 8601 UTC.
 *
 * Deviations from v10 (KrunkNative takes no payments and has no Skin Manager): an application
 * stays pending until a developer reviews it in the database; currency balances are 0 (there are
 * no PAYMENTS currency balances); skins is always empty and studios.c refuses imports (no custom
 * skins exist). KR charged / earned are the studio's games' consented charges (0358_studio_charges).
 *
 * The game servers' side (P_STUDIO_GAME, P_STUDIO_CHARGE, at the end): whether a hosted published
 * map is monetized, and the KR of PAYMENTS.charge / transact after the player's consent.
 */
#include "db_procs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

/* v10's limits (the window's hints: studios.create.hint, members.hint, currencies.hint) */
#define STUDIO_OWNED_MAX 3
#define STUDIO_MEMBERS_MAX 20       /* counting pending invites */
#define STUDIO_CURRENCIES_MAX 5
/* monetization requirements (studios.monetization.req.*; the ban window, 180 days, is in the SQL)
 * and the wait after a rejection; invites last 14 days ({later:1209600}) */
#define MON_ACCOUNT_AGE_DAYS 90
#define MON_LEVEL 20
#define MON_REAPPLY_S 2592000       /* 30 days (v10 does not say; KrunkNative's choice) */

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

static int64_t now_ms(void) { return (int64_t) time(NULL) * 1000; }

/* epoch ms -> "2026-10-29T12:00:00Z" (v10 parses it with new Date) or JSON null for 0 */
static cJSON *iso(int64_t ms) {
    if (ms <= 0) return cJSON_CreateNull();
    const time_t t = (time_t) (ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return cJSON_CreateString(buf);
}

static kn_db_result *refuse(Dbx *x, const char *code) { return dbx_finish(x, kn_db_result_user_error(code)); }

/* studio names: 3-30 letters, digits or _ (v10 /^[a-zA-Z0-9_]{3,30}$/) */
static int name_ok(const char *s) {
    const size_t len = strlen(s);
    if (len < 3 || len > 30) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char) s[i]) && s[i] != '_') return 0;
    return 1;
}

/* the actor's role in the studio: 1 owner, 2 member, 0 not a member (or no studio) */
static int role_of(Dbx *x, int64_t studio, int64_t actor) {
    return (int) dbx_int(x, SQL(.any = "SELECT m.role FROM studio_members m JOIN studios s ON s.id = m.studio_id"
                                       " WHERE m.studio_id = {int:$1} AND m.account_id = {int:$2}"),
                         KN_DB_ARGS(I64S(studio), I64S(actor)), 0);
}

static int64_t account_by_name(Dbx *x, const char *name) {
    return dbx_int(x, SQL(.any = "SELECT id FROM accounts WHERE lower(username) = lower($1)"), KN_DB_ARGS(nz(name)), 0);
}

static int64_t owned_count(Dbx *x, int64_t account) {
    return dbx_int(x, SQL(.any = "SELECT count(*) FROM studios WHERE owner_id = {int:$1}"), KN_DB_ARGS(I64S(account)), 0);
}

static void drop_expired_invites(Dbx *x) {
    dbx_exec(x, SQL(.any = "DELETE FROM studio_invites WHERE expires_at <= {now}"), 0, NULL);
}

/* ------------------------------------------------------------------ list / create / get / delete */

static kn_db_result *op_mine(Dbx *x, int64_t actor) {
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT s.id AS id__i, s.name AS name__t, o.username AS owner__t, m.role AS role__i,"
                                         " s.monetization AS mon__i,"
                                         " (SELECT count(*) FROM studio_members c WHERE c.studio_id = s.id) AS member_count__i"
                                         " FROM studio_members m JOIN studios s ON s.id = m.studio_id"
                                         " LEFT JOIN accounts o ON o.id = s.owner_id"
                                         " WHERE m.account_id = {int:$1} ORDER BY m.role, lower(s.name)"),
                           KN_DB_ARGS(I64S(actor)));
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", jnum(r, "id"));
        cJSON_AddStringToObject(o, "name", jstr(r, "name"));
        cJSON_AddStringToObject(o, "owner", jstr(r, "owner"));
        cJSON_AddNumberToObject(o, "member_count", jnum(r, "member_count"));
        cJSON_AddNumberToObject(o, "role", jnum(r, "role"));
        cJSON_AddBoolToObject(o, "monetization", jnum(r, "mon") != 0);
        cJSON_AddItemToArray(out, o);
    }
    cJSON_Delete(rows);
    return dbx_json(x, "studios", out);
}

static kn_db_result *op_create(Dbx *x, int64_t actor, const char *name) {
    if (!name_ok(name)) return refuse(x, "STUDIO_NAME_INVALID");
    if (owned_count(x, actor) >= STUDIO_OWNED_MAX) return refuse(x, "STUDIO_LIMIT");
    if (dbx_exists(x, SQL(.any = "SELECT 1 FROM studios WHERE lower(name) = lower($1)"), KN_DB_ARGS(name))) return refuse(x, "STUDIO_NAME_TAKEN");
    const int64_t id = dbx_insert_id(x,
                                     SQL(.pg = "INSERT INTO studios (name, owner_id) VALUES ($1, $2::int) RETURNING id",
                                         .sqlite = "INSERT INTO studios (name, owner_id) VALUES ($1, {int:$2}) RETURNING id",
                                         .any = "INSERT INTO studios (name, owner_id) VALUES ($1, {int:$2})"),
                                     KN_DB_ARGS(name, I64S(actor)));
    dbx_exec(x, SQL(.any = "INSERT INTO studio_members (studio_id, account_id, role) VALUES ({int:$1}, {int:$2}, 1)"),
             KN_DB_ARGS(I64S(id), I64S(actor)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) id);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "role", 1);
    return dbx_json(x, "studio", o);
}

static kn_db_result *op_get(Dbx *x, int64_t actor, int64_t studio) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    cJSON *o = dbx_row(x, SQL(.any = "SELECT s.id AS id__i, s.name AS name__t, a.username AS owner__t, s.monetization AS mon__i,"
                                     " {epoch_ms:s.created_at} AS created__i"
                                     " FROM studios s LEFT JOIN accounts a ON a.id = s.owner_id WHERE s.id = {int:$1}"),
                       KN_DB_ARGS(I64S(studio)));
    if (!o) return dbx_finish(x, NULL);
    const int64_t created = (int64_t) jnum(o, "created");
    cJSON_DeleteItemFromObjectCaseSensitive(o, "created");
    cJSON_AddItemToObject(o, "created_at", iso(created));
    cJSON_AddBoolToObject(o, "monetization", jnum(o, "mon") != 0);
    cJSON_DeleteItemFromObjectCaseSensitive(o, "mon");
    cJSON_AddNumberToObject(o, "my_role", role);

    cJSON *members = dbx_rows(x, SQL(.any = "SELECT a.id AS player_id__i, a.username AS name__t, m.role AS role__i,"
                                            " coalesce(p.creator_badge, 0) AS creator_badge__i, {epoch_ms:m.joined_at} AS joined__i"
                                            " FROM studio_members m JOIN accounts a ON a.id = m.account_id"
                                            " LEFT JOIN players p ON p.player_id = m.account_id"
                                            " WHERE m.studio_id = {int:$1} ORDER BY m.role, m.joined_at"),
                              KN_DB_ARGS(I64S(studio)));
    cJSON *r;
    cJSON_ArrayForEach(r, members) {
        const int64_t j = (int64_t) jnum(r, "joined");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "joined");
        cJSON_AddItemToObject(r, "joined_at", iso(j));
    }
    cJSON_AddItemToObject(o, "members", members ? members : cJSON_CreateArray());

    cJSON *invites = dbx_rows(x, SQL(.any = "SELECT i.id AS invite_id__i, a.username AS name__t, b.username AS invited_by__t,"
                                            " {epoch_ms:i.expires_at} AS expires__i FROM studio_invites i"
                                            " JOIN accounts a ON a.id = i.account_id LEFT JOIN accounts b ON b.id = i.invited_by"
                                            " WHERE i.studio_id = {int:$1} AND i.expires_at > {now} ORDER BY i.id"),
                              KN_DB_ARGS(I64S(studio)));
    cJSON_ArrayForEach(r, invites) {
        const int64_t e = (int64_t) jnum(r, "expires");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "expires");
        cJSON_AddItemToObject(r, "expires_at", iso(e));
    }
    cJSON_AddItemToObject(o, "invites", invites ? invites : cJSON_CreateArray());

    cJSON *maps = dbx_rows(x, SQL(.any = "SELECT m.map_id AS map_id__i, m.map_name AS map_name__t,"
                                         " coalesce(m.map_video, '') AS map_videolink__t, a.username AS creator__t"
                                         " FROM studio_maps s JOIN published_maps m ON m.map_id = s.map_id"
                                         " LEFT JOIN accounts a ON a.id = m.creator_id"
                                         " WHERE s.studio_id = {int:$1} AND m.deleted_at IS NULL ORDER BY s.added_at, m.map_id"),
                           KN_DB_ARGS(I64S(studio)));
    cJSON_AddItemToObject(o, "maps", maps ? maps : cJSON_CreateArray());

    cJSON *cur = dbx_rows(x, SQL(.any = "SELECT id AS currency_id__i, name AS name__t, symbol AS symbol__t, decimals AS decimals__i,"
                                        " active AS active__i FROM studio_currencies WHERE studio_id = {int:$1} ORDER BY id"),
                          KN_DB_ARGS(I64S(studio)));
    cJSON_ArrayForEach(r, cur) {
        const int a = jnum(r, "active") != 0;
        cJSON_ReplaceItemInObjectCaseSensitive(r, "active", cJSON_CreateBool(a));
    }
    cJSON_AddItemToObject(o, "currencies", cur ? cur : cJSON_CreateArray());
    return dbx_json(x, "studio", o);
}

static kn_db_result *op_delete(Dbx *x, int64_t actor, int64_t studio) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    static const char *const tables[] = {"studio_monetization", "studio_currencies", "studio_maps", "studio_invites", "studio_members"};
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        char sql[96];
        snprintf(sql, sizeof sql, "DELETE FROM %s WHERE studio_id = {int:$1}", tables[i]);
        dbx_exec(x, SQL(.any = sql), KN_DB_ARGS(I64S(studio)));
    }
    dbx_exec(x, SQL(.any = "DELETE FROM studios WHERE id = {int:$1}"), KN_DB_ARGS(I64S(studio)));
    return dbx_json(x, "studio", cJSON_CreateObject());
}

/* ------------------------------------------------------------------ members and invites */

static kn_db_result *op_invite(Dbx *x, int64_t actor, int64_t studio, const char *name) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    const int64_t who = account_by_name(x, name);
    if (who <= 0) return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "STUDIO_PLAYER_NOT_FOUND");
    if (who == actor) return refuse(x, "STUDIO_INVITE_SELF");
    if (role_of(x, studio, who) > 0) return refuse(x, "STUDIO_ALREADY_MEMBER");
    drop_expired_invites(x);
    if (dbx_exists(x, SQL(.any = "SELECT 1 FROM studio_invites WHERE studio_id = {int:$1} AND account_id = {int:$2}"),
                   KN_DB_ARGS(I64S(studio), I64S(who))))
        return refuse(x, "STUDIO_ALREADY_INVITED");
    const int64_t seats = dbx_int(x, SQL(.any = "SELECT (SELECT count(*) FROM studio_members WHERE studio_id = {int:$1})"
                                                " + (SELECT count(*) FROM studio_invites WHERE studio_id = {int:$1})"),
                                  KN_DB_ARGS(I64S(studio)), 0);
    if (seats >= STUDIO_MEMBERS_MAX) return refuse(x, "STUDIO_FULL");
    const int64_t id = dbx_insert_id(
        x,
        SQL(.pg = "INSERT INTO studio_invites (studio_id, account_id, invited_by, expires_at) VALUES ($1::int, $2::int, $3::int,"
                  " now() + interval '14 days') RETURNING id",
            .sqlite = "INSERT INTO studio_invites (studio_id, account_id, invited_by, expires_at) VALUES ({int:$1}, {int:$2}, {int:$3},"
                      " {later:1209600}) RETURNING id",
            .any = "INSERT INTO studio_invites (studio_id, account_id, invited_by, expires_at) VALUES ({int:$1}, {int:$2}, {int:$3},"
                   " {later:1209600})"),
        KN_DB_ARGS(I64S(studio), I64S(who), I64S(actor)));
    const int64_t exp = dbx_int(x, SQL(.any = "SELECT {epoch_ms:expires_at} FROM studio_invites WHERE id = {int:$1}"), KN_DB_ARGS(I64S(id)), 0);
    char *real = dbx_text(x, SQL(.any = "SELECT username FROM accounts WHERE id = {int:$1}"), KN_DB_ARGS(I64S(who)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "invite_id", (double) id);
    cJSON_AddStringToObject(o, "name", real ? real : name);
    cJSON_AddItemToObject(o, "expires_at", iso(exp));
    free(real);
    return dbx_json(x, "invite", o);
}

static kn_db_result *op_invite_cancel(Dbx *x, int64_t actor, int64_t studio, int64_t invite) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    if (dbx_exec(x, SQL(.any = "DELETE FROM studio_invites WHERE id = {int:$1} AND studio_id = {int:$2}"), KN_DB_ARGS(I64S(invite), I64S(studio))) == 0)
        return refuse(x, "STUDIO_INVITE_NOT_FOUND");
    return dbx_json(x, "invite", cJSON_CreateObject());
}

static kn_db_result *op_invites(Dbx *x, int64_t actor) {
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT i.id AS invite_id__i, s.id AS studio_id__i, s.name AS studio_name__t,"
                                         " coalesce(b.username, '') AS invited_by__t, {epoch_ms:i.expires_at} AS expires__i"
                                         " FROM studio_invites i JOIN studios s ON s.id = i.studio_id"
                                         " LEFT JOIN accounts b ON b.id = i.invited_by"
                                         " WHERE i.account_id = {int:$1} AND i.expires_at > {now} ORDER BY i.id"),
                           KN_DB_ARGS(I64S(actor)));
    cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const int64_t e = (int64_t) jnum(r, "expires");
        cJSON_DeleteItemFromObjectCaseSensitive(r, "expires");
        cJSON_AddItemToObject(r, "expires_at", iso(e));
    }
    return dbx_json(x, "invites", rows ? rows : cJSON_CreateArray());
}

static kn_db_result *op_invite_answer(Dbx *x, int64_t actor, int64_t invite, int accept) {
    cJSON *inv = dbx_row(x, SQL(.any = "SELECT i.studio_id AS studio__i, s.name AS name__t FROM studio_invites i"
                                       " JOIN studios s ON s.id = i.studio_id"
                                       " WHERE i.id = {int:$1} AND i.account_id = {int:$2} AND i.expires_at > {now}"),
                         KN_DB_ARGS(I64S(invite), I64S(actor)));
    if (!inv) return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "STUDIO_INVITE_NOT_FOUND");
    const int64_t studio = (int64_t) jnum(inv, "studio");
    dbx_exec(x, SQL(.any = "DELETE FROM studio_invites WHERE id = {int:$1}"), KN_DB_ARGS(I64S(invite)));
    if (accept && role_of(x, studio, actor) <= 0) {
        /* the invite held its seat: members + other invites stay within the limit */
        dbx_exec(x, SQL(.any = "INSERT INTO studio_members (studio_id, account_id, role) VALUES ({int:$1}, {int:$2}, 2)"),
                 KN_DB_ARGS(I64S(studio), I64S(actor)));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "studio_id", (double) studio);
    cJSON_AddStringToObject(o, "studio_name", jstr(inv, "name"));
    cJSON_AddBoolToObject(o, "accepted", accept);
    cJSON_Delete(inv);
    return dbx_json(x, "invite", o);
}

static kn_db_result *op_transfer(Dbx *x, int64_t actor, int64_t studio, const char *name) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    const int64_t who = account_by_name(x, name);
    if (who <= 0 || role_of(x, studio, who) <= 0) return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "STUDIO_NOT_MEMBER");
    if (who == actor) return refuse(x, "STUDIO_TRANSFER_SELF");
    if (owned_count(x, who) >= STUDIO_OWNED_MAX) return refuse(x, "STUDIO_TRANSFER_LIMIT");
    dbx_exec(x, SQL(.any = "UPDATE studio_members SET role = 2 WHERE studio_id = {int:$1} AND account_id = {int:$2}"),
             KN_DB_ARGS(I64S(studio), I64S(actor)));
    dbx_exec(x, SQL(.any = "UPDATE studio_members SET role = 1 WHERE studio_id = {int:$1} AND account_id = {int:$2}"),
             KN_DB_ARGS(I64S(studio), I64S(who)));
    dbx_exec(x, SQL(.any = "UPDATE studios SET owner_id = {int:$2} WHERE id = {int:$1}"), KN_DB_ARGS(I64S(studio), I64S(who)));
    char *real = dbx_text(x, SQL(.any = "SELECT username FROM accounts WHERE id = {int:$1}"), KN_DB_ARGS(I64S(who)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "owner", real ? real : name);
    free(real);
    return dbx_json(x, "studio", o);
}

static kn_db_result *op_member_remove(Dbx *x, int64_t actor, int64_t studio, const char *name) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    const int64_t who = account_by_name(x, name);
    const int their = who > 0 ? role_of(x, studio, who) : 0;
    if (their <= 0) return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "STUDIO_NOT_MEMBER");
    if (who != actor && role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    if (their == 1) return refuse(x, "STUDIO_OWNER_LEAVE");
    dbx_exec(x, SQL(.any = "DELETE FROM studio_members WHERE studio_id = {int:$1} AND account_id = {int:$2}"),
             KN_DB_ARGS(I64S(studio), I64S(who)));
    return dbx_json(x, "member", cJSON_CreateObject());
}

/* ------------------------------------------------------------------ games, currencies, analytics */

static kn_db_result *op_map_add(Dbx *x, int64_t actor, int64_t studio, int64_t map) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    cJSON *m = dbx_row(x, SQL(.any = "SELECT map_name AS name__t, creator_id AS creator__i FROM published_maps"
                                     " WHERE map_id = {int:$1} AND deleted_at IS NULL"),
                       KN_DB_ARGS(I64S(map)));
    if (!m) return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "STUDIO_MAP_NOT_FOUND");
    if ((int64_t) jnum(m, "creator") != actor) {
        cJSON_Delete(m);
        return refuse(x, "STUDIO_MAP_NOT_YOURS");
    }
    if (dbx_exists(x, SQL(.any = "SELECT 1 FROM studio_maps WHERE map_id = {int:$1}"), KN_DB_ARGS(I64S(map)))) {
        cJSON_Delete(m);
        return refuse(x, "STUDIO_MAP_TAKEN");
    }
    dbx_exec(x, SQL(.any = "INSERT INTO studio_maps (map_id, studio_id, added_by) VALUES ({int:$1}, {int:$2}, {int:$3})"),
             KN_DB_ARGS(I64S(map), I64S(studio), I64S(actor)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "map_id", (double) map);
    cJSON_AddStringToObject(o, "map_name", jstr(m, "name"));
    cJSON_Delete(m);
    return dbx_json(x, "map", o);
}

static kn_db_result *op_map_remove(Dbx *x, int64_t actor, int64_t studio, int64_t map) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    if (dbx_exec(x, SQL(.any = "DELETE FROM studio_maps WHERE map_id = {int:$1} AND studio_id = {int:$2}"), KN_DB_ARGS(I64S(map), I64S(studio))) == 0)
        return refuse(x, "STUDIO_MAP_NOT_ASSIGNED");
    return dbx_json(x, "map", cJSON_CreateObject());
}

/* v10: name /^[a-zA-Z0-9 _]{3,20}$/, symbol /^[A-Z]{2,4}$/ and not KR / JNK / RP, decimals 0..2 */
static int currency_ok(const char *name, const char *symbol, int64_t decimals) {
    const size_t nl = strlen(name), sl = strlen(symbol);
    if (nl < 3 || nl > 20 || sl < 2 || sl > 4 || decimals < 0 || decimals > 2) return 0;
    for (size_t i = 0; i < nl; i++)
        if (!isalnum((unsigned char) name[i]) && name[i] != ' ' && name[i] != '_') return 0;
    for (size_t i = 0; i < sl; i++)
        if (symbol[i] < 'A' || symbol[i] > 'Z') return 0;
    return strcmp(symbol, "KR") && strcmp(symbol, "JNK") && strcmp(symbol, "RP");
}

static kn_db_result *op_currency_create(Dbx *x, int64_t actor, int64_t studio, const char *json) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    cJSON *in = cJSON_Parse(nz(json));
    char name[64], symbol[16];
    snprintf(name, sizeof name, "%s", jstr(in, "name"));
    snprintf(symbol, sizeof symbol, "%s", jstr(in, "symbol"));
    const int64_t decimals = (int64_t) jnum(in, "decimals");
    cJSON_Delete(in);
    if (!currency_ok(name, symbol, decimals)) return refuse(x, "STUDIO_CURRENCY_INVALID");
    if (dbx_int(x, SQL(.any = "SELECT count(*) FROM studio_currencies WHERE studio_id = {int:$1}"), KN_DB_ARGS(I64S(studio)), 0) >= STUDIO_CURRENCIES_MAX)
        return refuse(x, "STUDIO_CURRENCY_LIMIT");
    if (dbx_exists(x, SQL(.any = "SELECT 1 FROM studio_currencies WHERE studio_id = {int:$1} AND symbol = $2"), KN_DB_ARGS(I64S(studio), symbol)))
        return refuse(x, "STUDIO_CURRENCY_TAKEN");
    const int64_t id = dbx_insert_id(
        x,
        SQL(.pg = "INSERT INTO studio_currencies (studio_id, name, symbol, decimals) VALUES ($1::int, $2, $3, $4::int) RETURNING id",
            .sqlite = "INSERT INTO studio_currencies (studio_id, name, symbol, decimals) VALUES ({int:$1}, $2, $3, {int:$4}) RETURNING id",
            .any = "INSERT INTO studio_currencies (studio_id, name, symbol, decimals) VALUES ({int:$1}, $2, $3, {int:$4})"),
        KN_DB_ARGS(I64S(studio), name, symbol, I64S(decimals)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "currency_id", (double) id);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "symbol", symbol);
    cJSON_AddNumberToObject(o, "decimals", (double) decimals);
    cJSON_AddBoolToObject(o, "active", 1);
    return dbx_json(x, "currency", o);
}

static kn_db_result *op_currency_active(Dbx *x, int64_t actor, int64_t studio, int64_t currency, int active) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    if (!dbx_exists(x, SQL(.any = "SELECT 1 FROM studio_currencies WHERE id = {int:$1} AND studio_id = {int:$2}"), KN_DB_ARGS(I64S(currency), I64S(studio))))
        return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "STUDIO_CURRENCY_NOT_FOUND");
    dbx_exec(x, SQL(.any = "UPDATE studio_currencies SET active = {int:$2} WHERE id = {int:$1}"), KN_DB_ARGS(I64S(currency), I64S(active)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "currency_id", (double) currency);
    cJSON_AddBoolToObject(o, "active", active);
    return dbx_json(x, "currency", o);
}

// Edit a currency's name, symbol and decimals (the Edit dialog, deob-10.0.4.js:138692-138732). Its ID stays
// the same, so scripts that use it keep working; publishing is op_currency_active's.
static kn_db_result *op_currency_edit(Dbx *x, int64_t actor, int64_t studio, int64_t currency, const char *json) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    cJSON *in = cJSON_Parse(nz(json));
    char name[64], symbol[16];
    snprintf(name, sizeof name, "%s", jstr(in, "name"));
    snprintf(symbol, sizeof symbol, "%s", jstr(in, "symbol"));
    const int64_t decimals = (int64_t) jnum(in, "decimals");
    cJSON_Delete(in);
    if (!currency_ok(name, symbol, decimals)) return refuse(x, "STUDIO_CURRENCY_INVALID");
    if (!dbx_exists(x, SQL(.any = "SELECT 1 FROM studio_currencies WHERE id = {int:$1} AND studio_id = {int:$2}"), KN_DB_ARGS(I64S(currency), I64S(studio))))
        return dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "STUDIO_CURRENCY_NOT_FOUND");
    if (dbx_exists(x, SQL(.any = "SELECT 1 FROM studio_currencies WHERE studio_id = {int:$1} AND symbol = $2 AND id <> {int:$3}"),
                   KN_DB_ARGS(I64S(studio), symbol, I64S(currency))))
        return refuse(x, "STUDIO_CURRENCY_TAKEN");
    dbx_exec(x, SQL(.any = "UPDATE studio_currencies SET name = $2, symbol = $3, decimals = {int:$4} WHERE id = {int:$1}"),
             KN_DB_ARGS(I64S(currency), name, symbol, I64S(decimals)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "currency_id", (double) currency);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "symbol", symbol);
    cJSON_AddNumberToObject(o, "decimals", (double) decimals);
    return dbx_json(x, "currency", o);
}

static kn_db_result *op_analytics(Dbx *x, int64_t actor, int64_t studio) {
    if (role_of(x, studio, actor) <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    cJSON *cur = dbx_rows(x, SQL(.any = "SELECT id AS currency_id__i, name AS name__t, symbol AS symbol__t FROM studio_currencies"
                                        " WHERE studio_id = {int:$1} ORDER BY id"),
                          KN_DB_ARGS(I64S(studio)));
    cJSON *r;
    cJSON_ArrayForEach(r, cur) { /* no PAYMENTS currency balances exist (see the header) */
        cJSON_AddNumberToObject(r, "circulating", 0);
        cJSON_AddNumberToObject(r, "holders", 0);
    }
    /* the studio's games' charges (0358_studio_charges, P_STUDIO_CHARGE): KR Earned (its share) and Gross spent */
    const int64_t earned = dbx_int(x, SQL(.any = "SELECT coalesce(sum(earned), 0) FROM studio_charges WHERE studio_id = {int:$1}"),
                                   KN_DB_ARGS(I64S(studio)), 0);
    const int64_t charged = dbx_int(x, SQL(.any = "SELECT coalesce(sum(amount), 0) FROM studio_charges WHERE studio_id = {int:$1}"),
                                    KN_DB_ARGS(I64S(studio)), 0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "kr_earned", (double) earned);
    cJSON_AddNumberToObject(o, "kr_charged", (double) charged);
    cJSON_AddItemToObject(o, "currencies", cur ? cur : cJSON_CreateArray());
    return dbx_json(x, "analytics", o);
}

static kn_db_result *op_skins(Dbx *x, int64_t actor, int64_t studio) {
    if (role_of(x, studio, actor) <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    return dbx_json(x, "skins", cJSON_CreateArray());
}

/* ------------------------------------------------------------------ monetization */

static void add_req(cJSON *reqs, const char *key, int ok, int required, int value, int *all) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "key", key);
    cJSON_AddBoolToObject(o, "ok", ok);
    if (required >= 0) cJSON_AddNumberToObject(o, "required", required);
    if (value >= 0) cJSON_AddNumberToObject(o, "value", value);
    cJSON_AddItemToArray(reqs, o);
    if (!ok) *all = 0;
}

/* the status object; NULL after a failure. *can_apply / *pending for the writes. */
static cJSON *mon_status(Dbx *x, int64_t studio, int role, int *can_apply, int *pending) {
    cJSON *s = dbx_row(x, SQL(.any = "SELECT s.owner_id AS owner__i, s.monetization AS mon__i, s.monetization_blocked AS blocked__i,"
                                     " {epoch_ms:a.created_at} AS acct__i, coalesce(p.player_score, 0) AS score__i,"
                                     " CASE WHEN a.twofa_secret IS NOT NULL THEN 1 ELSE 0 END AS twofa__i"
                                     " FROM studios s JOIN accounts a ON a.id = s.owner_id LEFT JOIN players p ON p.player_id = s.owner_id"
                                     " WHERE s.id = {int:$1}"),
                       KN_DB_ARGS(I64S(studio)));
    if (!s) return NULL;
    const int64_t owner = (int64_t) jnum(s, "owner"), now = now_ms();
    const int enabled = jnum(s, "mon") != 0, blocked = jnum(s, "blocked") != 0;
    const int64_t bans = dbx_int(x, SQL(.any = "SELECT count(*) FROM bans WHERE account_id = {int:$1} AND revoked_at IS NULL"
                                               " AND created_at > {ago:15552000}"),
                                 KN_DB_ARGS(I64S(owner)), 0);
    const int64_t maps = dbx_int(x, SQL(.any = "SELECT count(*) FROM studio_maps s JOIN published_maps m ON m.map_id = s.map_id"
                                               " WHERE s.studio_id = {int:$1} AND m.deleted_at IS NULL"),
                                 KN_DB_ARGS(I64S(studio)), 0);
    cJSON *req = dbx_row(x, SQL(.any = "SELECT id AS id__i, status AS status__t, description AS description__t, map_ids AS map_ids__t,"
                                       " review_note AS review_note__t, {epoch_ms:created_at} AS created__i,"
                                       " {epoch_ms:reviewed_at} AS reviewed__i FROM studio_monetization"
                                       " WHERE studio_id = {int:$1} ORDER BY id DESC LIMIT 1"),
                         KN_DB_ARGS(I64S(studio)));
    if (dbx_failed(x)) {
        cJSON_Delete(s);
        cJSON_Delete(req);
        return NULL;
    }
    cJSON *reqs = cJSON_CreateArray();
    int all = 1;
    const int64_t age_days = (now - (int64_t) jnum(s, "acct")) / 86400000;
    const int level = db_level((int64_t) jnum(s, "score"), 0.03);
    add_req(reqs, "account_age", age_days >= MON_ACCOUNT_AGE_DAYS, -1, -1, &all);
    add_req(reqs, "level", level >= MON_LEVEL, MON_LEVEL, level, &all);
    add_req(reqs, "twofa", jnum(s, "twofa") != 0, -1, -1, &all);
    add_req(reqs, "no_recent_ban", bans == 0, -1, -1, &all);
    add_req(reqs, "has_map", maps > 0, -1, -1, &all);

    const char *status = req ? jstr(req, "status") : "";
    int64_t next = 0;
    if (req && (!strcmp(status, "rejected") || !strcmp(status, "revoked"))) {
        const int64_t reviewed = (int64_t) jnum(req, "reviewed");
        if (reviewed > 0 && reviewed + (int64_t) MON_REAPPLY_S * 1000 > now) next = reviewed + (int64_t) MON_REAPPLY_S * 1000;
    }
    *pending = req && !strcmp(status, "pending");
    *can_apply = role == 1 && all && !blocked && !enabled && !*pending && next == 0;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", enabled);
    cJSON_AddBoolToObject(o, "blocked", blocked);
    cJSON_AddBoolToObject(o, "eligible", all);
    cJSON_AddBoolToObject(o, "can_apply", *can_apply);
    cJSON_AddItemToObject(o, "next_apply_at", iso(next));
    if (req) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", jnum(req, "id"));
        cJSON_AddStringToObject(r, "status", status);
        cJSON_AddStringToObject(r, "description", jstr(req, "description"));
        cJSON *ids = cJSON_Parse(jstr(req, "map_ids"));
        cJSON_AddItemToObject(r, "map_ids", cJSON_IsArray(ids) ? ids : (cJSON_Delete(ids), cJSON_CreateArray()));
        const cJSON *note = cJSON_GetObjectItemCaseSensitive(req, "review_note");
        cJSON_AddItemToObject(r, "review_note", cJSON_IsString(note) && note->valuestring[0] ? cJSON_CreateString(note->valuestring) : cJSON_CreateNull());
        cJSON_AddItemToObject(r, "created_at", iso((int64_t) jnum(req, "created")));
        cJSON_AddItemToObject(r, "reviewed_at", iso((int64_t) jnum(req, "reviewed")));
        cJSON_AddItemToObject(o, "request", r);
    } else {
        cJSON_AddNullToObject(o, "request");
    }
    cJSON_AddItemToObject(o, "requirements", reqs);
    cJSON_Delete(s);
    cJSON_Delete(req);
    return o;
}

static kn_db_result *op_monetization(Dbx *x, int64_t actor, int64_t studio) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    int can, pending;
    return dbx_json(x, "monetization", mon_status(x, studio, role, &can, &pending));
}

static kn_db_result *op_monetization_apply(Dbx *x, int64_t actor, int64_t studio, const char *json) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    cJSON *in = cJSON_Parse(nz(json));
    /* the description: 10..500 characters after trimming (v10's counter) */
    const char *d = jstr(in, "description");
    while (*d == ' ' || *d == '\n' || *d == '\t' || *d == '\r') d++;
    size_t len = strlen(d);
    while (len && (d[len - 1] == ' ' || d[len - 1] == '\n' || d[len - 1] == '\t' || d[len - 1] == '\r')) len--;
    size_t chars = 0;
    for (size_t i = 0; i < len; i++) chars += ((unsigned char) d[i] & 0xC0) != 0x80;
    char *desc = malloc(len + 1);
    if (desc) {
        memcpy(desc, d, len);
        desc[len] = 0;
    }
    cJSON *ids = cJSON_CreateArray();
    int maps_ok = 1, count = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(in, "map_ids")) {
        if (!cJSON_IsNumber(m) || ++count > 5) {
            maps_ok = 0;
            break;
        }
        if (!dbx_exists(x, SQL(.any = "SELECT 1 FROM studio_maps WHERE map_id = {int:$1} AND studio_id = {int:$2}"),
                        KN_DB_ARGS(I64S((int64_t) m->valuedouble), I64S(studio))))
            maps_ok = 0;
        cJSON_AddItemToArray(ids, cJSON_CreateNumber((double) (int64_t) m->valuedouble));
    }
    cJSON_Delete(in);
    kn_db_result *bad = NULL;
    int can = 0, pending = 0;
    cJSON *st = NULL;
    if (!desc || chars < 10 || chars > 500) bad = refuse(x, "MONETIZATION_INVALID");
    else if (!maps_ok) bad = dbx_failed(x) ? dbx_finish(x, NULL) : refuse(x, "MONETIZATION_MAPS_INVALID");
    else if (!(st = mon_status(x, studio, role, &can, &pending))) bad = dbx_finish(x, NULL);
    else if (!can) {
        const int enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "enabled"));
        const int blocked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "blocked"));
        const int eligible = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st, "eligible"));
        bad = refuse(x, blocked ? "MONETIZATION_BLOCKED" : enabled ? "MONETIZATION_ENABLED" : pending ? "MONETIZATION_PENDING"
                       : !eligible ? "MONETIZATION_NOT_ELIGIBLE" : "MONETIZATION_COOLDOWN");
    }
    if (bad) {
        free(desc);
        cJSON_Delete(ids);
        cJSON_Delete(st);
        return bad;
    }
    cJSON_Delete(st);
    char *ids_text = cJSON_PrintUnformatted(ids);
    cJSON_Delete(ids);
    dbx_exec(x, SQL(.any = "INSERT INTO studio_monetization (studio_id, applicant_id, description, map_ids) VALUES ({int:$1}, {int:$2}, $3, $4)"),
             KN_DB_ARGS(I64S(studio), I64S(actor), desc, ids_text ? ids_text : "[]"));
    free(desc);
    free(ids_text);
    return dbx_json(x, "monetization", mon_status(x, studio, role, &can, &pending));
}

static kn_db_result *op_monetization_cancel(Dbx *x, int64_t actor, int64_t studio) {
    const int role = role_of(x, studio, actor);
    if (role <= 0) return refuse(x, "STUDIO_NOT_FOUND");
    if (role != 1) return refuse(x, "STUDIO_FORBIDDEN");
    if (dbx_exec(x, SQL(.any = "UPDATE studio_monetization SET status = 'cancelled' WHERE studio_id = {int:$1} AND status = 'pending'"),
                 KN_DB_ARGS(I64S(studio))) == 0)
        return refuse(x, "MONETIZATION_NONE");
    int can, pending;
    return dbx_json(x, "monetization", mon_status(x, studio, role, &can, &pending));
}

/* ------------------------------------------------------------------ the proc */

static kn_db_result *run_studio(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *op = nz(P(0));
    const int64_t actor = PI(1), a = PI(2);
    if (actor <= 0) return refuse(&x, "STUDIO_NOT_FOUND");
    if (!strcmp(op, "mine")) return op_mine(&x, actor);
    if (!strcmp(op, "create")) return op_create(&x, actor, nz(P(2)));
    if (!strcmp(op, "invites")) return op_invites(&x, actor);
    if (!strcmp(op, "invite_answer")) return op_invite_answer(&x, actor, a, PI(3) != 0);
    if (a <= 0) return refuse(&x, "STUDIO_NOT_FOUND");
    if (!strcmp(op, "get")) return op_get(&x, actor, a);
    if (!strcmp(op, "delete")) return op_delete(&x, actor, a);
    if (!strcmp(op, "invite")) return op_invite(&x, actor, a, nz(P(3)));
    if (!strcmp(op, "invite_cancel")) return op_invite_cancel(&x, actor, a, PI(3));
    if (!strcmp(op, "transfer")) return op_transfer(&x, actor, a, nz(P(3)));
    if (!strcmp(op, "member_remove")) return op_member_remove(&x, actor, a, nz(P(3)));
    if (!strcmp(op, "map_add")) return op_map_add(&x, actor, a, PI(3));
    if (!strcmp(op, "map_remove")) return op_map_remove(&x, actor, a, PI(3));
    if (!strcmp(op, "currency_create")) return op_currency_create(&x, actor, a, P(3));
    if (!strcmp(op, "currency_active")) return op_currency_active(&x, actor, a, PI(3), PI(4) != 0);
    if (!strcmp(op, "currency_edit")) return op_currency_edit(&x, actor, a, PI(3), nz(P(4)));
    if (!strcmp(op, "analytics")) return op_analytics(&x, actor, a);
    if (!strcmp(op, "skins")) return op_skins(&x, actor, a);
    if (!strcmp(op, "monetization")) return op_monetization(&x, actor, a);
    if (!strcmp(op, "monetization_apply")) return op_monetization_apply(&x, actor, a, P(3));
    if (!strcmp(op, "monetization_cancel")) return op_monetization_cancel(&x, actor, a);
    return refuse(&x, "STUDIO_NOT_FOUND");
}
const kn_proc P_STUDIO = {"studio", NULL, run_studio};

/* ------------------------------------------------------------------ monetized games (game servers)
 * The game server side of v10.0.3's GAME.PAYMENTS (game.isMonetized / authorizeSpend / transact, deob-10.0.3.js:
 * 221175-221336): the "studio_game" and "studio_charge" ops of services/brain/studios.c. */

/* the live published map of a name and its studio: {map_id, studio_id, mon} (studio_id 0: in no studio), NULL none */
static cJSON *game_of(Dbx *x, const char *map) {
    if (!map || !*map) return NULL;
    return dbx_row(x, SQL(.any = "SELECT m.map_id AS map_id__i, coalesce(sm.studio_id, 0) AS studio_id__i, coalesce(s.monetization, 0) AS mon__i"
                                 " FROM published_maps m LEFT JOIN studio_maps sm ON sm.map_id = m.map_id LEFT JOIN studios s ON s.id = sm.studio_id"
                                 " WHERE lower(m.map_name) = lower($1) AND m.deleted_at IS NULL ORDER BY m.map_id LIMIT 1"),
                   KN_DB_ARGS(map));
}

/* (map name) -> {map_id, studio_id, monetized}: game.isMonetized() of a hosted published map, v10's
 * customMapData.monetization (:221175-221177): the map's studio is approved for monetization (studios.monetization).
 * map_id 0: no such published map. */
static kn_db_result *run_studio_game(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *g = game_of(&x, P(0));
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "map_id", g ? jnum(g, "map_id") : 0);
    cJSON_AddNumberToObject(o, "studio_id", g ? jnum(g, "studio_id") : 0);
    cJSON_AddBoolToObject(o, "monetized", g && jnum(g, "studio_id") > 0 && jnum(g, "mon") != 0);
    cJSON_Delete(g);
    return dbx_json(&x, "studio_game", o);
}
const kn_proc P_STUDIO_GAME = {"studio_game", NULL, run_studio_game};

#define STUDIO_CHARGE_MAX 100000   /* authorizeSpend refuses more (:221234) */
#define STUDIO_SHARE_PCT 90        /* studios.analytics.krHint: "Studios keep 90% of the KR players spend in their games" */

/* (account, map name, amount, key) -> {success, krBalance, charged, earned, duplicate, account}: the KR part of
 * game.transact after a granted consent (customCurrency.transact(..., kr) -> {success, krBalance}, :221312-221321).
 * One transaction: the player's KR (db_add_funds, ledger source 'studio-charge', ref = key), the studio_charges row
 * with the studio's share (STUDIO_SHARE_PCT, rounded down; booked, never paid to an account: KrunkNative pays no
 * studio out). key is unique: a resent charge replays success with the balance now and takes nothing (duplicate).
 * Refusals: MONETIZATION_DISABLED (the map is in no monetized studio), STUDIO_CHARGE_INVALID (amount out of
 * 1..100000, a bad key), "Not enough KR", "Account not found.". account: the account object after the charge (the
 * caller pushes "ua" with it and drops it from the game server's answer). */
static kn_db_result *run_studio_charge(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t account = PI(0), amount = PI(2);
    const char *key = nz(P(3));
    if (account <= 0) return refuse(&x, "Account not found.");
    if (amount < 1 || amount > STUDIO_CHARGE_MAX || !key[0] || strlen(key) > 96) return refuse(&x, "STUDIO_CHARGE_INVALID");
    cJSON *o = cJSON_CreateObject();
    cJSON *done = dbx_row(&x, SQL(.any = "SELECT amount AS amount__i, earned AS earned__i, account_id AS account__i FROM studio_charges WHERE req_key = $1"),
                          KN_DB_ARGS(key));
    if (done) {
        const int64_t mine = (int64_t) jnum(done, "account") == account;
        cJSON_AddBoolToObject(o, "success", mine);
        cJSON_AddNumberToObject(o, "charged", mine ? jnum(done, "amount") : 0);
        cJSON_AddNumberToObject(o, "earned", mine ? jnum(done, "earned") : 0);
        cJSON_AddBoolToObject(o, "duplicate", 1);
        cJSON_Delete(done);
    } else {
        cJSON *g = dbx_failed(&x) ? NULL : game_of(&x, P(1));
        if (!g || jnum(g, "studio_id") <= 0 || jnum(g, "mon") == 0) {
            cJSON_Delete(g);
            cJSON_Delete(o);
            return dbx_failed(&x) ? dbx_finish(&x, NULL) : refuse(&x, "MONETIZATION_DISABLED");
        }
        const int64_t earned = amount * STUDIO_SHARE_PCT / 100;
        if (db_add_funds(&x, account, -amount, "studio-charge", key, NULL) == 0)
            dbx_exec(&x, SQL(.any = "INSERT INTO studio_charges (req_key, studio_id, map_id, account_id, amount, earned)"
                                    " VALUES ($1, {int:$2}, {int:$3}, {int:$4}, {int:$5}, {int:$6})"),
                     KN_DB_ARGS(key, I64S((int64_t) jnum(g, "studio_id")), I64S((int64_t) jnum(g, "map_id")), I64S(account), I64S(amount),
                                I64S(earned)));
        cJSON_Delete(g);
        cJSON_AddBoolToObject(o, "success", 1);
        cJSON_AddNumberToObject(o, "charged", (double) amount);
        cJSON_AddNumberToObject(o, "earned", (double) earned);
        cJSON_AddBoolToObject(o, "duplicate", 0);
    }
    if (dbx_failed(&x)) {
        cJSON_Delete(o);
        return dbx_finish(&x, NULL);
    }
    cJSON_AddNumberToObject(o, "krBalance",
                            (double) dbx_int(&x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = {int:$1}"), KN_DB_ARGS(I64S(account)), 0));
    cJSON *acc = db_account_json(&x, account);
    if (acc) cJSON_AddItemToObject(o, "account", acc);
    return dbx_json(&x, "studio_charge", o);
}
const kn_proc P_STUDIO_CHARGE = {"studio_charge", NULL, run_studio_charge};
