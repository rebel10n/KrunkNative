/*
 * services/brain/clan_v10.c - Krunker v10.0.2's clan window settings tab (windows[12] tab 4,
 * the Svelte clan-settings component, v10:95700-96924) over the gapi tunnel (social_gapi.c).
 *
 *   GET    /clans/<id>/motd              {message, set_by_name, set_at}       members
 *   PUT    /clans/<id>/motd {message}    same                                 owner, role >= 2
 *   DELETE /clans/<id>/motd              {}                                   owner, role >= 2
 *   POST   /clans/<id>/banner-color {paint_item_id}  {clan_banner_color, paint_item_id, count}
 *                                        owner, role >= 2; consumes one owned paint (v10 store
 *                                        skins of type 18, the table below)
 *   POST   /clans/<id>/logo              refused: no upload pipeline (no logo storage / review)
 *   DELETE /clans/<id>/logo              {}                                   owner, Captain
 *   POST   /clans/<id>/transfer {name}   ownership_transfer                   owner
 *   DELETE /clans/<id>/transfer          {}                                   owner
 *   POST   /clans/<id>/transfer/respond {accept}  {accepted}                  the transfer's target
 *
 * Deviation: v10 does ownership transfer with socket RPCs (rpc('clan-transfer-initiate' |
 * 'clan-transfer-cancel' | 'clan-transfer-respond')); KrunkNative maps them onto the three
 * /transfer paths above on the same tunnel. Roles are clan_members.rank (v10 clanRole): 2 Captain,
 * 3 Commander; the owner is clans.clan_playerid. Everything is authorised by the socket's own
 * account, never a client-supplied player id. Writes are rate limited per account.
 * Loot config (/api/clans/<id>/loot-config, turf.c) and the recruitment board are other modules'.
 */
#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hot.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

/* v10.0.2 paints (store skins of type 18: index, paintColor or paintShader) */
static const struct {
    int index;
    const char *color; /* NULL: shader */
    int shader;
} k_paints[] = {
    {7776, "#dc3c3c", -1}, {7777, "#50c864", -1}, {7778, "#0457d1", -1}, {7779, "#1a1a1a", -1}, {7780, "#f0f0f0", -1},
    {7781, "#f0d23c", -1}, {7795, NULL, 0},       {7796, NULL, 1},       {7831, "#A884E1", -1}, {7832, "#84E18C", -1},
    {7833, "#84D9E1", -1}, {7834, "#FF1FB8", -1}, {7835, "#c08552", -1}, {7836, "#ff9913", -1}, {7837, "#bf00ff", -1},
    {7904, NULL, 2},       {7905, NULL, 3},       {7906, NULL, 4},       {7907, NULL, 5},
};

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }
#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

/* ------------------------------------------------------------------ procs (C on every driver) */

/* -1 = not a member of clan; else the rank, *owner = 1 for the owner */
static int64_t member_rank(Dbx *x, const char *actor, int64_t clan, int *owner) {
    *owner = dbx_exists(x, SQL(.any = "SELECT 1 FROM clans WHERE clan_id = $1 AND clan_playerid = $2"), KN_DB_ARGS(I64S(clan), actor));
    return dbx_int(x, SQL(.any = "SELECT rank FROM clan_members WHERE clan_id = $1 AND account_id = $2"), KN_DB_ARGS(I64S(clan), actor), -1);
}

static cJSON *motd_json(Dbx *x, int64_t clan) {
    cJSON *o = dbx_row(x,
        SQL(.any = "SELECT c.clan_motd AS message, a.username AS set_by_name, {epoch_ms:c.clan_motd_at} AS set_at"
                   " FROM clans c LEFT JOIN accounts a ON a.id = c.clan_motd_by WHERE c.clan_id = $1"),
        KN_DB_ARGS(I64S(clan)));
    if (!o) return NULL;
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(o, "message");
    if (!cJSON_IsString(m) || !m->valuestring[0]) {
        cJSON_ReplaceItemInObjectCaseSensitive(o, "message", cJSON_CreateNull());
        cJSON_ReplaceItemInObjectCaseSensitive(o, "set_by_name", cJSON_CreateNull());
        cJSON_ReplaceItemInObjectCaseSensitive(o, "set_at", cJSON_CreateNull());
    }
    return o;
}

