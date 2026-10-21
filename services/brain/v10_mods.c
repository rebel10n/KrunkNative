/*
 * services/brain/v10_mods.c - the v10 moderation, mods and KPD routes on the brain's root paths
 * (v10.0.2 gapi wrappers at v10.js 27087-27098, 27195-27229, 27329-27354; the decoded v10.0.3 bundle
 * puts the same wrappers on line 5161; the inventory is doc/v10/api-inventory.md, rows 158-160,
 * 173-178, 190-193). Each route is an op (an HTTP request, a session and one C proc on the database
 * worker, db_procs.h): the proc decides the answer and the write, and answers {"s", "data" | "code",
 * "message"}, so a refusal and its reason come from the same transaction as the write.
 *
 *   moderation (staff: moderators and developers, as flg / set_hack in db_social.c)
 *     GET  /moderation/chat/:name   {data: {chat_banned}}: the chat ban in force (feature_bans 'chat');
 *                                   a caller without staff sees chat_banned null (not disclosed).
 *     POST /moderation/chat         {player_name, ban} -> {data: {player_name, chat_banned}}: a feature_bans
 *                                   row ('chat') or its revoke, moderation_log 'chat_ban' / 'chat_unban'.
 *     POST /moderation/flag         {player_name, flag} -> {data: {player_name, flag}}: the staff hacker flag
 *                                   (players.player_hack, moderation_log 'flag' / 'unflag'), v10's flagUser
 *                                   ("Flag Player as a Hacker"). Player reports are the social tunnel's.
 *
 *   mods (creations.c's published_mods; the upload is not hosted)
 *     POST /mods/publish/key        REFUSAL 403 NOT_IMPLEMENTED: KrunkNative keeps no mod files (sh_upload_files)
 *     POST /mods/publish            REFUSAL 403 NOT_IMPLEMENTED: the multipart upload, the same
 *     GET  /mods/by-name/:name      {data: mod row} (published_mods, the list's columns plus name, creator_name,
 *                                   featured and version: the fields v10's mod view reads; version null when
 *                                   mod_info has none); 404 NOT_FOUND
 *     POST /mods/:id/vote           {vote: -1|0|1} -> {data: {id, vote, votes}}: creation_votes (kind 1), the
 *                                   net votes in published_mods.mod_votes. A signed-in player.
 *     POST /mods/:id/feature        {featured} -> {data: {mod_id, featured}}: developers (ftC's rule)
 *     DELETE /mods/:id              {data: {mod_id, deleted}}: the creator, a developer or a mod maker (delC's rule)
 *
 *   KPD (officers: moderators and developers, the officer flag of P_KPD_OFFICER in db_kpd.c)
 *     GET  /kpd/demos?off&q         {data: {rows: [group], more}} (kpd_demos, migration 0346): a group is one
 *                                   suspect; its name shows only when banned; a guest is gst 1. Each group
 *                                   carries demos[] (tok, ct, typ, map, mod, dat, dur, rsl, opn, yes, f_no, myv:
 *                                   the caller's own verdict). Public, as the list P_KPD_DEMOS serves it.
 *     GET  /kpd/demos/:id?ct=       {data: demo} for an officer (the demo's tok and ct); any other caller 404
 *     POST /kpd/demos/:id/vote      {ct, verdict 0|1} -> {data: {yes, no, myv, banned, resolved}}: one verdict per
 *                                   officer (kpd_demo_votes, migration 0351); the counts follow; a closed case 400
 *     GET  /kpd/reviewers           {data: {month: [{name, cnt}], all: [{name, cnt}]}} (P_KPD_REVIEWERS, db_kpd.c)
 *
 * Refusals: 403 FORBIDDEN (a staff-only write from another player, with its reason), 401 LOGIN_REQUIRED (a
 * write or a per-caller read without a session), 404 NOT_FOUND (an unknown resource, or a demo the caller may
 * not open), 400 BAD_REQUEST (a value outside the caller's contract). 403 NOT_IMPLEMENTED is only for the two
 * uploads, which KrunkNative cannot store.
 */
#include "v10_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acct_internal.h"
#include "v10_api.h"
#include "knsvc/http.h"
#include "knsvc/json.h"

/* the refusals' reasons and the not-found texts (the v10 caller shows the message) */
#define WHY_UPLOAD "Mod uploads are not supported on this server"
#define WHY_LOGIN "You must be logged in to do this"
#define WHY_STAFF_CHAT "Only moderators and developers can ban chat"
#define WHY_STAFF_FLAG "Only moderators and developers can flag a player"
#define WHY_STAFF_FEATURE "Only developers can feature a mod"
#define WHY_DELETE "Only the creator, a developer or a mod maker can delete a mod"
#define WHY_NO_PLAYER "Player not found"
#define WHY_NO_MOD "Mod not found"
#define WHY_NO_DEMO "Demo not found or not authorized"
#define WHY_VOTE_CLOSED "This case is closed for votes"
#define WHY_BAD_VOTE "vote must be -1, 0 or 1"
#define WHY_BAD_VERDICT "verdict must be 0 or 1"

/* the list of K.P.D demos: suspect groups per page (v10's Prev / Next step, db_kpd.c) */
#define DEMO_GROUPS 30
#define DEMO_GROUPS_MAX 50
#define OP_MAX 5

