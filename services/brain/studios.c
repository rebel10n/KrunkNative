/*
 * services/brain/studios.c - Krunker v10.0.2's Studios API (v10 gapi /studios paths, the client's
 * this.studios at v10.js 27360-27520) over the gapi tunnel of the social socket (social_gapi.c).
 * Storage and rules: db_studios.c (P_STUDIO, migration 0330_studios).
 *
 *   POST   /studios {name}                          createStudio          {id, name, role}
 *   GET    /studios/mine                            getMyStudios          [{id, name, owner, member_count, role}]
 *   GET    /studios/invites                         getMyStudioInvites    [{invite_id, studio_id, studio_name, invited_by, expires_at}]
 *   POST   /studios/invites/<id>/accept | /decline  accept / declineStudioInvite
 *   GET    /studios/<id>                            getStudio             the studio (members, invites, maps, currencies)
 *   DELETE /studios/<id>                            deleteStudio
 *   POST   /studios/<id>/members {player_name}      addStudioMember       (an invite)
 *   DELETE /studios/<id>/members/<name>             removeStudioMember    (leave: your own name)
 *   DELETE /studios/<id>/invites/<invite id>        cancelStudioInvite
 *   POST   /studios/<id>/transfer {player_name}     transferStudio
 *   POST   /studios/<id>/maps {map_id}              addStudioMap
 *   DELETE /studios/<id>/maps/<map id>              removeStudioMap
 *   POST   /studios/<id>/currencies {name, symbol, decimals}  createStudioCurrency
 *   POST   /studios/<id>/currencies/<cid>/publish | /unpublish
 *   POST   /studios/<id>/currencies/<cid>/edit {name, symbol, decimals}  editStudioCurrency
 *   GET    /studios/<id>/analytics                  getStudioAnalytics
 *   GET    /studios/<id>/skins                      getStudioSkins        []
 *   POST   /studios/<id>/skins/import {custom_skin_id}         refused: KrunkNative has no custom skins
 *   POST   /studios/<id>/skins/<sid>/publish | /unpublish      refused: no such skin
 *   GET    /studios/<id>/monetization               getStudioMonetization the status
 *   POST   /studios/<id>/monetization {description, map_ids}  applyStudioMonetization
 *   DELETE /studios/<id>/monetization               cancelStudioMonetization
 *
 * Answers keep v10's {data} / {error: {code, message}} shape; the window shows error.message.
 * Deviation from v10: the requests ride the authenticated social socket instead of HTTPS with a
 * bearer token (as every gapi route, social_gapi.c). Writes are rate limited per account.
 */
#include "acct_internal.h"
#include "studios.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hot.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

/* the refusal codes of db_studios.c: HTTP status and the message the window shows */
static const struct {
    const char *code;
    int status;
    const char *message;
} k_errors[] = {
    {"STUDIO_NOT_FOUND", 404, "Studio not found."},
    {"STUDIO_FORBIDDEN", 403, "Only the studio owner can do that."},
    {"STUDIO_NAME_INVALID", 400, "Use 3-30 letters, numbers or underscores."},
    {"STUDIO_NAME_TAKEN", 409, "That studio name is already taken."},
    {"STUDIO_LIMIT", 400, "You can own up to 3 studios."},
    {"STUDIO_PLAYER_NOT_FOUND", 404, "That player could not be found."},
    {"STUDIO_INVITE_SELF", 400, "You are already in this studio."},
    {"STUDIO_ALREADY_MEMBER", 409, "That player is already a member of this studio."},
    {"STUDIO_ALREADY_INVITED", 409, "That player already has a pending invite."},
    {"STUDIO_FULL", 400, "A studio can hold up to 20 members, counting pending invites."},
    {"STUDIO_INVITE_NOT_FOUND", 404, "That invite no longer exists."},
    {"STUDIO_NOT_MEMBER", 404, "That player is not a member of this studio."},
    {"STUDIO_OWNER_LEAVE", 400, "The owner can't leave the studio. Transfer it or delete it instead."},
    {"STUDIO_TRANSFER_SELF", 400, "You already own this studio."},
    {"STUDIO_TRANSFER_LIMIT", 400, "That player already owns 3 studios."},
    {"STUDIO_MAP_NOT_FOUND", 404, "Game not found."},
    {"STUDIO_MAP_NOT_YOURS", 403, "You can only assign your own games."},
    {"STUDIO_MAP_TAKEN", 409, "A game can only belong to one studio at a time."},
    {"STUDIO_MAP_NOT_ASSIGNED", 404, "That game is not in this studio."},
    {"STUDIO_CURRENCY_INVALID", 400, "Symbols are 2-4 uppercase letters. KR, JNK and RP are reserved."},
    {"STUDIO_CURRENCY_LIMIT", 400, "Up to 5 currencies per studio."},
    {"STUDIO_CURRENCY_TAKEN", 409, "This studio already has a currency with that symbol."},
    {"STUDIO_CURRENCY_NOT_FOUND", 404, "Currency not found."},
    {"STUDIO_SKIN_NOT_FOUND", 404, "Skin not found."},
    {"MONETIZATION_INVALID", 400, "Describe what you want to monetize in 10 to 500 characters."},
    {"MONETIZATION_MAPS_INVALID", 400, "Example games must be games of this studio (up to 5)."},
    {"MONETIZATION_NOT_ELIGIBLE", 400, "Meet every requirement above to apply."},
    {"MONETIZATION_PENDING", 409, "The Krunker team is reviewing your application."},
    {"MONETIZATION_ENABLED", 409, "Monetization is already enabled for this studio."},
    {"MONETIZATION_BLOCKED", 403, "This studio can no longer apply for monetization."},
    {"MONETIZATION_COOLDOWN", 400, "You can't apply again yet."},
    {"MONETIZATION_NONE", 404, "There is no application to withdraw."},
};