/* (actor, clan) */
static kn_db_result *run_motd_get(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int owner;
    if (member_rank(&x, P(0), PI(1), &owner) < 0) return dbx_finish(&x, kn_db_result_user_error("Not a member of this clan"));
    return dbx_json(&x, "motd", motd_json(&x, PI(1)));
}
static const kn_proc P_MOTD_GET = {"clan_motd_get", NULL, run_motd_get};

/* (actor, clan, message | "" = delete) */
static kn_db_result *run_motd_set(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int owner;
    const int64_t rank = member_rank(&x, P(0), PI(1), &owner);
    if (!owner && rank < 2) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    if (P(2) && P(2)[0])
        dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_motd = $2, clan_motd_by = {int:$3}, clan_motd_at = {now} WHERE clan_id = $1"),
                 KN_DB_ARGS(I64S(PI(1)), P(2), P(0)));
    else
        dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_motd = '', clan_motd_by = NULL, clan_motd_at = NULL WHERE clan_id = $1"),
                 KN_DB_ARGS(I64S(PI(1))));
    return dbx_json(&x, "motd", motd_json(&x, PI(1)));
}
static const kn_proc P_MOTD_SET = {"clan_motd_set", NULL, run_motd_set};

/* (actor, clan, paint skin, banner value) */
static kn_db_result *run_banner(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int owner;
    const int64_t rank = member_rank(&x, P(0), PI(1), &owner);
    if (!owner && rank < 2) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    const int64_t have = dbx_int(&x, SQL(.any = "SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2"),
                                 KN_DB_ARGS(P(0), P(2)), 0);
    if (have <= 0) return dbx_finish(&x, kn_db_result_user_error("You don't have any of that paint left."));
    int64_t left = 0;
    char ref[32];
    snprintf(ref, sizeof ref, "clan:%lld", (long long) PI(1));
    db_add_skin(&x, PI(0), PI(2), -1, "clan-banner", ref, &left);
    dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_banner_color = $2 WHERE clan_id = $1"), KN_DB_ARGS(I64S(PI(1)), P(3)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "clan_banner_color", P(3) ? P(3) : "");
    cJSON_AddNumberToObject(o, "paint_item_id", (double) PI(2));
    cJSON_AddNumberToObject(o, "count", (double) left);
    return dbx_json(&x, "banner", o);
}
static const kn_proc P_BANNER = {"clan_banner", NULL, run_banner};

/* (actor, clan) */
static kn_db_result *run_logo_delete(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int owner;
    const int64_t rank = member_rank(&x, P(0), PI(1), &owner);
    if (!owner && rank != 2) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_logo = 0, clan_logo_pending = 0 WHERE clan_id = $1"), KN_DB_ARGS(I64S(PI(1))));
    return dbx_json(&x, "logo", cJSON_CreateObject());
}
static const kn_proc P_LOGO_DELETE = {"clan_logo_delete", NULL, run_logo_delete};

static cJSON *transfer_json(Dbx *x, int64_t clan) {
    return dbx_row(x,
        SQL(.any = "SELECT c.clan_transfer_to AS to_player_id, t.username AS to_player_name, o.username AS from_player_name,"
                   " c.clan_name, {epoch_ms:c.clan_transfer_at} AS created_at"
                   " FROM clans c JOIN accounts t ON t.id = c.clan_transfer_to LEFT JOIN accounts o ON o.id = c.clan_playerid"
                   " WHERE c.clan_id = $1"),
        KN_DB_ARGS(I64S(clan)));
}

/* (actor, clan, name) */
static kn_db_result *run_transfer(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int owner;
    member_rank(&x, P(0), PI(1), &owner);
    if (!owner) return dbx_finish(&x, kn_db_result_user_error("Only the clan owner can transfer ownership"));
    const int64_t to = dbx_int(&x,
        SQL(.any = "SELECT m.account_id FROM clan_members m JOIN accounts a ON a.id = m.account_id"
                   " WHERE m.clan_id = $1 AND lower(a.username) = lower($2)"),
        KN_DB_ARGS(I64S(PI(1)), P(2) ? P(2) : ""), 0);
    if (to <= 0) return dbx_finish(&x, kn_db_result_user_error("Player is not a member of your clan"));
    if (to == PI(0)) return dbx_finish(&x, kn_db_result_user_error("You already own this clan"));
    dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_transfer_to = {int:$2}, clan_transfer_at = {now} WHERE clan_id = $1"),
             KN_DB_ARGS(I64S(PI(1)), I64S(to)));
    return dbx_json(&x, "transfer", transfer_json(&x, PI(1)));
}
static const kn_proc P_TRANSFER = {"clan_transfer", NULL, run_transfer};