/* ------------------------------------------------------------------ DB helpers (run on the worker) */

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

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

/* the envelope a proc returns: {"s": status, "data": ...} or {"s", "code", "message"} */
static cJSON *env_ok(cJSON *data) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "s", 200);
    cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateNull());
    return o;
}

static cJSON *env_err(int status, const char *code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "s", status);
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", message);
    return o;
}

static kn_db_result *finish(Dbx *x, cJSON *env) { return dbx_json(x, "r", env); }

/* the staff flags of an account (players): developer, moderator, mod maker. 0 for a guest. */
typedef struct Staff {
    int dev;
    int mod;
    int mapmod;
} Staff;

static void staff_of(Dbx *x, int64_t id, Staff *st) {
    st->dev = st->mod = st->mapmod = 0;
    if (id <= 0) return;
    cJSON *row = dbx_row(x, SQL(.any = "SELECT player_developer AS dev__i, player_moderator AS mod__i, player_mapmod AS map__i"
                                       " FROM players WHERE player_id = {int:$1}"),
                         KN_DB_ARGS(I64S(id)));
    if (!row) return;
    st->dev = jnum(row, "dev") > 0;
    st->mod = jnum(row, "mod") > 0;
    st->mapmod = jnum(row, "map") > 0;
    cJSON_Delete(row);
}

/* K.P.D officers: moderators and developers (db_social.c's staff flags) */
static int officer(const Staff *st) { return st->dev || st->mod; }

static void log_action(Dbx *x, int64_t actor, int64_t target, const char *action) {
    dbx_exec(x, SQL(.any = "INSERT INTO moderation_log (actor_id, target_id, action) VALUES ({int:$1}, {int:$2}, $3)"),
             KN_DB_ARGS(I64S(actor), I64S(target), action));
}

static int mod_live(Dbx *x, int64_t id) {
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM published_mods WHERE mod_id = {int:$1} AND deleted_at IS NULL"),
                      KN_DB_ARGS(I64S(id)));
}

static int chat_banned(Dbx *x, int64_t id) {
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM feature_bans WHERE account_id = {int:$1} AND feature = 'chat'"
                                    " AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > {now})"),
                      KN_DB_ARGS(I64S(id)));
}

/* ---- moderation */

/* p: caller, name. Staff see the ban; anyone else sees null (not disclosed). */
static kn_db_result *run_chat_get(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    Staff st;
    staff_of(&x, PI(0), &st);
    const int64_t target = db_account_id(&x, nz(P(1)));
    if (target <= 0) return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_PLAYER));
    cJSON *d = cJSON_CreateObject();
    if (officer(&st)) cJSON_AddBoolToObject(d, "chat_banned", chat_banned(&x, target));
    else cJSON_AddNullToObject(d, "chat_banned");
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_CHAT_GET = {"mod_chat_get", NULL, run_chat_get};

/* p: caller, name, ban ("1" | "0") */
static kn_db_result *run_chat_set(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t caller = PI(0);
    const int ban = PI(2) != 0;
    Staff st;
    staff_of(&x, caller, &st);
    if (!officer(&st)) return finish(&x, env_err(403, "FORBIDDEN", WHY_STAFF_CHAT));
    const int64_t target = db_account_id(&x, nz(P(1)));
    if (target <= 0) return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_PLAYER));
    if (ban) {
        if (!chat_banned(&x, target) &&
            dbx_exec(&x, SQL(.any = "INSERT INTO feature_bans (account_id, feature, reason, created_by)"
                                    " VALUES ({int:$1}, 'chat', 'Chat ban', {int:$2})"),
                     KN_DB_ARGS(I64S(target), I64S(caller))) > 0)
            log_action(&x, caller, target, "chat_ban");
    } else if (dbx_exec(&x, SQL(.any = "UPDATE feature_bans SET revoked_at = {now}"
                                       " WHERE account_id = {int:$1} AND feature = 'chat' AND revoked_at IS NULL"),
                        KN_DB_ARGS(I64S(target))) > 0) {
        log_action(&x, caller, target, "chat_unban");
    }
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "player_name", nz(P(1)));
    cJSON_AddBoolToObject(d, "chat_banned", ban);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_CHAT_SET = {"mod_chat_set", NULL, run_chat_set};

/* p: caller, name, flag ("1" | "0"): the staff hacker flag (set_hack's rule and log) */
static kn_db_result *run_flag(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t caller = PI(0);
    const int on = PI(2) != 0;
    Staff st;
    staff_of(&x, caller, &st);
    if (!officer(&st)) return finish(&x, env_err(403, "FORBIDDEN", WHY_STAFF_FLAG));
    const int64_t target = db_account_id(&x, nz(P(1)));
    if (target <= 0) return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_PLAYER));
    if (dbx_exec(&x, SQL(.any = "UPDATE players SET player_hack = {int:$2} WHERE player_id = {int:$1}"),
                 KN_DB_ARGS(I64S(target), I64S(on))) <= 0)
        return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_PLAYER));
    log_action(&x, caller, target, on ? "flag" : "unflag");
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "player_name", nz(P(1)));
    cJSON_AddBoolToObject(d, "flag", on);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_FLAG = {"mod_flag", NULL, run_flag};

/* ---- mods */