/* per-account write limiter (60 writes / minute), shared with Redis when it is up */
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
    return hot_limit("studios", key, 60, 60, local);
}

static void done(SocialSession *s, kn_db_result *res, void *extra) {
    const int64_t rid = *(int64_t *) extra;
    if (!kn_db_ok(res)) {
        const char *msg = acct_db_error(res, "studios");
        for (size_t i = 0; i < sizeof k_errors / sizeof k_errors[0]; i++)
            if (!strcmp(msg, k_errors[i].code)) {
                gapi_error(s, rid, k_errors[i].status, k_errors[i].code, k_errors[i].message);
                return;
            }
        gapi_error(s, rid, 500, "STUDIO_ERROR", msg);
        return;
    }
    cJSON *v = acct_db_json(res);
    gapi_reply(s, rid, v ? v : cJSON_CreateObject());
}

static void call(SocialSession *s, const GapiReq *r, const char *op, const char *a, const char *b, const char *c) {
    int64_t *rid = malloc(sizeof *rid);
    if (!rid) return;
    *rid = r->rid;
    char me[24];
    social_viewer(s, me);
    social_call(s, &P_STUDIO, KN_DB_ARGS(op, me, a ? a : "", b ? b : "", c ? c : ""), done, rid);
}

static int is_id(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

int studios_gapi(SocialSession *s, const GapiReq *r) {
    if (r->nseg < 1 || strcmp(r->seg[0], "studios")) return 0;
    if (s->account_id <= 0) {
        /* the Studios window reads these on open: a guest has no studios and no invites, so the
         * empty list (v10 reads data || []) and not an error the window would show */
        if (gapi_is(r, "GET", "/studios/mine") || gapi_is(r, "GET", "/studios/invites")) {
            gapi_reply(s, r->rid, cJSON_CreateArray());
            return 1;
        }
        gapi_error(s, r->rid, 401, "UNAUTHORIZED", "You must be signed in.");
        return 1;
    }
    if (strcmp(r->method, "GET") && !write_ok(s->account_id)) {
        gapi_error(s, r->rid, 429, "RATE_LIMITED", "Rate limit exceeded. Try again later.");
        return 1;
    }
    char num[24];
    if (gapi_is(r, "POST", "/studios")) {
        char name[64];
        snprintf(name, sizeof name, "%s", kn_json_str(r->body, "name", ""));
        call(s, r, "create", name, NULL, NULL);
    } else if (gapi_is(r, "GET", "/studios/mine")) {
        call(s, r, "mine", NULL, NULL, NULL);
    } else if (gapi_is(r, "GET", "/studios/invites")) {
        call(s, r, "invites", NULL, NULL, NULL);
    } else if (gapi_is(r, "POST", "/studios/invites/:id/accept") || gapi_is(r, "POST", "/studios/invites/:id/decline")) {
        if (!is_id(r->seg[2])) gapi_error(s, r->rid, 404, "STUDIO_INVITE_NOT_FOUND", "That invite no longer exists.");
        else call(s, r, "invite_answer", r->seg[2], strcmp(r->seg[3], "accept") ? "0" : "1", NULL);
    } else if (r->nseg >= 2 && !is_id(r->seg[1])) {
        gapi_error(s, r->rid, 404, "STUDIO_NOT_FOUND", "Studio not found.");
    } else if (gapi_is(r, "GET", "/studios/:id")) {
        call(s, r, "get", r->seg[1], NULL, NULL);
    } else if (gapi_is(r, "DELETE", "/studios/:id")) {
        call(s, r, "delete", r->seg[1], NULL, NULL);
    } else if (gapi_is(r, "POST", "/studios/:id/members")) {
        call(s, r, "invite", r->seg[1], kn_json_str(r->body, "player_name", ""), NULL);
    } else if (gapi_is(r, "DELETE", "/studios/:id/members/:name")) {
        call(s, r, "member_remove", r->seg[1], r->seg[3], NULL);
    } else if (gapi_is(r, "DELETE", "/studios/:id/invites/:invite")) {
        call(s, r, "invite_cancel", r->seg[1], is_id(r->seg[3]) ? r->seg[3] : "0", NULL);
    } else if (gapi_is(r, "POST", "/studios/:id/transfer")) {
        call(s, r, "transfer", r->seg[1], kn_json_str(r->body, "player_name", ""), NULL);
    } else if (gapi_is(r, "POST", "/studios/:id/maps")) {
        snprintf(num, sizeof num, "%lld", (long long) kn_json_int(r->body, "map_id", 0));
        call(s, r, "map_add", r->seg[1], num, NULL);
    } else if (gapi_is(r, "DELETE", "/studios/:id/maps/:map")) {
        call(s, r, "map_remove", r->seg[1], is_id(r->seg[3]) ? r->seg[3] : "0", NULL);
    } else if (gapi_is(r, "POST", "/studios/:id/currencies")) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "name", kn_json_str(r->body, "name", ""));
        cJSON_AddStringToObject(c, "symbol", kn_json_str(r->body, "symbol", ""));
        cJSON_AddNumberToObject(c, "decimals", (double) kn_json_int(r->body, "decimals", -1));
        char *text = cJSON_PrintUnformatted(c);
        cJSON_Delete(c);
        call(s, r, "currency_create", r->seg[1], text ? text : "{}", NULL);
        free(text);
    } else if (gapi_is(r, "POST", "/studios/:id/currencies/:cid/publish") || gapi_is(r, "POST", "/studios/:id/currencies/:cid/unpublish")) {
        call(s, r, "currency_active", r->seg[1], is_id(r->seg[3]) ? r->seg[3] : "0", strcmp(r->seg[4], "publish") ? "0" : "1");
    } else if (gapi_is(r, "POST", "/studios/:id/currencies/:cid/edit")) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "name", kn_json_str(r->body, "name", ""));
        cJSON_AddStringToObject(c, "symbol", kn_json_str(r->body, "symbol", ""));
        cJSON_AddNumberToObject(c, "decimals", (double) kn_json_int(r->body, "decimals", -1));
        char *text = cJSON_PrintUnformatted(c);
        cJSON_Delete(c);
        call(s, r, "currency_edit", r->seg[1], is_id(r->seg[3]) ? r->seg[3] : "0", text ? text : "{}");
        free(text);
    } else if (gapi_is(r, "GET", "/studios/:id/analytics")) {
        call(s, r, "analytics", r->seg[1], NULL, NULL);
    } else if (gapi_is(r, "GET", "/studios/:id/skins")) {
        call(s, r, "skins", r->seg[1], NULL, NULL);
    } else if (gapi_is(r, "POST", "/studios/:id/skins/import") || gapi_is(r, "POST", "/studios/:id/skins/:sid/publish") ||
               gapi_is(r, "POST", "/studios/:id/skins/:sid/unpublish")) {
        /* KrunkNative has no Skin Manager (v10 /skins custom skins): no skin can exist */
        gapi_error(s, r->rid, 404, "STUDIO_SKIN_NOT_FOUND", "Skin not found.");
    } else if (gapi_is(r, "GET", "/studios/:id/monetization")) {
        call(s, r, "monetization", r->seg[1], NULL, NULL);
    } else if (gapi_is(r, "POST", "/studios/:id/monetization")) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "description", kn_json_str(r->body, "description", ""));
        const cJSON *ids = cJSON_GetObjectItemCaseSensitive(r->body, "map_ids");
        cJSON_AddItemToObject(c, "map_ids", cJSON_IsArray(ids) ? cJSON_Duplicate(ids, 1) : cJSON_CreateArray());
        char *text = cJSON_PrintUnformatted(c);
        cJSON_Delete(c);
        call(s, r, "monetization_apply", r->seg[1], text ? text : "{}", NULL);
        free(text);
    } else if (gapi_is(r, "DELETE", "/studios/:id/monetization")) {
        call(s, r, "monetization_cancel", r->seg[1], NULL, NULL);
    } else {
        gapi_error(s, r->rid, 404, "NOT_FOUND", "Not found.");
    }
    return 1;
}