/* (actor, clan) */
static kn_db_result *run_transfer_cancel(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int owner;
    member_rank(&x, P(0), PI(1), &owner);
    if (!owner) return dbx_finish(&x, kn_db_result_user_error("Only the clan owner can cancel the transfer"));
    dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_transfer_to = NULL, clan_transfer_at = NULL WHERE clan_id = $1"), KN_DB_ARGS(I64S(PI(1))));
    return dbx_json(&x, "transfer", cJSON_CreateObject());
}
static const kn_proc P_TRANSFER_CANCEL = {"clan_transfer_cancel", NULL, run_transfer_cancel};

/* (actor, clan, accept 1|0): the target takes the clan (rank 3); the old owner becomes a Captain */
static kn_db_result *run_transfer_respond(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t clan = PI(1);
    const int64_t from = dbx_int(&x, SQL(.any = "SELECT clan_playerid FROM clans WHERE clan_id = $1 AND clan_transfer_to = {int:$2} {for_update}"),
                                 KN_DB_ARGS(I64S(clan), P(0)), 0);
    if (from <= 0) return dbx_finish(&x, kn_db_result_user_error("No pending transfer"));
    const int accept = PI(2) != 0;
    if (accept) {
        if (dbx_exec(&x, SQL(.any = "UPDATE clan_members SET rank = 3 WHERE clan_id = $1 AND account_id = $2"), KN_DB_ARGS(I64S(clan), P(0))) < 1)
            return dbx_finish(&x, kn_db_result_user_error("You are not a member of this clan"));
        dbx_exec(&x, SQL(.any = "UPDATE clan_members SET rank = 2 WHERE clan_id = $1 AND account_id = $2"), KN_DB_ARGS(I64S(clan), I64S(from)));
        dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_playerid = {int:$2} WHERE clan_id = $1"), KN_DB_ARGS(I64S(clan), P(0)));
    }
    dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_transfer_to = NULL, clan_transfer_at = NULL WHERE clan_id = $1"), KN_DB_ARGS(I64S(clan)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "accepted", accept);
    return dbx_json(&x, "transfer", o);
}
static const kn_proc P_TRANSFER_RESPOND = {"clan_transfer_respond", NULL, run_transfer_respond};

/* ------------------------------------------------------------------ routes */

/* per-account write limiter (30 writes / minute), shared with Redis when it is up */
static int write_ok(int64_t account) {
    static struct {
        int64_t id;
        uint64_t t0;
        int n;
    } slots[128];
    const uint64_t now = kn_now_ms();
    int local = 1;
    size_t i = (size_t) (account % 128);
    if (slots[i].id != account || now - slots[i].t0 > 60000) {
        slots[i].id = account;
        slots[i].t0 = now;
        slots[i].n = 0;
    }
    if (++slots[i].n > 30) local = 0;
    char key[24];
    snprintf(key, sizeof key, "%lld", (long long) account);
    return hot_limit("clanv10", key, 30, 60, local);
}

static void data_done(SocialSession *s, kn_db_result *res, void *extra) {
    const int64_t rid = *(int64_t *) extra;
    if (!kn_db_ok(res)) {
        gapi_error(s, rid, 400, "CLAN_ERROR", acct_db_error(res, "clan"));
        return;
    }
    cJSON *v = acct_db_json(res);
    gapi_reply(s, rid, v);
}

static int64_t *rid_of(const GapiReq *r) {
    int64_t *x = malloc(sizeof *x);
    if (x) *x = r->rid;
    return x;
}