/* p: caller, name: the published mod by name (the list's columns plus the v10 mod view's fields) */
static kn_db_result *run_mod_by_name(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT d.mod_id AS mod_id__i, {html:d.mod_name} AS mod_name,"
                                        " {html:d.mod_description} AS mod_description, d.mod_info AS mod_info__t,"
                                        " d.mod_image AS mod_image__t, d.mod_url AS mod_url__t, d.mod_votes AS mod_votes__i,"
                                        " d.mod_plays AS mod_pl__i, d.mod_updatecounter AS mod_updatecounter__i,"
                                        " d.mod_featured AS mod_featured__i, coalesce(a.username, 'Community') AS creatorname__t"
                                        " FROM published_mods d LEFT JOIN accounts a ON a.id = d.creator_id"
                                        " WHERE lower(d.mod_name) = lower($1) AND d.deleted_at IS NULL"
                                        " ORDER BY d.mod_id LIMIT 1"),
                         KN_DB_ARGS(nz(P(1))));
    if (!row) return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_MOD));
    /* mod_info is a JSON text; the version (when it has one) is the mod view's "v<version>" */
    const char *text = kn_json_str(row, "mod_info", "");
    cJSON *info = text[0] ? cJSON_Parse(text) : NULL;
    if (!cJSON_IsObject(info)) {
        cJSON_Delete(info);
        info = cJSON_CreateObject();
    }
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(info, "version");
    const int featured = kn_json_int(row, "mod_featured", 0) != 0;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "mod_id", (double) kn_json_int(row, "mod_id", 0));
    cJSON_AddStringToObject(d, "name", kn_json_str(row, "mod_name", ""));
    cJSON_AddStringToObject(d, "mod_name", kn_json_str(row, "mod_name", ""));
    cJSON_AddStringToObject(d, "creator_name", kn_json_str(row, "creatorname", "Community"));
    cJSON_AddStringToObject(d, "creatorname", kn_json_str(row, "creatorname", "Community"));
    cJSON_AddStringToObject(d, "mod_description", kn_json_str(row, "mod_description", ""));
    cJSON_AddStringToObject(d, "mod_url", kn_json_str(row, "mod_url", ""));
    if (kn_json_str(row, "mod_image", "")[0]) cJSON_AddStringToObject(d, "mod_image", kn_json_str(row, "mod_image", ""));
    else cJSON_AddNullToObject(d, "mod_image");
    cJSON_AddNumberToObject(d, "mod_votes", (double) kn_json_int(row, "mod_votes", 0));
    cJSON_AddNumberToObject(d, "mod_pl", (double) kn_json_int(row, "mod_pl", 0));
    cJSON_AddNumberToObject(d, "mod_updatecounter", (double) kn_json_int(row, "mod_updatecounter", 1));
    cJSON_AddNumberToObject(d, "mod_featured", featured);
    cJSON_AddBoolToObject(d, "featured", featured);
    if (cJSON_IsNumber(ver) || cJSON_IsString(ver)) cJSON_AddItemToObject(d, "version", cJSON_Duplicate(ver, 1));
    else cJSON_AddNullToObject(d, "version");
    cJSON_AddItemToObject(d, "mod_info", cJSON_Duplicate(info, 1));
    cJSON_Delete(info);
    cJSON_Delete(row);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_BY_NAME = {"mod_by_name", NULL, run_mod_by_name};

/* p: caller, id, vote (-1 | 0 | 1): one vote per account on the mod, the net count kept on the mod */
static kn_db_result *run_mod_vote(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t caller = PI(0), id = PI(1), vote = PI(2);
    if (!mod_live(&x, id)) return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_MOD));
    dbx_exec(&x, SQL(.any = "DELETE FROM creation_votes WHERE kind = 1 AND creation_id = {int:$1} AND account_id = {int:$2}"),
             KN_DB_ARGS(I64S(id), I64S(caller)));
    if (vote != 0)
        dbx_exec(&x, SQL(.any = "INSERT INTO creation_votes (kind, creation_id, account_id, vote)"
                                " VALUES (1, {int:$1}, {int:$2}, {int:$3})"),
                 KN_DB_ARGS(I64S(id), I64S(caller), I64S(vote)));
    dbx_exec(&x, SQL(.any = "UPDATE published_mods SET mod_votes = (SELECT coalesce(sum(vote), 0) FROM creation_votes"
                            " WHERE kind = 1 AND creation_id = {int:$1}) WHERE mod_id = {int:$1}"),
             KN_DB_ARGS(I64S(id)));
    const int64_t votes = dbx_int(&x, SQL(.any = "SELECT mod_votes FROM published_mods WHERE mod_id = {int:$1}"),
                                  KN_DB_ARGS(I64S(id)), 0);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "id", (double) id);
    cJSON_AddNumberToObject(d, "vote", (double) vote);
    cJSON_AddNumberToObject(d, "votes", (double) votes);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_VOTE = {"mod_vote", NULL, run_mod_vote};

