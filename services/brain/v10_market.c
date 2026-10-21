/*
 * services/brain/v10_market.c - the v10 REST routes of the market, items, trades, moderation, mods,
 * KPD and the players' HTTP-only routes (v10.js 26857-27365; the inventory is doc/v10/api-inventory.md).
 *
 * KrunkNative keeps this economy on the social socket (economy.c: market pages, list / buy / unlist /
 * quick sell, trade offers, the KR ledger; db_kpd.c: the distress calls and demos). These HTTP routes
 * have no mapping to those frames, so each one answers one of three ways:
 *
 *   EMPTY      200, {"data": ...} with the shape v10's consumer reads when it has nothing (its own
 *              default; v10.js names the consumer). Nothing is invented: an empty list says "no records
 *              here", never a balance, price or owner.
 *   REFUSAL    403, {"error": {"code": "NOT_IMPLEMENTED", "message"}}: every write, and every read of an
 *              account KrunkNative does not map over HTTP (the caller's own data needs a session the root
 *              API does not resolve; another player's data is not looked up here).
 *   NOT_FOUND  404, {"error": {"code": "NOT_FOUND"}}: an item route whose {id} is not an index of
 *              assets/skins.json (acct_skin); a real source says the item is absent.
 *
 * Routes (method path: answer, v10.js caller line):
 *
 *   market (26857-26948)
 *     GET  /market/listings            EMPTY {listings: [], has_more: false}      (128405)
 *     GET  /market/inventory           REFUSAL  caller's inventory                (128416)
 *     GET  /market/sales               REFUSAL  caller's sales                    (134079)
 *     GET  /market/stats               EMPTY {stats: [], has_more: false}         (128424)
 *     GET  /market/items/:id/sales     EMPTY [] / NOT_FOUND unknown item          (no v10 caller)
 *     GET  /market/items/:id/sales-statistics  EMPTY [] / NOT_FOUND               (134931)
 *     POST /market/list                REFUSAL  write                             (128440)
 *     POST /market/purchase            REFUSAL  write                             (132836)
 *     POST /market/unlist              REFUSAL  write                             (132978)
 *     POST /market/quick-sell          REFUSAL  write                             (131580)
 *     GET  /market/black-market        EMPTY []                                   (128486)
 *     GET  /market/black-market/rank   EMPTY null: the caller's peak MMR is not mapped; v10 joins this
 *                                      with the black-market list in Promise.all, so a 403 here would
 *                                      blank the whole popup (v10.js 136255). null is the consumer's
 *                                      "no rank" (128494).
 *     POST /market/black-market/purchase  REFUSAL write                           (136215)
 *     GET  /market/history             REFUSAL  caller's history ("log in to view your history", 131476)
 *
 *   items (26957-26980)
 *     GET  /items/:id/view             EMPTY {stats: null, owners: [], sales: [], listings: [], stackable:
 *                                      false, wearable: false} (v10's own default, 128558) / NOT_FOUND
 *     GET  /items/:id/sales            EMPTY [] / NOT_FOUND                       (129914)
 *     GET  /items/:id/instances        EMPTY [] / NOT_FOUND                       (128573)
 *     POST /items/:id/paint            REFUSAL  write                             (128724)
 *     POST /items/:id/style            REFUSAL  write                             (128924)
 *
 *   trades (27047-27078)
 *     GET  /trades/history             REFUSAL  caller's trades                   (108327)
 *     POST /trades/create              REFUSAL  write                             (109194)
 *     POST /trades/:id/accept          REFUSAL  write                             (135709)
 *     POST /trades/:id/cancel          REFUSAL  write                             (108339)
 *     POST /trades/decline-all         REFUSAL  write                             (135779)
 *     GET  /trades/inventory/:name     REFUSAL  another account's inventory       (109226)
 *     GET  /trades/player-search       REFUSAL  account lookup (the social tunnel's /players/search)
 *                                                                                 (108349)
 *
 *   moderation (27087-27098)
 *     GET  /moderation/chat/:name      REFUSAL  another account's chat ban       (110003, 217008)
 *     POST /moderation/chat            REFUSAL  write                             (109549)
 *     POST /moderation/flag            REFUSAL  write                             (109570)
 *
 *   mods (27195-27229)
 *     POST /mods/publish/key           REFUSAL  write                             (213947)
 *     GET  /mods/by-name/:name         REFUSAL  a published mod record is not on the v10 root (107663)
 *     POST /mods/:id/vote              REFUSAL  write                             (no v10 caller)
 *     POST /mods/:id/feature           REFUSAL  write                             (107604)
 *     DELETE /mods/:id                 REFUSAL  write                             (107616)
 *     POST /mods/publish               REFUSAL  write, the multipart upload       (no v10 caller)
 *
 *   KPD (27329-27354)
 *     GET  /kpd/demos                  EMPTY {rows: [], more: false} (policeRes code 4, 216006)
 *     GET  /kpd/demos/:id              REFUSAL  an officer's demo view (ct token, officer account; 222638)
 *     POST /kpd/demos/:id/vote         REFUSAL  write                             (no v10 caller)
 *     GET  /kpd/reviewers              EMPTY {month: [], all: []} (policeRes code 5, 216013)
 *
 *   players, HTTP only (27123-27182; the profile and /players/search are the social tunnel's)
 *     GET  /players/:name/sales        REFUSAL  another account's sales           (109908)
 *     GET  /players/:name/listings     REFUSAL  another account's listings        (110044)
 *     POST /players/:name/verify       REFUSAL  write                             (109590)
 *     POST /players/:name/gift         REFUSAL  write                             (109361)
 *
 * The 403 messages are short reasons for the API reader. v10's gapi wrapper (v10.js 26094) throws on
 * any status >= 500, so v10 shows a generic "Server error" for these rather than the message.
 */