int clan_v10_gapi(SocialSession *s, const GapiReq *r) {
    if (r->nseg < 3 || strcmp(r->seg[0], "clans")) return 0;
    const char *what = r->seg[2];
    const int known = !strcmp(what, "motd") || !strcmp(what, "banner-color") || !strcmp(what, "logo") || !strcmp(what, "transfer");
    if (!known) return 0;
    if (s->account_id <= 0) {
        gapi_error(s, r->rid, 401, "UNAUTHORIZED", "You must be signed in.");
        return 1;
    }
    char *end = NULL;
    const long long clan = strtoll(r->seg[1], &end, 10);
    if (!end || *end || clan <= 0) {
        gapi_error(s, r->rid, 404, "CLAN_NOT_FOUND", "Clan not found");
        return 1;
    }
    if (strcmp(r->method, "GET") && !write_ok(s->account_id)) {
        gapi_error(s, r->rid, 429, "RATE_LIMITED", "Rate limit exceeded. Try again later.");
        return 1;
    }
    char me[24], cl[24];
    social_viewer(s, me);
    snprintf(cl, sizeof cl, "%lld", clan);
    if (gapi_is(r, "GET", "/clans/:id/motd")) {
        social_call(s, &P_MOTD_GET, KN_DB_ARGS(me, cl), data_done, rid_of(r));
    } else if (gapi_is(r, "PUT", "/clans/:id/motd") || gapi_is(r, "DELETE", "/clans/:id/motd")) {
        char msg[1600] = "";
        if (!strcmp(r->method, "PUT")) {
            const char *m = kn_json_str(r->body, "message", "");
            while (*m == ' ' || *m == '\n' || *m == '\t') m++;
            size_t len = strlen(m);
            while (len && (m[len - 1] == ' ' || m[len - 1] == '\n' || m[len - 1] == '\t')) len--;
            size_t chars = 0;
            for (size_t i = 0; i < len; i++) chars += ((unsigned char) m[i] & 0xC0) != 0x80;
            if (!len || chars > 500 || len >= sizeof msg) {
                gapi_error(s, r->rid, 400, "CLAN_MOTD_INVALID", !len ? "Message is required" : "Message is too long");
                return 1;
            }
            memcpy(msg, m, len);
            msg[len] = 0;
        }
        social_call(s, &P_MOTD_SET, KN_DB_ARGS(me, cl, msg), data_done, rid_of(r));
    } else if (gapi_is(r, "POST", "/clans/:id/banner-color")) {
        const int64_t item = kn_json_int(r->body, "paint_item_id", -1);
        char value[16] = "", skin[24];
        for (size_t i = 0; i < sizeof k_paints / sizeof k_paints[0]; i++) {
            if (k_paints[i].index != item) continue;
            if (k_paints[i].color) snprintf(value, sizeof value, "%s", k_paints[i].color);
            else snprintf(value, sizeof value, "s%d", k_paints[i].shader);
        }
        if (!value[0]) {
            gapi_error(s, r->rid, 400, "CLAN_PAINT_INVALID", "That item is not a paint.");
            return 1;
        }
        snprintf(skin, sizeof skin, "%lld", (long long) item);
        social_call(s, &P_BANNER, KN_DB_ARGS(me, cl, skin, value), data_done, rid_of(r));
    } else if (gapi_is(r, "DELETE", "/clans/:id/logo")) {
        social_call(s, &P_LOGO_DELETE, KN_DB_ARGS(me, cl), data_done, rid_of(r));
    } else if (gapi_is(r, "POST", "/clans/:id/logo")) {
        gapi_error(s, r->rid, 501, "CLAN_LOGO_UNSUPPORTED", "Logo uploads are not supported on this server");
    } else if (gapi_is(r, "POST", "/clans/:id/transfer")) {
        const char *name = kn_json_str(r->body, "name", "");
        if (!name[0]) {
            gapi_error(s, r->rid, 400, "CLAN_TRANSFER_INVALID", "Please enter a player name");
            return 1;
        }
        social_call(s, &P_TRANSFER, KN_DB_ARGS(me, cl, name), data_done, rid_of(r));
    } else if (gapi_is(r, "DELETE", "/clans/:id/transfer")) {
        social_call(s, &P_TRANSFER_CANCEL, KN_DB_ARGS(me, cl), data_done, rid_of(r));
    } else if (gapi_is(r, "POST", "/clans/:id/transfer/respond")) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(r->body, "accept");
        social_call(s, &P_TRANSFER_RESPOND, KN_DB_ARGS(me, cl, cJSON_IsTrue(a) || (cJSON_IsNumber(a) && a->valuedouble != 0) ? "1" : "0"),
                    data_done, rid_of(r));
    } else {
        gapi_error(s, r->rid, 405, "METHOD_NOT_ALLOWED", "Method not allowed.");
    }
    return 1;
}