/* p: caller, id, featured ("1" | "0"): developers only (ftC) */
static kn_db_result *run_mod_feature(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(1);
    const int on = PI(2) != 0;
    Staff st;
    staff_of(&x, PI(0), &st);
    if (!st.dev) return finish(&x, env_err(403, "FORBIDDEN", WHY_STAFF_FEATURE));
    if (dbx_exec(&x, SQL(.any = "UPDATE published_mods SET mod_featured = {int:$2} WHERE mod_id = {int:$1}"
                                " AND deleted_at IS NULL"),
                 KN_DB_ARGS(I64S(id), I64S(on))) <= 0)
        return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_MOD));
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "mod_id", (double) id);
    cJSON_AddBoolToObject(d, "featured", on);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_FEATURE = {"mod_feature", NULL, run_mod_feature};

/* p: caller, id: the creator, a developer or a mod maker (delC's rule), a soft delete */
static kn_db_result *run_mod_delete(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t caller = PI(0), id = PI(1);
    if (!mod_live(&x, id)) return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_MOD));
    const int64_t creator = dbx_int(&x, SQL(.any = "SELECT coalesce(creator_id, 0) AS c__i FROM published_mods"
                                                   " WHERE mod_id = {int:$1}"),
                                    KN_DB_ARGS(I64S(id)), 0);
    Staff st;
    staff_of(&x, caller, &st);
    const int allowed = (caller > 0 && creator == caller) || st.dev || st.mapmod;
    if (!allowed) return finish(&x, env_err(403, "FORBIDDEN", WHY_DELETE));
    dbx_exec(&x, SQL(.any = "UPDATE published_mods SET deleted_at = {now} WHERE mod_id = {int:$1} AND deleted_at IS NULL"),
             KN_DB_ARGS(I64S(id)));
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "mod_id", (double) id);
    cJSON_AddBoolToObject(d, "deleted", 1);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_DELETE = {"mod_delete", NULL, run_mod_delete};

/* ---- KPD */

/* the demo row's fields the viewer and the list read (db_kpd.c's kpd_demo_row, the same names) */
static cJSON *demo_of(const cJSON *r) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "tok", jstr(r, "tok"));
    cJSON_AddStringToObject(o, "ct", jstr(r, "ct"));
    cJSON_AddNumberToObject(o, "typ", jnum(r, "typ"));
    cJSON_AddStringToObject(o, "map", jstr(r, "map"));
    cJSON_AddStringToObject(o, "mod", jstr(r, "mod"));
    cJSON_AddNumberToObject(o, "dat", jnum(r, "dat") / 1000.0); /* seconds */
    cJSON_AddNumberToObject(o, "dur", jnum(r, "dur"));          /* milliseconds */
    cJSON_AddNumberToObject(o, "rsl", jnum(r, "rsl") != 0.0 ? 1 : 0);
    cJSON_AddNumberToObject(o, "opn", jnum(r, "opn") != 0.0 ? 1 : 0);
    cJSON_AddNumberToObject(o, "yes", jnum(r, "yes"));
    cJSON_AddNumberToObject(o, "f_no", jnum(r, "fno"));
    const cJSON *mine = cJSON_GetObjectItemCaseSensitive(r, "myv");
    if (cJSON_IsNumber(mine)) cJSON_AddNumberToObject(o, "myv", mine->valuedouble);
    else cJSON_AddNullToObject(o, "myv");
    return o;
}

/* a suspect group of the list: the name shows only when banned; a guest keeps its suspect_group */
static cJSON *group_of(const cJSON *r, int64_t ordinal) {
    const int guest = jnum(r, "ggst") != 0.0;
    const int banned = jnum(r, "gban") != 0.0;
    cJSON *g = cJSON_CreateObject();
    cJSON_AddNumberToObject(g, "grp", guest ? jnum(r, "grp") : (double) ordinal);
    cJSON_AddStringToObject(g, "name", banned ? jstr(r, "gname") : "");
    cJSON_AddNumberToObject(g, "gst", guest);
    cJSON_AddNumberToObject(g, "ban", banned);
    cJSON_AddItemToObject(g, "demos", cJSON_CreateArray());
    return g;
}

/* p: caller, off (groups), limit (0 = the page size), q (the suspect's name, "" all). The groups key is the
 * suspect (its account, or a guest's group), not grp: registered suspects all have grp 0. */