#include "v10_market.h"

#include <stdlib.h>
#include <string.h>

#include "acct_internal.h"
#include "v10_api.h"

/* the refusals' reasons (the handler's ud) */
#define WHY_MARKET_WRITE "Market writes are not served over HTTP"
#define WHY_ITEM_WRITE "Item paint and style are not served over HTTP"
#define WHY_TRADE_WRITE "Trade writes are not served over HTTP"
#define WHY_MOD_WRITE "Mod publishing, voting and removal are not served over HTTP"
#define WHY_MOD_BY_NAME "Mod records are not served on the v10 root"
#define WHY_KPD_WRITE "KPD votes are not served over HTTP"
#define WHY_KPD_OFFICER "A KPD demo view needs an officer account, which HTTP does not map here"
#define WHY_ACCOUNT_READ "Needs the caller's account, which HTTP does not map here"
#define WHY_OTHER_ACCOUNT "Needs another account's data, which HTTP does not map here"
#define WHY_PLAYER_WRITE "Player verify and gift are not served over HTTP"
#define WHY_CHAT_BAN "Chat bans are not served over HTTP"

static void h_refuse(kn_http_req *req, void *ud) {
    v10_reply_error(req, 403, "NOT_IMPLEMENTED", (const char *) ud);
}

/* the {id} of an item route is an index of assets/skins.json; anything else is a 404 */
static int item_known(kn_http_req *req) {
    const char *id = kn_http_param(req, "id");
    if (id && *id) {
        char *end = NULL;
        const long long index = strtoll(id, &end, 10);
        if (end && !*end && index >= 0 && acct_skin((int64_t) index)) return 1;
    }
    v10_reply_error(req, 404, "NOT_FOUND", "Item not found");
    return 0;
}

/* ---- EMPTY reads */

static void h_market_listings(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "listings", cJSON_CreateArray());
    cJSON_AddFalseToObject(o, "has_more");
    v10_reply_data(req, 200, o);
}

static void h_market_stats(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "stats", cJSON_CreateArray());
    cJSON_AddFalseToObject(o, "has_more");
    v10_reply_data(req, 200, o);
}

static void h_empty_list(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_data(req, 200, cJSON_CreateArray());
}

/* an item's sales or instances: a list, 404 for an id that is not an item */
static void h_item_list(kn_http_req *req, void *ud) {
    (void) ud;
    if (!item_known(req)) return;
    v10_reply_data(req, 200, cJSON_CreateArray());
}

static void h_black_market_rank(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_data(req, 200, NULL); /* null: no rank (v10.js 128494) */
}

static void h_item_view(kn_http_req *req, void *ud) {
    (void) ud;
    if (!item_known(req)) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNullToObject(o, "stats");
    cJSON_AddItemToObject(o, "owners", cJSON_CreateArray());
    cJSON_AddItemToObject(o, "sales", cJSON_CreateArray());
    cJSON_AddItemToObject(o, "listings", cJSON_CreateArray());
    cJSON_AddFalseToObject(o, "stackable");
    cJSON_AddFalseToObject(o, "wearable");
    v10_reply_data(req, 200, o);
}

static void h_kpd_demos(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "rows", cJSON_CreateArray());
    cJSON_AddFalseToObject(o, "more");
    v10_reply_data(req, 200, o);
}

static void h_kpd_reviewers(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "month", cJSON_CreateArray());
    cJSON_AddItemToObject(o, "all", cJSON_CreateArray());
    v10_reply_data(req, 200, o);
}