static kn_db_result *run_kpd_demos(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t off = PI(1), limit = PI(2);
    const char *q = nz(P(3));
    if (off < 0) off = 0;
    if (limit <= 0 || limit > DEMO_GROUPS_MAX) limit = DEMO_GROUPS;
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT g.gk AS gk__i, g.gname AS gname__t, g.gban AS gban__i, g.ggst AS ggst__i,"
                                          " d.token AS tok__t, d.ct AS ct__t, d.typ AS typ__i, d.map AS map__t, d.mode_name AS mod__t,"
                                          " {epoch_ms:d.created_at} AS dat__i, d.duration_ms AS dur__i, d.resolved AS rsl__i,"
                                          " d.opened AS opn__i, d.yes_votes AS yes__i, d.no_votes AS fno__i, d.suspect_group AS grp__i,"
                                          " (SELECT v.verdict FROM kpd_demo_votes v WHERE v.demo_id = d.id AND v.officer_id = {int:$1})"
                                          " AS myv__i"
                                          " FROM kpd_demos d JOIN (SELECT coalesce(suspect_id, -suspect_group) AS gk,"
                                          " max(suspect_name) AS gname, max(banned) AS gban,"
                                          " max(CASE WHEN suspect_id IS NULL THEN 1 ELSE 0 END) AS ggst, max(created_at) AS last_at"
                                          " FROM kpd_demos WHERE ($4 = '' OR lower(suspect_name) = lower($4))"
                                          " GROUP BY coalesce(suspect_id, -suspect_group)"
                                          " ORDER BY last_at DESC, gk DESC LIMIT {int:$3} OFFSET {int:$2}) g"
                                          " ON coalesce(d.suspect_id, -d.suspect_group) = g.gk"
                                          " WHERE ($4 = '' OR lower(d.suspect_name) = lower($4))"
                                          " ORDER BY g.last_at DESC, g.gk DESC, d.created_at DESC, d.id DESC"),
                           KN_DB_ARGS(I64S(PI(0)), I64S(off), I64S(limit + 1), q));
    cJSON *groups = cJSON_CreateArray();
    cJSON *group = NULL, *demos = NULL;
    const cJSON *r;
    int64_t key = 0, count = 0;
    int more = 0;
    cJSON_ArrayForEach(r, rows) {
        const int64_t gk = (int64_t) jnum(r, "gk");
        if (!group || gk != key) {
            if (++count > limit) { /* the page is full: this group starts the next page */
                more = 1;
                break;
            }
            key = gk;
            group = group_of(r, count);
            demos = cJSON_GetObjectItemCaseSensitive(group, "demos");
            cJSON_AddItemToArray(groups, group);
        }
        cJSON_AddItemToArray(demos, demo_of(r));
    }
    cJSON_Delete(rows);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "rows", groups);
    cJSON_AddBoolToObject(d, "more", more);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_KPD_DEMOS = {"mod_kpd_demos", NULL, run_kpd_demos};

/* p: caller, token, ct ("" none): an officer's view of one demo; anyone else (or another call token) 404 */
static kn_db_result *run_kpd_demo(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t caller = PI(0);
    const char *token = nz(P(1)), *ct = nz(P(2));
    Staff st;
    staff_of(&x, caller, &st);
    if (!officer(&st)) return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_DEMO));
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT d.id AS id__i, d.token AS tok__t, d.ct AS ct__t, d.typ AS typ__i, d.map AS map__t,"
                                        " d.mode_name AS mod__t, {epoch_ms:d.created_at} AS dat__i, d.duration_ms AS dur__i,"
                                        " d.banned AS ban__i, d.suspect_name AS gname__t, d.suspect_id AS sid__i,"
                                        " d.resolved AS rsl__i, d.opened AS opn__i, d.yes_votes AS yes__i, d.no_votes AS fno__i"
                                        " FROM kpd_demos d WHERE d.token = $1"),
                         KN_DB_ARGS(token));
    if (!row || (ct[0] && strcmp(jstr(row, "ct"), ct) != 0)) {
        cJSON_Delete(row);
        return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_DEMO));
    }
    const int64_t id = (int64_t) jnum(row, "id");
    const int banned = jnum(row, "ban") != 0.0;
    cJSON *mine = dbx_row(&x, SQL(.any = "SELECT verdict AS v__i FROM kpd_demo_votes WHERE demo_id = {int:$1}"
                                         " AND officer_id = {int:$2}"),
                          KN_DB_ARGS(I64S(id), I64S(caller)));
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "tok", jstr(row, "tok"));
    cJSON_AddStringToObject(d, "ct", jstr(row, "ct"));
    cJSON_AddNumberToObject(d, "typ", jnum(row, "typ"));
    cJSON_AddStringToObject(d, "map", jstr(row, "map"));
    cJSON_AddStringToObject(d, "mod", jstr(row, "mod"));
    cJSON_AddNumberToObject(d, "dat", jnum(row, "dat") / 1000.0);
    cJSON_AddNumberToObject(d, "dur", jnum(row, "dur"));
    cJSON_AddNumberToObject(d, "ban", banned ? 1 : 0);
    cJSON_AddNumberToObject(d, "gst", jnum(row, "sid") != 0.0 ? 0 : 1);
    cJSON_AddStringToObject(d, "name", banned ? jstr(row, "gname") : "");
    cJSON_AddNumberToObject(d, "rsl", jnum(row, "rsl") != 0.0 ? 1 : 0);
    cJSON_AddNumberToObject(d, "opn", jnum(row, "opn") != 0.0 ? 1 : 0);
    cJSON_AddNumberToObject(d, "yes", jnum(row, "yes"));
    cJSON_AddNumberToObject(d, "f_no", jnum(row, "fno"));
    if (mine) cJSON_AddNumberToObject(d, "myv", jnum(mine, "v"));
    else cJSON_AddNullToObject(d, "myv");
    cJSON_Delete(mine);
    cJSON_Delete(row);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_KPD_DEMO = {"mod_kpd_demo", NULL, run_kpd_demo};

/* p: caller, token, ct, verdict ("1" yes | "0" no): one verdict per officer, the counts follow */
static kn_db_result *run_kpd_vote(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t caller = PI(0);
    const char *token = nz(P(1)), *ct = nz(P(2));
    const int verdict = PI(3) != 0;
    Staff st;
    staff_of(&x, caller, &st);
    if (!officer(&st)) return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_DEMO));
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT d.id AS id__i, d.ct AS ct__t, d.banned AS ban__i, d.resolved AS rsl__i,"
                                        " d.opened AS opn__i FROM kpd_demos d WHERE d.token = $1"),
                         KN_DB_ARGS(token));
    if (!row || (ct[0] && strcmp(jstr(row, "ct"), ct) != 0)) {
        cJSON_Delete(row);
        return finish(&x, env_err(404, "NOT_FOUND", WHY_NO_DEMO));
    }
    const int64_t id = (int64_t) jnum(row, "id");
    if (jnum(row, "opn") == 0.0) {
        cJSON_Delete(row);
        return finish(&x, env_err(400, "VOTE_CLOSED", WHY_VOTE_CLOSED));
    }
    cJSON_Delete(row);
    dbx_exec(&x, SQL(.any = "DELETE FROM kpd_demo_votes WHERE demo_id = {int:$1} AND officer_id = {int:$2}"),
             KN_DB_ARGS(I64S(id), I64S(caller)));
    dbx_exec(&x, SQL(.any = "INSERT INTO kpd_demo_votes (demo_id, officer_id, verdict) VALUES ({int:$1}, {int:$2}, {int:$3})"),
             KN_DB_ARGS(I64S(id), I64S(caller), I64S(verdict)));
    dbx_exec(&x, SQL(.any = "UPDATE kpd_demos SET yes_votes = (SELECT count(*) FROM kpd_demo_votes"
                            " WHERE demo_id = {int:$1} AND verdict = 1),"
                            " no_votes = (SELECT count(*) FROM kpd_demo_votes WHERE demo_id = {int:$1} AND verdict = 0)"
                            " WHERE id = {int:$1}"),
             KN_DB_ARGS(I64S(id)));
    cJSON *now = dbx_row(&x, SQL(.any = "SELECT yes_votes AS yes__i, no_votes AS fno__i, banned AS ban__i, resolved AS rsl__i"
                                        " FROM kpd_demos WHERE id = {int:$1}"),
                         KN_DB_ARGS(I64S(id)));
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "yes", jnum(now, "yes"));
    cJSON_AddNumberToObject(d, "no", jnum(now, "fno"));
    cJSON_AddNumberToObject(d, "myv", verdict);
    cJSON_AddBoolToObject(d, "banned", jnum(now, "ban") != 0.0);
    cJSON_AddBoolToObject(d, "resolved", jnum(now, "rsl") != 0.0);
    cJSON_Delete(now);
    return finish(&x, env_ok(d));
}
static const kn_proc P_MOD_KPD_VOTE = {"mod_kpd_vote", NULL, run_kpd_vote};

/* ------------------------------------------------------------------ the HTTP side */

static void h_refuse(kn_http_req *req, void *ud) { v10_reply_error(req, 403, "NOT_IMPLEMENTED", (const char *) ud); }

static void bad_request(kn_http_req *req, const char *message) { v10_reply_error(req, 400, "BAD_REQUEST", message); }

/* the session token: ?token= or the Bearer header (ranked.c's get_token) */
static void get_token(kn_http_req *req, char *token, size_t cap) {
    token[0] = 0;
    if (kn_http_query_get(req, "token", token, cap) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(token, cap, "%s", bearer ? bearer : "");
    }
}

/* the reply of a proc's envelope (or of a shape function's): 200 data, else the error */
static void reply_env(kn_http_req *req, cJSON *env) {
    if (!env) {
        v10_reply_error(req, 500, "SERVER_ERROR", "Server error.");
        return;
    }
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(env, "s");
    const int status = cJSON_IsNumber(s) ? (int) s->valuedouble : 500;
    if (status == 200) {
        v10_reply_data(req, 200, cJSON_DetachItemFromObjectCaseSensitive(env, "data"));
        return;
    }
    v10_reply_error(req, status, kn_json_str(env, "code", "SERVER_ERROR"), kn_json_str(env, "message", "Server error."));
}

/* An op: one request, its session (the caller id) and one proc. The params are copied in when the
 * request is read, so the proc runs after the session lookup with no pointer into the request. */
typedef struct Op {
    kn_http_req *req;
    Brain *b;
    const kn_proc *proc;
    cJSON *(*shape)(cJSON *raw); /* NULL: the proc's own result is the envelope */
    int login;                   /* 1: a session is required (401 without one) */
    int caller_arg;              /* 1: p[0] is the caller's account id (0 when none) */
    int argc;
    char arg[OP_MAX][256];
} Op;

static Op *op_new(kn_http_req *req, Brain *b, const kn_proc *proc, int login, int caller_arg) {
    Op *op = calloc(1, sizeof *op);
    if (!op) {
        v10_reply_error(req, 500, "SERVER_ERROR", "Server error.");
        return NULL;
    }
    op->req = req;
    op->b = b;
    op->proc = proc;
    op->login = login;
    op->caller_arg = caller_arg;
    return op;
}

static void op_add(Op *op, const char *text) {
    if (op->argc < OP_MAX) snprintf(op->arg[op->argc++], sizeof op->arg[0], "%s", text ? text : "");
}

static void op_add_int(Op *op, int64_t v) {
    char buf[24];
    op_add(op, acct_itoa(buf, v));
}