int v10_market_init(Brain *b) {
    /* market */
    kn_http_route(b->http, "GET", "/market/listings", h_market_listings, b);
    kn_http_route(b->http, "GET", "/market/inventory", h_refuse, (void *) WHY_ACCOUNT_READ);
    kn_http_route(b->http, "GET", "/market/sales", h_refuse, (void *) WHY_ACCOUNT_READ);
    kn_http_route(b->http, "GET", "/market/stats", h_market_stats, b);
    kn_http_route(b->http, "GET", "/market/items/:id/sales", h_item_list, b);
    kn_http_route(b->http, "GET", "/market/items/:id/sales-statistics", h_item_list, b);
    kn_http_route(b->http, "POST", "/market/list", h_refuse, (void *) WHY_MARKET_WRITE);
    kn_http_route(b->http, "POST", "/market/purchase", h_refuse, (void *) WHY_MARKET_WRITE);
    kn_http_route(b->http, "POST", "/market/unlist", h_refuse, (void *) WHY_MARKET_WRITE);
    kn_http_route(b->http, "POST", "/market/quick-sell", h_refuse, (void *) WHY_MARKET_WRITE);
    kn_http_route(b->http, "GET", "/market/black-market", h_empty_list, b);
    kn_http_route(b->http, "GET", "/market/black-market/rank", h_black_market_rank, b);
    kn_http_route(b->http, "POST", "/market/black-market/purchase", h_refuse, (void *) WHY_MARKET_WRITE);
    kn_http_route(b->http, "GET", "/market/history", h_refuse, (void *) WHY_ACCOUNT_READ);

    /* items */
    kn_http_route(b->http, "GET", "/items/:id/view", h_item_view, b);
    kn_http_route(b->http, "GET", "/items/:id/sales", h_item_list, b);
    kn_http_route(b->http, "GET", "/items/:id/instances", h_item_list, b);
    kn_http_route(b->http, "POST", "/items/:id/paint", h_refuse, (void *) WHY_ITEM_WRITE);
    kn_http_route(b->http, "POST", "/items/:id/style", h_refuse, (void *) WHY_ITEM_WRITE);

    /* trades */
    kn_http_route(b->http, "GET", "/trades/history", h_refuse, (void *) WHY_ACCOUNT_READ);
    kn_http_route(b->http, "POST", "/trades/create", h_refuse, (void *) WHY_TRADE_WRITE);
    kn_http_route(b->http, "POST", "/trades/:id/accept", h_refuse, (void *) WHY_TRADE_WRITE);
    kn_http_route(b->http, "POST", "/trades/:id/cancel", h_refuse, (void *) WHY_TRADE_WRITE);
    kn_http_route(b->http, "POST", "/trades/decline-all", h_refuse, (void *) WHY_TRADE_WRITE);
    kn_http_route(b->http, "GET", "/trades/inventory/:name", h_refuse, (void *) WHY_OTHER_ACCOUNT);
    kn_http_route(b->http, "GET", "/trades/player-search", h_refuse, (void *) WHY_OTHER_ACCOUNT);

    /* moderation */
    kn_http_route(b->http, "GET", "/moderation/chat/:name", h_refuse, (void *) WHY_OTHER_ACCOUNT);
    kn_http_route(b->http, "POST", "/moderation/chat", h_refuse, (void *) WHY_CHAT_BAN);
    kn_http_route(b->http, "POST", "/moderation/flag", h_refuse, (void *) WHY_CHAT_BAN);

    /* mods */
    kn_http_route(b->http, "POST", "/mods/publish/key", h_refuse, (void *) WHY_MOD_WRITE);
    kn_http_route(b->http, "GET", "/mods/by-name/:name", h_refuse, (void *) WHY_MOD_BY_NAME);
    kn_http_route(b->http, "POST", "/mods/:id/vote", h_refuse, (void *) WHY_MOD_WRITE);
    kn_http_route(b->http, "POST", "/mods/:id/feature", h_refuse, (void *) WHY_MOD_WRITE);
    kn_http_route(b->http, "DELETE", "/mods/:id", h_refuse, (void *) WHY_MOD_WRITE);
    kn_http_route(b->http, "POST", "/mods/publish", h_refuse, (void *) WHY_MOD_WRITE);

    /* KPD */
    kn_http_route(b->http, "GET", "/kpd/demos", h_kpd_demos, b);
    kn_http_route(b->http, "GET", "/kpd/demos/:id", h_refuse, (void *) WHY_KPD_OFFICER);
    kn_http_route(b->http, "POST", "/kpd/demos/:id/vote", h_refuse, (void *) WHY_KPD_WRITE);
    kn_http_route(b->http, "GET", "/kpd/reviewers", h_kpd_reviewers, b);

    /* players, HTTP only */
    kn_http_route(b->http, "GET", "/players/:name/sales", h_refuse, (void *) WHY_OTHER_ACCOUNT);
    kn_http_route(b->http, "GET", "/players/:name/listings", h_refuse, (void *) WHY_OTHER_ACCOUNT);
    kn_http_route(b->http, "POST", "/players/:name/verify", h_refuse, (void *) WHY_PLAYER_WRITE);
    kn_http_route(b->http, "POST", "/players/:name/gift", h_refuse, (void *) WHY_PLAYER_WRITE);
    return 0;
}