static void on_done(kn_db_result *res, void *ud) {
    Op *op = ud;
    cJSON *env;
    if (!kn_db_ok(res)) {
        acct_db_error(res, "moderation");
        env = env_err(500, "SERVER_ERROR", "Server error.");
    } else {
        cJSON *raw = acct_db_json(res);
        env = op->shape ? op->shape(raw) : raw;
    }
    if (kn_http_req_alive(op->req)) reply_env(op->req, env);
    cJSON_Delete(env);
    free(op);
}

static void op_call(Op *op, int64_t caller) {
    if (op->login && caller <= 0) {
        v10_reply_error(op->req, 401, "LOGIN_REQUIRED", WHY_LOGIN);
        free(op);
        return;
    }
    const char *pv[OP_MAX + 1];
    int np = 0;
    char cid[24];
    if (op->caller_arg) pv[np++] = acct_itoa(cid, caller);
    for (int i = 0; i < op->argc; i++) pv[np++] = op->arg[i];
    if (!op->b->db || kn_db_call(op->b->db, op->proc, np, pv, on_done, op) != 0) {
        v10_reply_error(op->req, 500, "SERVER_ERROR", "Server error.");
        free(op);
    }
}

static void on_acc(void *ud, const char *err, const cJSON *acc) {
    Op *op = ud;
    op_call(op, err || !acc ? 0 : kn_json_int(acc, "id", 0));
}

static void op_start(Op *op) {
    char token[512];
    get_token(op->req, token, sizeof token);
    if (!token[0]) {
        op_call(op, 0);
        return;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(op->b, "token", args, on_acc, op);
    cJSON_Delete(args);
}

/* the path's {name} as a positive integer id */
static int path_id(kn_http_req *req, const char *name, int64_t *out) {
    const char *s = kn_http_param(req, name);
    char *end = NULL;
    const long long v = s ? strtoll(s, &end, 10) : 0;
    if (!s || !*s || !end || *end || v <= 0) return 0;
    *out = v;
    return 1;
}

static int body_bool(const cJSON *in, const char *key, int *out) {
    const cJSON *v = in ? cJSON_GetObjectItemCaseSensitive(in, key) : NULL;
    if (!cJSON_IsBool(v)) return 0;
    *out = cJSON_IsTrue(v) ? 1 : 0;
    return 1;
}

static int body_int(const cJSON *in, const char *key, int64_t lo, int64_t hi, int64_t *out) {
    const cJSON *v = in ? cJSON_GetObjectItemCaseSensitive(in, key) : NULL;
    if (!cJSON_IsNumber(v)) return 0;
    const double d = v->valuedouble;
    if (d < (double) lo || d > (double) hi) return 0; /* range first: the cast below is then in range */
    if (d != (double) (int64_t) d) return 0;
    *out = (int64_t) d;
    return 1;
}

/* ---- moderation */

static void h_chat_get(kn_http_req *req, void *ud) {
    Op *op = op_new(req, ud, &P_MOD_CHAT_GET, 0, 1);
    if (!op) return;
    op_add(op, kn_http_param(req, "name"));
    op_start(op);
}

static void h_chat_set(kn_http_req *req, void *ud) {
    cJSON *in = kn_http_body_json(req);
    const char *name = in ? kn_json_str(in, "player_name", "") : "";
    int ban = 0;
    if (!name[0] || !body_bool(in, "ban", &ban)) {
        cJSON_Delete(in);
        bad_request(req, "player_name and ban (true or false) are needed");
        return;
    }
    Op *op = op_new(req, ud, &P_MOD_CHAT_SET, 1, 1);
    if (op) {
        op_add(op, name);
        op_add_int(op, ban);
        op_start(op);
    }
    cJSON_Delete(in);
}

static void h_flag(kn_http_req *req, void *ud) {
    cJSON *in = kn_http_body_json(req);
    const char *name = in ? kn_json_str(in, "player_name", "") : "";
    int flag = 0;
    if (!name[0] || !body_bool(in, "flag", &flag)) {
        cJSON_Delete(in);
        bad_request(req, "player_name and flag (true or false) are needed");
        return;
    }
    Op *op = op_new(req, ud, &P_MOD_FLAG, 1, 1);
    if (op) {
        op_add(op, name);
        op_add_int(op, flag);
        op_start(op);
    }
    cJSON_Delete(in);
}

/* ---- mods */

static void h_by_name(kn_http_req *req, void *ud) {
    Op *op = op_new(req, ud, &P_MOD_BY_NAME, 0, 1);
    if (!op) return;
    op_add(op, kn_http_param(req, "name"));
    op_start(op);
}

static void h_vote(kn_http_req *req, void *ud) {
    int64_t id = 0;
    if (!path_id(req, "id", &id)) {
        v10_reply_error(req, 404, "NOT_FOUND", WHY_NO_MOD);
        return;
    }
    cJSON *in = kn_http_body_json(req);
    int64_t vote = 0;
    if (!body_int(in, "vote", -1, 1, &vote)) {
        cJSON_Delete(in);
        bad_request(req, WHY_BAD_VOTE);
        return;
    }
    cJSON_Delete(in);
    Op *op = op_new(req, ud, &P_MOD_VOTE, 1, 1);
    if (op) {
        op_add_int(op, id);
        op_add_int(op, vote);
        op_start(op);
    }
}

static void h_feature(kn_http_req *req, void *ud) {
    int64_t id = 0;
    if (!path_id(req, "id", &id)) {
        v10_reply_error(req, 404, "NOT_FOUND", WHY_NO_MOD);
        return;
    }
    cJSON *in = kn_http_body_json(req);
    int featured = 0;
    if (!body_bool(in, "featured", &featured)) {
        cJSON_Delete(in);
        bad_request(req, "featured (true or false) is needed");
        return;
    }
    cJSON_Delete(in);
    Op *op = op_new(req, ud, &P_MOD_FEATURE, 1, 1);
    if (op) {
        op_add_int(op, id);
        op_add_int(op, featured);
        op_start(op);
    }
}

static void h_delete(kn_http_req *req, void *ud) {
    int64_t id = 0;
    if (!path_id(req, "id", &id)) {
        v10_reply_error(req, 404, "NOT_FOUND", WHY_NO_MOD);
        return;
    }
    Op *op = op_new(req, ud, &P_MOD_DELETE, 1, 1);
    if (op) {
        op_add_int(op, id);
        op_start(op);
    }
}

/* ---- KPD */

/* the Demos list: off (groups), q (the suspect's name) */
static void h_demos(kn_http_req *req, void *ud) {
    char off[24] = "", q[64] = "";
    kn_http_query_get(req, "off", off, sizeof off);
    kn_http_query_get(req, "q", q, sizeof q);
    Op *op = op_new(req, ud, &P_MOD_KPD_DEMOS, 0, 1);
    if (!op) return;
    op_add_int(op, off[0] ? pint(off) : 0);
    op_add_int(op, 0); /* limit: the page size */
    op_add(op, q);
    op_start(op);
}

static void h_demo(kn_http_req *req, void *ud) {
    char ct[80] = "";
    kn_http_query_get(req, "ct", ct, sizeof ct);
    Op *op = op_new(req, ud, &P_MOD_KPD_DEMO, 1, 1);
    if (!op) return;
    op_add(op, kn_http_param(req, "id"));
    op_add(op, ct);
    op_start(op);
}

static void h_demo_vote(kn_http_req *req, void *ud) {
    const char *token = kn_http_param(req, "id");
    cJSON *in = kn_http_body_json(req);
    int64_t verdict = 0;
    const char *ct = in ? kn_json_str(in, "ct", "") : "";
    if (!token || !*token || !body_int(in, "verdict", 0, 1, &verdict)) {
        cJSON_Delete(in);
        bad_request(req, WHY_BAD_VERDICT);
        return;
    }
    Op *op = op_new(req, ud, &P_MOD_KPD_VOTE, 1, 1);
    if (op) {
        op_add(op, token);
        op_add(op, ct);
        op_add_int(op, verdict);
        op_start(op);
    }
    cJSON_Delete(in);
}

/* the Reviewers: the officers' handled calls, split into the last 30 days and all time */
static cJSON *shape_reviewers(cJSON *raw) {
    cJSON *month = cJSON_CreateArray(), *all = cJSON_CreateArray();
    cJSON *list = raw ? cJSON_GetObjectItemCaseSensitive(raw, "reviewers") : NULL;
    const cJSON *r;
    cJSON_ArrayForEach(r, list) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", kn_json_str(r, "name", ""));
        cJSON_AddNumberToObject(o, "cnt", (double) kn_json_int(r, "cnt", 0));
        cJSON_AddItemToArray(strcmp(kn_json_str(r, "range", ""), "month") == 0 ? month : all, o);
    }
    cJSON_Delete(raw);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "month", month);
    cJSON_AddItemToObject(d, "all", all);
    return env_ok(d);
}

static void h_reviewers(kn_http_req *req, void *ud) {
    Op *op = op_new(req, ud, &P_KPD_REVIEWERS, 0, 0);
    if (!op) return;
    op->shape = shape_reviewers;
    op_start(op);
}

int v10_mods_init(Brain *b) {
    /* moderation */
    kn_http_route(b->http, "GET", "/moderation/chat/:name", h_chat_get, b);
    kn_http_route(b->http, "POST", "/moderation/chat", h_chat_set, b);
    kn_http_route(b->http, "POST", "/moderation/flag", h_flag, b);

    /* mods: the uploads are refused (no mod file storage); the rest is the published_mods catalogue */
    kn_http_route(b->http, "POST", "/mods/publish/key", h_refuse, (void *) WHY_UPLOAD);
    kn_http_route(b->http, "POST", "/mods/publish", h_refuse, (void *) WHY_UPLOAD);
    kn_http_route(b->http, "GET", "/mods/by-name/:name", h_by_name, b);
    kn_http_route(b->http, "POST", "/mods/:id/vote", h_vote, b);
    kn_http_route(b->http, "POST", "/mods/:id/feature", h_feature, b);
    kn_http_route(b->http, "DELETE", "/mods/:id", h_delete, b);

    /* KPD */
    kn_http_route(b->http, "GET", "/kpd/demos", h_demos, b);
    kn_http_route(b->http, "GET", "/kpd/demos/:id", h_demo, b);
    kn_http_route(b->http, "POST", "/kpd/demos/:id/vote", h_demo_vote, b);
    kn_http_route(b->http, "GET", "/kpd/reviewers", h_reviewers, b);
    return 0;
}
