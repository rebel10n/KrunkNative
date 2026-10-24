/*
 * services/brain/v10_market.c - the v10 REST routes of the market, the items, the trades and the players'
 * HTTP routes (v10.js 26857-27182; the inventory is doc/v10/api-inventory.md), and the moderation, mods and
 * KPD routes that refuse or answer empty here (their rows belong to the mods family and are unchanged).
 *
 * KrunkNative keeps this economy on the social socket (economy.c: market pages, list / buy / unlist /
 * quick sell, trade offers, the KR ledger; profiles.c: the player's sales and verify; social_gapi.c: the
 * player search). Every route here runs the socket's own handler on an HTTP-bound session
 * (social_capture_new, SocialCapture in acct_internal.h): the handler sends its frames into this file,
 * which maps them to the REST answer. So each check, error message and rate limit is the socket's.
 *
 *   The caller is the request's Bearer token (ranked.c's get_token and with_account): a caller-owned read
 *   answers its empty shape without one (a guest owns nothing); a write needs a login (401). A write loads
 *   the account's email state and passes the socket's feature gate (social_email_gate) before its handler,
 *   as the socket's dispatch does.
 *   A socket error ["po", null, message] becomes an error with v10.0.3's code for that message (err_reply:
 *   MARKET_*, TRADE_*, PLAYERS_*, GIFT_*): v10.0.3's translator shows the text of a code it knows and "An
 *   unexpected error occurred." for any other code (deob-10.0.3.js:117395-117413, codes 117329-117393), so a
 *   message with no 10.0.3 code is sent as a text error, which it shows as it is. 401 for "Must be logged in",
 *   404 for "Player not found", 400 for the rest. The paint route keeps {code, message}: its consumer reads
 *   error.message (deob-10.0.3.js:122139).
 *
 * Routes (method path: answer, v10.js caller line):
 *
 *   market (26857-26948)
 *     GET  /market/listings            listings (economy "fil": items, sort, cursor_id, limit) (128405)
 *     GET  /market/inventory           the caller's inventory [{i, cnt}] (economy "inventory"); guest [] (128416)
 *     GET  /market/sales               the caller's open listings (economy "sales"); guest [] (134079)
 *     GET  /market/stats               the stats rows per skin, filtered (economy "stats") (128424)
 *     GET  /market/items/:id/sales     the item's sales info {ind, onS, inC, high, low, owners} (no v10 caller)
 *     GET  /market/items/:id/sales-statistics  daily [{d, f, t}] (economy "st") (134931)
 *     POST /market/list                economy "li": {cnt, funds, listingId, fee} (128440)
 *     POST /market/purchase            economy "pi": {f, i, sid, m, price} (132836)
 *     POST /market/unlist              economy "uli": {response} (132978)
 *     POST /market/quick-sell          economy "qsi": {funds, junk, count} (131580); action 0 KR, 1 Junk, 2 destroy
 *     GET  /market/black-market        the caller's black-market purchases: [skin index] (skin_transactions) (128486)
 *     GET  /market/black-market/rank   {peak_mmr} from ranked_profiles; null for a guest or no rank (128494)
 *     POST /market/black-market/purchase  economy "pbm": {item, kr, funds} (136215)
 *     GET  /market/history             the caller's market history {history, has_more, next_cursor} (131476)
 *
 *   items (26957-26980)
 *     GET  /items/:id/view             {stats, owners, sales, listings, stackable, wearable} (128558)
 *     GET  /items/:id/sales?range=     daily [{d, f, t}] (economy "st", range index) (129914)
 *     GET  /items/:id/instances?sort=  [] : KrunkNative keeps no per-copy instances (128573)
 *     POST /items/:id/paint            {item_id, paint_item_id}: paints an owned copy (or an unpainted unit of the item,
 *                                      which becomes a copy); 400 for a non-paint (128724; doc/v10/paint-copy.md)
 *     POST /items/:id/style            REFUSAL: no per-copy style state (128924)
 *
 *   trades (27047-27078)
 *     GET  /trades/history             {open, completed, completed_has_more} (economy's trade page) (108327)
 *     POST /trades/create              economy "ctr": {trid, funds, fee} (109194)
 *     POST /trades/:id/accept          economy "trA": {trid, funds} (135709)
 *     POST /trades/:id/cancel          economy "trC": {success, trid, funds} (108339)
 *     POST /trades/decline-all         economy "trCA": {success} (135779)
 *     GET  /trades/inventory/:name     economy "ri": {my_inventory, their_inventory} (109226)
 *     GET  /trades/player-search?name= social_gapi /players/search, the exact name: the player (108349)
 *
 *   players (27123-27182; the profile and the search's other routes are the social tunnel's)
 *     GET  /players/:name/sales?range= profiles "pst": daily [{d, f}] (109908)
 *     GET  /players/:name/listings     economy "uml": listing rows (110044)
 *     POST /players/:name/verify       profiles "ver": {featured}, or null when nothing changed (109590)
 *     POST /players/:name/gift         economy "kr": {funds, amount, fee} (109361)
 *
 *   moderation (27087-27098), mods (27195-27229), KPD (27329-27354): REFUSAL or EMPTY, unchanged here.
 *
 * The 403 messages are short reasons for the API reader. v10's gapi wrapper (v10.js 26094) throws on
 * any status >= 500, so v10 shows a generic "Server error" for those rather than the message.
 */
#include "v10_market.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "acct_internal.h"
#include "knsvc/json.h"
#include "v10_api.h"
#include "v10_paint.h"

/* the most item ids a listings request filters on (items= and item_id=) */
#define MAX_ITEM_IDS 256

/* the refusals' reasons (the handler's ud) */
#define WHY_ITEM_STYLE "Item style is not served over HTTP: KrunkNative keeps no per-copy style state"

/* the day ranges of v10's sales graphs (v10.js 10075: the range index picks one) */
static const int k_day_ranges[] = {7, 14, 21, 30, 60, 90, 120};
#define DAY_RANGES ((int) (sizeof k_day_ranges / sizeof k_day_ranges[0]))

static Brain *s_brain;

/* the {id} of an item route is an index of assets/skins.json; anything else is a 404 */
static int item_known(kn_http_req *req) {
    const char *id = kn_http_param(req, "id");
    if (id && *id) {
        char *end = NULL;
        const long long index = strtoll(id, &end, 10);
        if (end && !*end && index >= 0 && acct_skin((int64_t) index)) return 1;
    }
    v10_reply_error(req, 404, "ITEMS_INVALID_ID", "Item not found"); /* "That item could not be found." (deob-10.0.3.js:117384) */
    return 0;
}

/* ---- request helpers */

static int64_t query_int(kn_http_req *req, const char *key, int64_t def) {
    char v[32];
    if (kn_http_query_get(req, key, v, sizeof v) <= 0) return def;
    char *end = NULL;
    const long long x = strtoll(v, &end, 10);
    return end && end != v ? (int64_t) x : def;
}

static void query_text(kn_http_req *req, const char *key, char *out, size_t cap) {
    if (kn_http_query_get(req, key, out, cap) <= 0) out[0] = 0;
}

/* "1,3,5" -> up to max ids (the v10 itemIds and the filter lists; a search's 200 ids fit) */
static int csv_ids(kn_http_req *req, const char *key, int64_t *out, int max) {
    char csv[MAX_ITEM_IDS * 8];
    int n = 0;
    if (kn_http_query_get(req, key, csv, sizeof csv) <= 0) return 0;
    for (char *t = strtok(csv, ","); t && n < max; t = strtok(NULL, ",")) {
        char *end = NULL;
        const long long v = strtoll(t, &end, 10);
        if (end && end != t && v >= 0) out[n++] = (int64_t) v;
    }
    return n;
}

/* a rarity / weapon / type list -> its bit mask (economy.c's key_mask: index 0..31) */
static unsigned csv_bits(kn_http_req *req, const char *key) {
    int64_t ids[32];
    const int n = csv_ids(req, key, ids, 32);
    unsigned mask = 0;
    for (int i = 0; i < n; i++)
        if (ids[i] < 32) mask |= 1u << ids[i];
    return mask;
}

/* a key_mask object {"3": true, ...} for the economy's filter frames */
static cJSON *bits_object(unsigned bits) {
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < 32; i++) {
        if (!(bits & (1u << i))) continue;
        char k[8];
        snprintf(k, sizeof k, "%d", i);
        cJSON_AddBoolToObject(o, k, 1);
    }
    return o;
}

/* the frame's extension object: the client's idempotency key ("op"), when the body has one */
static cJSON *ext_of(const cJSON *body) {
    cJSON *e = cJSON_CreateObject();
    const char *op = kn_json_str(body, "op", NULL);
    if (op && *op) cJSON_AddStringToObject(e, "op", op);
    return e;
}

/* a copy of src[key], or null when absent */
static cJSON *copy_or_null(const cJSON *src, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(src, key);
    return v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull();
}

/* copies src[key] into o when present */
static void put_if(cJSON *o, const cJSON *src, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(src, key);
    if (v) cJSON_AddItemToObject(o, key, cJSON_Duplicate(v, 1));
}

/* the body of a POST (an object; an empty one when there is none) */
static cJSON *body_of(kn_http_req *req) {
    cJSON *b = kn_http_body_json(req);
    if (!cJSON_IsObject(b)) {
        cJSON_Delete(b);
        b = cJSON_CreateObject();
    }
    return b;
}

static void add_int(cJSON *f, int64_t v) { cJSON_AddItemToArray(f, cJSON_CreateNumber((double) v)); }
static void add_str(cJSON *f, const char *s) { cJSON_AddItemToArray(f, cJSON_CreateString(s ? s : "")); }
static void add_null(cJSON *f) { cJSON_AddItemToArray(f, cJSON_CreateNull()); }

/* the first column of a row, as an integer (NULL: 0); the rows are copies (db_done) */
static int64_t row_int(const cJSON *row) {
    const cJSON *v = row ? row->child : NULL;
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : 0;
}

/* ---- the operation: a socket handler on an HTTP-bound session
 *
 * An Op is one REST request. Its session (social_capture_new) has no socket: each socket handler it runs
 * sends its frames into op->frames (op_emit), and an async step (a handler's DB call, a lookup) holds the
 * session busy. When the session is idle (busy 0) the next step runs; a step that leaves no next step
 * finishes the request, and the reply reads the captured frames. The op owns the session's one reference
 * (op_finish drops it). Nothing reaches the op after it finished: op_finish detaches the session. */

typedef struct Op Op;
typedef void (*OpStep)(Op *op);
typedef void (*OpReply)(Op *op);

/* the route's parameters (the path, the query and the body), read before the caller is resolved */
typedef struct Args {
    int64_t id;                      /* the {id} of an item or a trade */
    int64_t num;                     /* a range index, days, the featured flag */
    int64_t offset, limit, cursor, season;
    int include_open;
    unsigned rar, wep, typ;          /* the listing filters (bit masks) */
    int desc;                        /* price_desc */
    int64_t items[MAX_ITEM_IDS];     /* items= (a search sends up to 200, deob-10.0.3.js:149528-149540) and item_id= */
    int nitems;
    char name[64];                   /* a player's name */
    char text[96];                   /* searchTerm */
    int ectx;                        /* ErrCtx: the codes the route's errors map to */
    char sort[16];                   /* the stats grid's sort (rarity, name, season, type, new, old) */
} Args;

struct Op {
    kn_http_req *req;
    SocialSession *s;                /* the HTTP-bound session; NULL once finished */
    SocialCapture cap;
    cJSON *frames;                   /* the frames the socket handlers sent, in order */
    cJSON *sent;                     /* the frames this request handed to a handler (kept to the end) */
    cJSON *cur;                      /* the frame of the handler op_call runs (a write's gate reads it) */
    cJSON *body;                     /* the request body (a JSON object) */
    SocialHandler handler;           /* what op_call runs */
    OpStep next;                     /* the step run once the session is idle; NULL = finish */
    OpReply reply;                   /* writes the HTTP reply from the frames */
    cJSON *rows;                     /* a DB step's rows (an array of objects), read by the next step */
    int dbok;                        /* the DB step succeeded */
    Args a;
    int64_t account;
    int write, required, pumping, finished;
    int fail_status;                 /* set: the reply is this error (the frames are not read) */
    const char *fail_code, *fail_msg;
};

static void op_pump(Op *op);

static void op_emit(void *ud, cJSON *frame) { cJSON_AddItemToArray(((Op *) ud)->frames, frame); }
static void op_idle(void *ud) { op_pump((Op *) ud); }

static cJSON *keep(Op *op, cJSON *frame) {
    cJSON_AddItemToArray(op->sent, frame);
    return frame;
}

/* the first captured frame with this label, or NULL */
static cJSON *op_frame(Op *op, const char *label) {
    cJSON *f;
    cJSON_ArrayForEach(f, op->frames) {
        const char *l = cJSON_IsArray(f) ? fa_str(f, 0) : NULL;
        if (l && !strcmp(l, label)) return f;
    }
    return NULL;
}

/* element i of the first captured frame with this label (NULL when either is absent) */
static cJSON *op_arg(Op *op, const char *label, int i) {
    cJSON *f = op_frame(op, label);
    return f ? cJSON_GetArrayItem(f, i) : NULL;
}

/* ---- errors: v10.0.3's codes for the economy's messages */

/* which of v10.0.3's code families a route's errors use */
typedef enum { ERR_MARKET = 0, ERR_TRADE, ERR_GIFT, ERR_OBJECT } ErrCtx;

/* the 10.0.3 code (deob-10.0.3.js:117329-117393) that says what the economy's message says, or NULL */
static const char *err_code(const char *msg, int ctx) {
    static const struct {
        const char *msg, *code;
    } k[] = {
        {"Item Unavailable", "MARKET_LISTING_GONE"},              /* "This listing is no longer available." */
        {"Listing not found", "MARKET_LISTING_GONE"},
        {"Cant buy your own item", "MARKET_SELF_PURCHASE"},
        {"Max Items on Sale", "MARKET_MAX_ITEMS"},
        {"Item cant be sold.", "MARKET_ITEM_NOT_SELLABLE"},
        {"Item cant be sold on krunker.", "MARKET_ITEM_NOT_SELLABLE"},
        {"Item cant be quick sold.", "MARKET_QUICK_SELL_NOT_SELLABLE"},
        {"Invalid offer", "TRADE_INVALID_OFFER"},                 /* "Your trade offer is empty or invalid." */
        {"No Items Selected", "TRADE_INVALID_OFFER"},
        {"Item cant be traded", "TRADE_ITEM_NOT_TRADABLE"},
        {"Player does not own these items", "TRADE_ITEM_NOT_OWNED"},
    };
    if (!msg) return NULL;
    if (!strcmp(msg, "Not enough KR")) return ctx == ERR_GIFT ? "GIFT_INSUFFICIENT_FUNDS" : "MARKET_INSUFFICIENT_FUNDS";
    if (!strcmp(msg, "Player not found")) return ctx == ERR_TRADE ? "TRADE_PLAYER_NOT_FOUND" : "PLAYERS_NOT_FOUND";
    /* 10.0.3's texts name level 10: only that gate maps (accounts.market_min_level / trade_min_level default 10) */
    if (!strcmp(msg, "Must be level 10")) return ctx == ERR_TRADE ? "TRADE_LEVEL_TOO_LOW" : ctx == ERR_MARKET ? "MARKET_LEVEL_TOO_LOW" : NULL;
    if (!strcmp(msg, "Slow down") && ctx == ERR_MARKET) return "MARKET_RATE_LIMITED";
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(msg, k[i].msg)) return k[i].code;
    return NULL;
}

/* the error reply: {code, message} with the 10.0.3 code; without one a text error, or {code: fallback, message}
 * on a route whose consumer reads error.message (ERR_OBJECT) */
static void err_reply(kn_http_req *req, int status, int ctx, const char *fallback, const char *msg) {
    const char *code = err_code(msg, ctx);
    if (code) v10_reply_error(req, status, code, msg);
    else if (ctx == ERR_OBJECT) v10_reply_error(req, status, fallback, msg);
    else v10_reply_text_error(req, status, msg);
}

/* ends the request with this error (its reply, not a socket frame) */
static void op_fail(Op *op, int status, const char *code, const char *msg) {
    op->fail_status = status;
    op->fail_code = code;
    op->fail_msg = msg;
    op->next = NULL;
}

/* the reply of a request that failed: the step's error, or the socket's ["po", null, message]; 1 when so */
static int op_failed(Op *op) {
    if (op->fail_msg) {
        if (op->fail_status >= 500) v10_reply_error(op->req, op->fail_status, op->fail_code, op->fail_msg);
        else err_reply(op->req, op->fail_status, op->a.ectx, op->fail_code, op->fail_msg);
        return 1;
    }
    const cJSON *po = op_frame(op, "po");
    if (!po) return 0;
    const cJSON *m = cJSON_GetArrayItem(po, 2);
    const char *msg = cJSON_IsString(m) ? m->valuestring : "Error";
    if (!strcmp(msg, "Must be logged in")) v10_reply_error(op->req, 401, "LOGIN_REQUIRED", msg); /* as pending_open's */
    else if (!strcmp(msg, "Player not found")) err_reply(op->req, 404, op->a.ectx, "NOT_FOUND", msg);
    else err_reply(op->req, 400, op->a.ectx, "BAD_REQUEST", msg);
    return 1;
}

static void op_finish(Op *op) {
    SocialSession *s = op->s;
    op->finished = 1;
    op->s = NULL;
    s->cap = NULL; /* no frame reaches the op after this: the session is only released below */
    if (op->reply) op->reply(op);
    else if (!op_failed(op)) v10_reply_error(op->req, 500, "INTERNAL", "No answer");
    social_release(s);
}

static void op_free(Op *op) {
    cJSON_Delete(op->frames);
    cJSON_Delete(op->sent);
    cJSON_Delete(op->body);
    cJSON_Delete(op->rows);
    free(op);
}

/* runs the steps while the session is idle; the request ends when a step leaves no next step */
static void op_pump(Op *op) {
    if (op->pumping || op->finished || !op->s) return;
    op->pumping = 1;
    SocialSession *s = social_retain(op->s); /* a synchronous completion must not free the session */
    while (!op->finished && s->busy == 0) {
        OpStep step = op->next;
        op->next = NULL;
        if (!step) {
            op_finish(op);
            break;
        }
        step(op);
    }
    op->pumping = 0;
    social_release(s);
    if (op->finished) op_free(op);
}

/* a DB step: the session is held until the result comes back. The DB layer frees the result when this
 * returns, so the rows are copied (op->rows); the next step reads them. */
static void db_done(kn_db_result *res, void *ud) {
    Op *op = ud;
    op->dbok = kn_db_ok(res);
    op->rows = op->dbok ? kn_db_result_to_json(res) : NULL;
    social_unhold(op->s);
}

static int op_query(Op *op, const kn_sql *q, int n, const char *const *p) {
    cJSON_Delete(op->rows);
    op->rows = NULL;
    op->dbok = 0;
    social_hold(op->s);
    if (kn_db_query_sql(s_brain->db, q, n, p, db_done, op) != 0) {
        social_unhold(op->s);
        return -1;
    }
    return 0;
}

/* a write's email gate (the socket's dispatch): a refused write answers the socket's error */
static void step_gate(Op *op) {
    const char *label = fa_str(op->cur, 0);
    if (!social_email_gate(op->s, label ? label : "", op->cur)) return;
    op->handler(op->s, op->cur);
}

/* runs a socket handler on the frame (kept until the request ends). A write first loads the email state
 * (sh_email_state), then passes the gate (step_gate). The handler's frames arrive through op_emit. */
static void op_call(Op *op, SocialHandler fn, cJSON *frame) {
    keep(op, frame);
    op->cur = frame;
    op->handler = fn;
    if (op->write) {
        op->next = step_gate;
        sh_email_state(op->s, NULL);
        return;
    }
    fn(op->s, frame);
}

/* ---- the caller (ranked.c's get_token and with_account) */

typedef struct Pending {
    kn_http_req *req;
    Args a;
    cJSON *body;
    OpStep first;
    OpReply reply;
    int write, required;
} Pending;

static void pending_open(Pending *p, int64_t id, const char *name) {
    if (p->required && id <= 0) {
        v10_reply_error(p->req, 401, "LOGIN_REQUIRED", "Must be logged in");
        cJSON_Delete(p->body);
        free(p);
        return;
    }
    Op *op = calloc(1, sizeof *op);
    if (!op) {
        v10_reply_error(p->req, 500, "INTERNAL", "Out of memory");
        cJSON_Delete(p->body);
        free(p);
        return;
    }
    op->cap.emit = op_emit;
    op->cap.idle = op_idle;
    op->cap.ud = op;
    op->s = social_capture_new(&op->cap, id, name);
    op->frames = cJSON_CreateArray();
    op->sent = cJSON_CreateArray();
    op->req = p->req;
    op->body = p->body;
    op->a = p->a;
    op->reply = p->reply;
    op->write = p->write;
    op->required = p->required;
    op->account = id;
    OpStep first = p->first;
    free(p);
    if (!op->s) {
        v10_reply_error(op->req, 500, "INTERNAL", "Out of memory");
        op_free(op);
        return;
    }
    op->next = first;
    op_pump(op);
}

static void on_caller(void *ud, const char *err, const cJSON *acc) {
    Pending *p = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    pending_open(p, id, acc && !err ? kn_json_str(acc, "username", "") : "");
}

/* the Bearer token, or ?token= (ranked.c's get_token: the query first) */
static void token_of(kn_http_req *req, char *token, size_t cap) {
    token[0] = 0;
    if (kn_http_query_get(req, "token", token, cap) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(token, cap, "%s", bearer ? bearer : "");
    }
}

/* starts a request: the caller is resolved first (required: 401 without one), then the first step runs */
static void route_begin(kn_http_req *req, int required, int write, OpStep first, OpReply reply, const Args *a,
                        cJSON *body) {
    Pending *p = calloc(1, sizeof *p);
    if (!p) {
        v10_reply_error(req, 500, "INTERNAL", "Out of memory");
        cJSON_Delete(body);
        return;
    }
    p->req = req;
    p->a = *a;
    p->body = body ? body : cJSON_CreateObject();
    p->first = first;
    p->reply = reply;
    p->write = write;
    p->required = required;
    char token[512];
    token_of(req, token, sizeof token);
    if (!token[0]) {
        pending_open(p, 0, "");
        return;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(s_brain, "token", args, on_caller, p);
    cJSON_Delete(args);
}

/* ---- row mappings: the socket's rows as v10's REST shapes */

/* a listing (LISTING_ROWS: m, i, f, p, ms, paint_*; listings_of adds sk and kr): {marketid, skinindex, funds, ...} */
static cJSON *listing_row(const cJSON *r) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "marketid", (double) kn_json_int(r, "m", 0));
    cJSON_AddNumberToObject(o, "skinindex", (double) kn_json_int(r, "i", kn_json_int(r, "sk", -1)));
    cJSON_AddNumberToObject(o, "funds", (double) kn_json_int(r, "f", kn_json_int(r, "kr", 0)));
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(r, "p");
    if (cJSON_IsString(p)) cJSON_AddStringToObject(o, "seller_name", p->valuestring);
    cJSON_AddNumberToObject(o, "created_at", (double) kn_json_int(r, "ms", 0));
    /* the cards read listed_time (deob-10.0.3.js:123698-123701, 151762-151765: new Date(listed_time)) and
     * seller_id (151768: no Buy on the caller's own listing, 146473) */
    cJSON_AddNumberToObject(o, "listed_time", (double) kn_json_int(r, "ms", 0));
    if (cJSON_GetObjectItemCaseSensitive(r, "seller_id")) cJSON_AddNumberToObject(o, "seller_id", (double) kn_json_int(r, "seller_id", 0));
    /* a painted copy's listing carries its paint as m (the official card reads the listing's m as its metadata) */
    cJSON *m = v10_paint_meta(kn_json_str(r, "paint_color", ""), (int) kn_json_int(r, "paint_shader", -1));
    if (m) cJSON_AddItemToObject(o, "m", m);
    return o;
}

/* an inventory row: {i, cnt} (cnt the owned count: the unpainted units), with the cards' nt / qs / nqs when set */
static cJSON *inventory_row(const cJSON *r) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "i", (double) kn_json_int(r, "i", -1));
    cJSON_AddNumberToObject(o, "cnt", (double) kn_json_int(r, "c", 0));
    cJSON_AddNumberToObject(o, "st", 1); /* a stack (deob-10.0.3.js:124726: st === 1 is a stack, else a copy) */
    put_if(o, r, "nt");
    put_if(o, r, "qs");
    put_if(o, r, "nqs");
    return o;
}

/* a painted copy's row (skin_copies): {i, sid: its copy id, m: {paint} | {paintShader}}, no cnt (one unit) */
static cJSON *copy_inventory_row(const cJSON *r) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "i", (double) kn_json_int(r, "i", -1));
    cJSON_AddNumberToObject(o, "sid", (double) kn_json_int(r, "sid", 0));
    cJSON_AddNumberToObject(o, "st", 0);
    /* the row's m (the market page, shaped by economy.c shape_inventory), else its paint columns (the trade rows) */
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "m");
    cJSON *paint = m ? cJSON_Duplicate(m, 1)
                     : v10_paint_meta(kn_json_str(r, "paint_color", ""), (int) kn_json_int(r, "paint_shader", -1));
    if (paint) cJSON_AddItemToObject(o, "m", paint);
    put_if(o, r, "nt");
    put_if(o, r, "qs");
    put_if(o, r, "nqs");
    return o;
}

/* a market history row: the other party's name is other_party_name */
static cJSON *history_row(const cJSON *r) {
    cJSON *o = cJSON_CreateObject();
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(r, "type");
    cJSON_AddStringToObject(o, "type", cJSON_IsString(type) ? type->valuestring : "");
    cJSON_AddNumberToObject(o, "date", (double) kn_json_int(r, "date", 0));
    cJSON_AddNumberToObject(o, "skinindex", (double) kn_json_int(r, "skinindex", -1));
    cJSON_AddNumberToObject(o, "amount", (double) kn_json_int(r, "amount", 0));
    cJSON_AddItemToObject(o, "other_party_name", copy_or_null(r, "who"));
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
    cJSON_AddStringToObject(o, "status", cJSON_IsString(status) ? status->valuestring : "");
    return o;
}

/* "1,1,5," (the socket's skin ids, one per unit) -> [{ind: 1}, {ind: 1}, {ind: 5}] (the trade card's items) */
static cJSON *units_of(const char *ids) {
    cJSON *a = cJSON_CreateArray();
    if (!ids || !*ids) return a;
    char *copy = strdup(ids);
    if (!copy) return a;
    for (char *t = strtok(copy, ","); t; t = strtok(NULL, ",")) {
        if (!*t) continue;
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "ind", (double) atoll(t));
        cJSON_AddItemToArray(a, u);
    }
    free(copy);
    return a;
}

/* a side's painted copies (economy's copies_s / copies_r: {sid, i, paint_color, paint_shader}) as units appended to the
 * side's units: {ind, sid: the copy id, m: its paint} (doc/v10/paint-copy.md) */
static void add_copy_units(cJSON *units, const cJSON *copies) {
    const cJSON *r;
    cJSON_ArrayForEach(r, copies) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "ind", (double) kn_json_int(r, "i", -1));
        cJSON_AddNumberToObject(u, "sid", (double) kn_json_int(r, "sid", 0));
        cJSON *m = v10_paint_meta(kn_json_str(r, "paint_color", ""), (int) kn_json_int(r, "paint_shader", -1));
        if (m) cJSON_AddItemToObject(u, "m", m);
        cJSON_AddItemToArray(units, u);
    }
}

/* a side's KR as the trade card's currency list: [{cid: 1, amt}] (cid 1 = KR); [] for none */
static cJSON *currency_of(int64_t kr) {
    cJSON *a = cJSON_CreateArray();
    if (kr <= 0) return a;
    cJSON *c = cJSON_CreateObject();
    cJSON_AddNumberToObject(c, "cid", 1);
    cJSON_AddNumberToObject(c, "amt", (double) kr);
    cJSON_AddItemToArray(a, c);
    return a;
}

/* a trade (economy's trade_rows): sksell / krsell are the sender's (pl), skbuy / krbuy the receiver's */
static cJSON *trade_row(const cJSON *t) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "trid", (double) kn_json_int(t, "trid", 0));
    cJSON_AddNumberToObject(o, "pl", (double) kn_json_int(t, "pl", 0));
    cJSON_AddItemToObject(o, "player", copy_or_null(t, "player"));
    cJSON_AddItemToObject(o, "buyer", copy_or_null(t, "buyer"));
    cJSON_AddItemToObject(o, "message", copy_or_null(t, "message"));
    cJSON_AddItemToObject(o, "status", copy_or_null(t, "status"));
    cJSON_AddNumberToObject(o, "dte", (double) kn_json_int(t, "dte", 0));
    put_if(o, t, "exp");
    /* a side's units: its stacks (one per unit), then its painted copies with their paint (m) */
    cJSON *sell = units_of(kn_json_str(t, "sksell", NULL)), *buy = units_of(kn_json_str(t, "skbuy", NULL));
    add_copy_units(sell, cJSON_GetObjectItemCaseSensitive(t, "copies_s"));
    add_copy_units(buy, cJSON_GetObjectItemCaseSensitive(t, "copies_r"));
    cJSON_AddItemToObject(o, "sksell", sell);
    cJSON_AddItemToObject(o, "skbuy", buy);
    cJSON_AddItemToObject(o, "csell", currency_of(kn_json_int(t, "krsell", 0)));
    cJSON_AddItemToObject(o, "cbuy", currency_of(kn_json_int(t, "krbuy", 0)));
    return o;
}

/* ---- market reads */

/* the listing passes the request's itemIds filter */
static int row_wanted(const Args *a, const cJSON *r) {
    if (!a->nitems) return 1;
    const int64_t skin = kn_json_int(r, "i", -1);
    for (int i = 0; i < a->nitems; i++)
        if (a->items[i] == skin) return 1;
    return 0;
}

/* appends up to limit listings after the cursor's listing (cursor 0, or one not in the list: from the first).
 * *more: rows remain after the page; *last: the page's last listing id. */
static void page_rows(cJSON *out, const cJSON *rows, const Args *a, int *more, int64_t *last) {
    int skipping = a->cursor > 0;
    cJSON *r;
    *more = 0;
    *last = 0;
    if (skipping) {
        int found = 0;
        cJSON_ArrayForEach(r, rows) {
            if (row_wanted(a, r) && kn_json_int(r, "m", 0) == a->cursor) {
                found = 1;
                break;
            }
        }
        if (!found) skipping = 0;
    }
    int count = 0;
    cJSON_ArrayForEach(r, rows) {
        if (!row_wanted(a, r)) continue;
        if (skipping) {
            if (kn_json_int(r, "m", 0) == a->cursor) skipping = 0;
            continue;
        }
        if (count >= a->limit) {
            *more = 1;
            break;
        }
        cJSON_AddItemToArray(out, listing_row(r));
        *last = kn_json_int(r, "m", 0);
        count++;
    }
}

static void step_listings(Op *op) {
    cJSON *f = social_frame("fil");
    add_int(f, 0);
    add_str(f, op->a.text);
    cJSON_AddItemToArray(f, bits_object(op->a.rar));
    cJSON_AddItemToArray(f, bits_object(op->a.wep));
    cJSON_AddItemToArray(f, bits_object(op->a.typ));
    add_int(f, op->a.season);
    add_str(f, op->a.desc ? "0" : "1");
    op_call(op, sh_market_filter, f);
}

/* ["fr", token, rows | null] */
static void reply_listings(Op *op) {
    if (op_failed(op)) return;
    const cJSON *rows = op_arg(op, "fr", 2);
    cJSON *list = cJSON_CreateArray();
    int more = 0;
    int64_t last = 0;
    if (cJSON_IsArray(rows)) page_rows(list, rows, &op->a, &more, &last);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "listings", list);
    cJSON_AddBoolToObject(o, "has_more", more);
    if (more && last > 0) cJSON_AddNumberToObject(o, "next_cursor_id", (double) last);
    else cJSON_AddNullToObject(o, "next_cursor_id");
    v10_reply_data(op->req, 200, o);
}

static void h_market_listings(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    a.limit = query_int(req, "limit", 50);
    if (a.limit < 1) a.limit = 1;
    if (a.limit > 100) a.limit = 100;
    a.cursor = query_int(req, "cursor_id", 0);
    char sort[24];
    query_text(req, "sort", sort, sizeof sort);
    a.desc = !strcmp(sort, "price_desc");
    query_text(req, "searchTerm", a.text, sizeof a.text);
    a.rar = csv_bits(req, "rarities");
    a.wep = csv_bits(req, "weapons");
    a.typ = csv_bits(req, "types");
    a.season = query_int(req, "season", 0);
    a.nitems = csv_ids(req, "items", a.items, MAX_ITEM_IDS);
    /* item_id: one item's listings (the item sidebar, deob-10.0.3.js:30822-30823, 148938, 148954) */
    const int64_t one = query_int(req, "item_id", -1);
    if (one >= 0 && a.nitems < MAX_ITEM_IDS) a.items[a.nitems++] = one;
    route_begin(req, 0, 0, step_listings, reply_listings, &a, NULL);
}

static void step_stats(Op *op) {
    cJSON *f = social_frame("stats");
    add_int(f, 0);
    add_str(f, op->a.text);
    cJSON_AddItemToArray(f, bits_object(op->a.rar));
    cJSON_AddItemToArray(f, bits_object(op->a.wep));
    cJSON_AddItemToArray(f, bits_object(op->a.typ));
    add_int(f, op->a.season);
    op_call(op, sh_market_stats_http, f);
}

/* ["0", "market", "stats", rows | null] */
/* the stats grid's order (v10.0.3's item grid comparator, deob-10.0.3.js:146885-146903): name A-Z, rarity high
 * first, season newest first (seas || 1), type ascending, new / old by store index; ties (and an unknown sort) keep
 * the store index order */
static const char *g_stats_sort;

static int stats_cmp(const void *pa, const void *pb) {
    const cJSON *a = *(const cJSON *const *) pa, *b = *(const cJSON *const *) pb;
    const int64_t ia = kn_json_int(a, "ski", -1), ib = kn_json_int(b, "ski", -1);
    const SkinInfo *ka = acct_skin(ia), *kb = acct_skin(ib);
    int r = 0;
    if (ka && kb) {
        if (!strcmp(g_stats_sort, "name")) r = strcasecmp(ka->name, kb->name);
        else if (!strcmp(g_stats_sort, "rarity")) r = kb->rarity - ka->rarity;
        else if (!strcmp(g_stats_sort, "season")) r = (kb->seas ? kb->seas : 1) - (ka->seas ? ka->seas : 1);
        else if (!strcmp(g_stats_sort, "type")) r = ka->type - kb->type;
        else if (!strcmp(g_stats_sort, "new")) r = ib > ia ? 1 : ib < ia ? -1 : 0;
    }
    if (r) return r;
    return ia < ib ? -1 : ia > ib ? 1 : 0;
}

/* ["0", "market", "stats", rows | null]: every filtered row, sorted, then the page after the cursor (an offset).
 * v10.0.3 sends sort, cursor (the previous page's next_cursor) and the filters, and reads stats, has_more and
 * next_cursor (deob-10.0.3.js:30873-30900, 150895-150918, load more 151236-151262). */
static void reply_stats(Op *op) {
    if (op_failed(op)) return;
    const cJSON *rows = op_arg(op, "0", 3);
    const int n = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
    const cJSON **sorted = n > 0 ? malloc((size_t) n * sizeof *sorted) : NULL;
    for (int i = 0; sorted && i < n; i++) sorted[i] = cJSON_GetArrayItem(rows, i);
    g_stats_sort = op->a.sort[0] ? op->a.sort : "rarity";
    if (sorted) qsort(sorted, (size_t) n, sizeof *sorted, stats_cmp);
    const int start = op->a.cursor > 0 && op->a.cursor < n ? (int) op->a.cursor : op->a.cursor >= n ? n : 0;
    const int end = start + (int) op->a.limit < n ? start + (int) op->a.limit : n;
    cJSON *page = cJSON_CreateArray();
    for (int i = start; sorted && i < end; i++) cJSON_AddItemToArray(page, cJSON_Duplicate(sorted[i], 1));
    free(sorted);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "stats", page);
    cJSON_AddBoolToObject(o, "has_more", end < n);
    if (end < n) cJSON_AddNumberToObject(o, "next_cursor", end);
    else cJSON_AddNullToObject(o, "next_cursor");
    v10_reply_data(op->req, 200, o);
}

static void h_market_stats(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    query_text(req, "searchTerm", a.text, sizeof a.text);
    a.rar = csv_bits(req, "rarities");
    a.wep = csv_bits(req, "weapons");
    a.typ = csv_bits(req, "types");
    a.season = query_int(req, "season", 0);
    query_text(req, "sort", a.sort, sizeof a.sort);
    a.cursor = query_int(req, "cursor", 0);
    a.limit = query_int(req, "limit", 50);
    if (a.limit < 1) a.limit = 1;
    if (a.limit > 100) a.limit = 100;
    route_begin(req, 0, 0, step_stats, reply_stats, &a, NULL);
}

/* ---- the caller's own market pages (market_page's subs: a guest has none, so its answer is null) */

/* a "r" frame of the market page: market_page answers ["0", "market", sub, rows | null] */
static void step_page(Op *op, const char *sub) {
    cJSON *f = keep(op, social_frame("r"));
    add_str(f, "market");
    add_str(f, sub);
    op->next = NULL;
    market_page(op->s, "market", f);
}

static void step_inventory(Op *op) { step_page(op, "inventory"); }
static void step_sales(Op *op) { step_page(op, "sales"); }
static void step_history(Op *op) { step_page(op, "history"); }
static void step_trades_page(Op *op) { step_page(op, "trades"); }

/* a painted copy's row carries its m (the market page's rows, shaped by economy.c shape_inventory) or its paint
 * columns (the raw rows); a stack's has neither */
static int is_copy_row(const cJSON *r) {
    return cJSON_GetObjectItemCaseSensitive(r, "m") != NULL || kn_json_str(r, "paint_color", "")[0] != '\0'
           || kn_json_int(r, "paint_shader", -1) >= 0;
}

/* the stacks (their unpainted units; sid = the store index, the Paint action's skinId) and the painted copies
 * (sid = the copy id), in the market page's order */
static void reply_inventory(Op *op) {
    if (op_failed(op)) return;
    const cJSON *rows = op_arg(op, "0", 3);
    cJSON *o = cJSON_CreateArray();
    cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        if (is_copy_row(r)) {
            cJSON_AddItemToArray(o, copy_inventory_row(r));
            continue;
        }
        cJSON *row = inventory_row(r);
        cJSON_AddNumberToObject(row, "sid", (double) kn_json_int(r, "i", -1));
        cJSON_AddItemToArray(o, row);
    }
    v10_reply_data(op->req, 200, o);
}

static void reply_sales(Op *op) {
    if (op_failed(op)) return;
    const cJSON *rows = op_arg(op, "0", 3);
    cJSON *o = cJSON_CreateArray();
    cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddItemToArray(o, listing_row(r));
    v10_reply_data(op->req, 200, o);
}

/* the history page: the socket's rows (newest first), paged by offset (cursor) */
static void reply_history(Op *op) {
    if (op_failed(op)) return;
    const cJSON *rows = op_arg(op, "0", 3);
    const int n = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
    const int start = op->a.offset < n ? (int) op->a.offset : n;
    const int count = n - start < op->a.limit ? n - start : (int) op->a.limit;
    cJSON *page = cJSON_CreateArray();
    for (int i = start; i < start + count; i++) cJSON_AddItemToArray(page, history_row(cJSON_GetArrayItem(rows, i)));
    const int more = start + count < n;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "history", page);
    cJSON_AddBoolToObject(o, "has_more", more);
    if (more) cJSON_AddNumberToObject(o, "next_cursor", (double) (start + count));
    else cJSON_AddNullToObject(o, "next_cursor");
    v10_reply_data(op->req, 200, o);
}

static void h_market_inventory(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 0, 0, step_inventory, reply_inventory, &a, NULL);
}

static void h_market_sales(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 0, 0, step_sales, reply_sales, &a, NULL);
}

static void h_market_history(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    a.limit = query_int(req, "limit", 50);
    if (a.limit < 1) a.limit = 1;
    if (a.limit > 100) a.limit = 100;
    a.offset = query_int(req, "cursor", 0);
    route_begin(req, 0, 0, step_history, reply_history, &a, NULL);
}

/* ---- the black market: the caller's purchases, the caller's rank (skin_transactions, ranked_profiles) */

static const kn_sql q_bm_owned = {
    .any = "SELECT skin_id FROM skin_transactions WHERE account_id = $1 AND source = 'blackmarket' ORDER BY skin_id"};
static const kn_sql q_peak = {.any = "SELECT max(peak_mmr) FROM ranked_profiles WHERE account_id = $1"};
static const kn_sql q_account = {.any = "SELECT id FROM accounts WHERE lower(username) = lower($1)"};

static void account_text(char buf[24], int64_t id) { snprintf(buf, 24, "%lld", (long long) id); }

static void step_bm_owned(Op *op) {
    if (op->account <= 0) return; /* a guest owns none */
    char acc[24];
    account_text(acc, op->account);
    const char *p[1] = {acc};
    if (op_query(op, &q_bm_owned, 1, p) != 0) op_fail(op, 500, "INTERNAL", "Server error");
}

/* [skin index, ...] of the caller's black-market purchases */
static void reply_bm_owned(Op *op) {
    if (op_failed(op)) return;
    cJSON *o = cJSON_CreateArray();
    if (op->rows) {
        cJSON *row;
        cJSON_ArrayForEach(row, op->rows) cJSON_AddItemToArray(o, cJSON_CreateNumber((double) row_int(row)));
    }
    v10_reply_data(op->req, 200, o);
}

static void h_bm_owned(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 0, 0, step_bm_owned, reply_bm_owned, &a, NULL);
}

static void step_bm_rank(Op *op) {
    if (op->account <= 0) return; /* a guest has no rank */
    char acc[24];
    account_text(acc, op->account);
    const char *p[1] = {acc};
    if (op_query(op, &q_peak, 1, p) != 0) op_fail(op, 500, "INTERNAL", "Server error");
}

/* {peak_mmr}, or null: the caller's peak over its seasons (the rank the black market's "rank" check reads) */
static void reply_bm_rank(Op *op) {
    if (op_failed(op)) return;
    const cJSON *peak = op->rows ? cJSON_GetArrayItem(op->rows, 0) : NULL;
    const cJSON *value = peak ? peak->child : NULL;
    if (!cJSON_IsNumber(value)) {
        v10_reply_data(op->req, 200, NULL);
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "peak_mmr", value->valuedouble);
    v10_reply_data(op->req, 200, o);
}

static void h_bm_rank(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 0, 0, step_bm_rank, reply_bm_rank, &a, NULL);
}

/* ---- trades: the caller's open and completed trades ([open, completed] from the socket) */

static void reply_trades_history(Op *op) {
    if (op_failed(op)) return;
    const cJSON *d = op_arg(op, "0", 3);
    const cJSON *opened = cJSON_IsArray(d) ? cJSON_GetArrayItem(d, 0) : NULL;
    const cJSON *done = cJSON_IsArray(d) ? cJSON_GetArrayItem(d, 1) : NULL;
    cJSON *opens = cJSON_CreateArray();
    cJSON *r;
    if (op->a.include_open && op->a.offset == 0 && cJSON_IsArray(opened)) {
        cJSON_ArrayForEach(r, opened) cJSON_AddItemToArray(opens, trade_row(r));
    }
    const int n = cJSON_IsArray(done) ? cJSON_GetArraySize(done) : 0;
    const int start = op->a.offset < n ? (int) op->a.offset : n;
    const int count = n - start < op->a.limit ? n - start : (int) op->a.limit;
    cJSON *completed = cJSON_CreateArray();
    for (int i = start; i < start + count; i++) cJSON_AddItemToArray(completed, trade_row(cJSON_GetArrayItem(done, i)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "open", opens);
    cJSON_AddItemToObject(o, "completed", completed);
    cJSON_AddBoolToObject(o, "completed_has_more", start + count < n);
    v10_reply_data(op->req, 200, o);
}

static void h_trades_history(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    a.limit = query_int(req, "limit", 30);
    if (a.limit < 1) a.limit = 1;
    if (a.limit > 100) a.limit = 100;
    a.offset = query_int(req, "offset", 0);
    char inc[16];
    /* v10.0.3 sends include_open=0|1 (deob-10.0.3.js:31078); includeOpen is kept for older callers */
    query_text(req, "include_open", inc, sizeof inc);
    if (!inc[0]) query_text(req, "includeOpen", inc, sizeof inc);
    a.include_open = strcmp(inc, "false") && strcmp(inc, "0");
    a.ectx = ERR_TRADE;
    route_begin(req, 0, 0, step_trades_page, reply_trades_history, &a, NULL);
}

/* ---- trades: the two inventories (the caller's, and the other player's by id or by name) */

static void step_trade_inv_call(Op *op) {
    cJSON *f = social_frame("ri");
    add_str(f, op->a.name);
    add_int(f, op->a.id);
    add_null(f);
    op_call(op, sh_trade_inv, f);
}

/* the other player's id by name (accounts, case-insensitive): the lookup's row gives a.id */
static void step_trade_inv_found(Op *op) {
    if (!op->dbok || cJSON_GetArraySize(op->rows) < 1) {
        op_fail(op, 404, "NOT_FOUND", "Player not found");
        return;
    }
    op->a.id = row_int(cJSON_GetArrayItem(op->rows, 0));
    op->next = step_trade_inv_call;
}

static void step_trade_inv_lookup(Op *op) {
    if (op->a.id > 0) { /* the route gave an id */
        op->next = step_trade_inv_call;
        return;
    }
    op->next = step_trade_inv_found;
    const char *p[1] = {op->a.name};
    if (op_query(op, &q_account, 1, p) != 0) op_fail(op, 500, "INTERNAL", "Server error");
}

/* a trade picker's side: the stacks as they are ({i, c}) with st 1; a painted copy as the market has it ({i, sid, m,
 * st 0}, one unit: c 1) (doc/v10/paint-copy.md). The picker treats a row as a stack only when st === 1
 * (deob-10.0.3.js:125012, 124726); any other row is one copy offered as {i, c: 1, iids: [sid]} (125069-125075). */
static cJSON *trade_inv_side(const cJSON *side) {
    cJSON *a = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, side) {
        if (kn_json_str(r, "paint_color", "")[0] == '\0' && kn_json_int(r, "paint_shader", -1) < 0) {
            cJSON *u = cJSON_Duplicate(r, 1);
            cJSON_DeleteItemFromObjectCaseSensitive(u, "st");
            cJSON_AddNumberToObject(u, "st", 1);
            cJSON_AddItemToArray(a, u);
            continue;
        }
        cJSON *u = copy_inventory_row(r);
        cJSON_AddNumberToObject(u, "c", 1);
        cJSON_AddItemToArray(a, u);
    }
    return a;
}

/* ["trD", name, id, [mine, theirs]] */
static void reply_trade_inv(Op *op) {
    if (op_failed(op)) return;
    const cJSON *sides = op_arg(op, "trD", 3);
    cJSON *o = cJSON_CreateObject();
    const cJSON *mine = cJSON_IsArray(sides) ? cJSON_GetArrayItem(sides, 0) : NULL;
    const cJSON *theirs = cJSON_IsArray(sides) ? cJSON_GetArrayItem(sides, 1) : NULL;
    cJSON_AddItemToObject(o, "my_inventory", trade_inv_side(mine));
    cJSON_AddItemToObject(o, "their_inventory", trade_inv_side(theirs));
    v10_reply_data(op->req, 200, o);
}

static void h_trade_inventory(kn_http_req *req, void *ud) {
    (void) ud;
    const char *p = kn_http_param(req, "name");
    Args a;
    memset(&a, 0, sizeof a);
    snprintf(a.name, sizeof a.name, "%s", p ? p : "");
    char *end = NULL;
    const long long id = strtoll(a.name, &end, 10);
    if (a.name[0] && end && !*end && id > 0) a.id = (int64_t) id;
    a.ectx = ERR_TRADE;
    route_begin(req, 1, 0, step_trade_inv_lookup, reply_trade_inv, &a, NULL);
}

/* ---- the trade player search: the social search's exact name (social_gapi.c /players/search) */

static int name_ok(const char *s) {
    const size_t n = strlen(s);
    if (n < 2 || n > 32) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_'))
            return 0;
    return 1;
}

static int same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20)) return 0;
    return *a == *b;
}

/* ["gapi", rid, status, {data: {results: [...]}} | {error}] */
static void step_player_search(Op *op) {
    if (!name_ok(op->a.name)) {
        op_fail(op, 404, "NOT_FOUND", "Player not found");
        return;
    }
    char path[160];
    snprintf(path, sizeof path, "/players/search?q=%s&limit=10", op->a.name);
    cJSON *f = social_frame("gapi");
    add_int(f, 0);
    add_str(f, "GET");
    add_str(f, path);
    add_null(f);
    op_call(op, sh_gapi, f);
}

static void reply_player_search(Op *op) {
    if (op_failed(op)) return;
    const cJSON *status = op_arg(op, "gapi", 2);
    const cJSON *payload = op_arg(op, "gapi", 3);
    const cJSON *data = payload ? cJSON_GetObjectItemCaseSensitive(payload, "data") : NULL;
    const cJSON *results = data ? cJSON_GetObjectItemCaseSensitive(data, "results") : NULL;
    const cJSON *hit = NULL, *r;
    if (cJSON_IsNumber(status) && status->valueint == 200 && cJSON_IsArray(results)) {
        cJSON_ArrayForEach(r, results) {
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(r, "player_name");
            if (cJSON_IsString(n) && same_name(n->valuestring, op->a.name)) {
                hit = r;
                break;
            }
        }
    }
    if (!hit) {
        err_reply(op->req, 404, ERR_TRADE, "NOT_FOUND", "Player not found"); /* TRADE_PLAYER_NOT_FOUND */
        return;
    }
    v10_reply_data(op->req, 200, cJSON_Duplicate(hit, 1));
}

static void h_player_search(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    query_text(req, "name", a.name, sizeof a.name);
    a.ectx = ERR_TRADE;
    route_begin(req, 0, 0, step_player_search, reply_player_search, &a, NULL);
}

/* ---- players: the sales chart and the listings (public) */

/* ["psd", [{d, f}]] for the range index's days (v10's day ranges; 30 days when the index is none) */
static void step_player_sales(Op *op) {
    const int64_t r = op->a.num;
    const int64_t days = r >= 0 && r < DAY_RANGES ? k_day_ranges[r] : 30;
    cJSON *f = social_frame("pst");
    add_str(f, op->a.name);
    add_int(f, days);
    op_call(op, sh_profile_stats, f);
}

static void reply_player_sales(Op *op) {
    if (op_failed(op)) return;
    const cJSON *rows = op_arg(op, "psd", 1);
    v10_reply_data(op->req, 200, cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : cJSON_CreateArray());
}

static void h_player_sales(kn_http_req *req, void *ud) {
    (void) ud;
    const char *p = kn_http_param(req, "name");
    Args a;
    memset(&a, 0, sizeof a);
    snprintf(a.name, sizeof a.name, "%s", p ? p : "");
    a.num = query_int(req, "range", 0);
    route_begin(req, 0, 0, step_player_sales, reply_player_sales, &a, NULL);
}

/* the player's listings: the name's id (accounts), then the socket's listings of that id */
static void step_player_listings(Op *op) {
    if (!op->dbok || cJSON_GetArraySize(op->rows) < 1) return; /* no such player: an empty list */
    cJSON *f = social_frame("uml");
    add_int(f, row_int(cJSON_GetArrayItem(op->rows, 0)));
    op_call(op, sh_listings_of, f);
}

static void step_player_listings_lookup(Op *op) {
    op->next = step_player_listings;
    const char *p[1] = {op->a.name};
    if (op_query(op, &q_account, 1, p) != 0) op_fail(op, 500, "INTERNAL", "Server error");
}

/* ["ul", [listing rows]] */
static void reply_player_listings(Op *op) {
    if (op_failed(op)) return;
    const cJSON *rows = op_arg(op, "ul", 1);
    cJSON *o = cJSON_CreateArray();
    cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddItemToArray(o, listing_row(r));
    v10_reply_data(op->req, 200, o);
}

static void h_player_listings(kn_http_req *req, void *ud) {
    (void) ud;
    const char *p = kn_http_param(req, "name");
    Args a;
    memset(&a, 0, sizeof a);
    snprintf(a.name, sizeof a.name, "%s", p ? p : "");
    route_begin(req, 0, 0, step_player_listings_lookup, reply_player_listings, &a, NULL);
}

/* ---- items: the sales graph, the instances, paint and style */

/* ["gd", [{d, f, t}]] for the item (sh_item_stats: a range index 0..6, else a number of days) */
static void step_item_graph(Op *op) {
    cJSON *f = social_frame("st");
    add_int(f, op->a.id);
    add_int(f, op->a.num);
    op_call(op, sh_item_stats, f);
}

static void reply_item_graph(Op *op) {
    if (op_failed(op)) return;
    const cJSON *rows = op_arg(op, "gd", 1);
    v10_reply_data(op->req, 200, cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : cJSON_CreateArray());
}

static void h_item_graph(kn_http_req *req, void *ud) {
    (void) ud;
    if (!item_known(req)) return;
    Args a;
    memset(&a, 0, sizeof a);
    a.id = (int64_t) strtoll(kn_http_param(req, "id"), NULL, 10);
    a.num = query_int(req, "range", 0);
    route_begin(req, 0, 0, step_item_graph, reply_item_graph, &a, NULL);
}

/* market/items/:id/sales-statistics?daysback= (the same graph; the days, not a range index) */
static void h_market_item_stats(kn_http_req *req, void *ud) {
    (void) ud;
    if (!item_known(req)) return;
    Args a;
    memset(&a, 0, sizeof a);
    a.id = (int64_t) strtoll(kn_http_param(req, "id"), NULL, 10);
    a.num = query_int(req, "daysback", 3);
    route_begin(req, 0, 0, step_item_graph, reply_item_graph, &a, NULL);
}

static void h_item_instances(kn_http_req *req, void *ud) {
    (void) ud;
    if (!item_known(req)) return;
    v10_reply_data(req, 200, cJSON_CreateArray()); /* KrunkNative keeps no per-copy instances: none */
}

/* ---- POST /items/:id/paint (v10.0.3's items.setPaint, body {item_id, paint_item_id}): the {id} is the owned copy's
 * id (the sid of the market rows) or the stack's store index, whose unpainted unit becomes a new painted copy.
 * The work is sh_item_paint (economy.c) and run_item_paint (db_economy.c); doc/v10/paint-copy.md. */
static void step_item_paint(Op *op) {
    cJSON *f = social_frame("ip");
    const int64_t paint = kn_json_int(op->body, "paint_item_id", -1);
    const char *color = NULL;
    int shader = -1;
    v10_paint_of(paint, &color, &shader);
    add_int(f, op->a.id);
    add_int(f, kn_json_int(op->body, "item_id", -1));
    add_int(f, paint);
    add_str(f, color ? color : "");
    add_int(f, shader);
    op_call(op, sh_item_paint, f);
}

static void reply_item_paint(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "ipr", 1);
    v10_reply_data(op->req, 200, cJSON_IsObject(v) ? cJSON_Duplicate(v, 1) : cJSON_CreateObject());
}

static void h_item_paint(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    a.id = (int64_t) strtoll(kn_http_param(req, "id"), NULL, 10);
    cJSON *body = body_of(req);
    const int64_t item = kn_json_int(body, "item_id", -1);
    const int64_t paint = kn_json_int(body, "paint_item_id", -1);
    const char *color = NULL;
    int shader = -1;
    if (!acct_skin(item)) {
        cJSON_Delete(body);
        v10_reply_error(req, 404, "ITEMS_INVALID_ID", "Item not found"); /* as item_known's */
        return;
    }
    if (!v10_paint_of(paint, &color, &shader)) {
        cJSON_Delete(body);
        v10_reply_error(req, 400, "ITEM_PAINT_INVALID", "That item is not a paint.");
        return;
    }
    if (a.id != item && a.id < V10_COPY_ID_BASE) {
        cJSON_Delete(body);
        v10_reply_error(req, 400, "ITEM_PAINT_INVALID", "Paint the copy of that item, or an unpainted one of it.");
        return;
    }
    a.ectx = ERR_OBJECT;
    route_begin(req, 1, 1, step_item_paint, reply_item_paint, &a, body);
}

static void h_item_style(kn_http_req *req, void *ud) {
    (void) ud;
    if (!item_known(req)) return;
    v10_reply_error(req, 403, "NOT_IMPLEMENTED", WHY_ITEM_STYLE);
}

/* the item's market page: {ind, onS, inC, high, low} with the owners ["0", "itemsales", sub, info, owners] */
static void step_item_info(Op *op) {
    cJSON *f = keep(op, social_frame("r"));
    add_str(f, "itemsales");
    add_null(f);
    add_null(f);
    add_null(f);
    add_null(f);
    add_null(f);
    add_int(f, op->a.id); /* the item index sits at position 7 */
    op->next = NULL;
    market_page(op->s, "itemsales", f);
}

static void reply_item_info(Op *op) {
    if (op_failed(op)) return;
    const cJSON *info = op_arg(op, "0", 3);
    const cJSON *owners = op_arg(op, "0", 4);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ind", (double) op->a.id);
    cJSON_AddNumberToObject(o, "onS", 0);
    cJSON_AddNumberToObject(o, "inC", 0);
    cJSON_AddNumberToObject(o, "high", 0);
    cJSON_AddNumberToObject(o, "low", 0);
    if (cJSON_IsObject(info)) {
        cJSON *k;
        cJSON_ArrayForEach(k, info) {
            cJSON_DeleteItemFromObjectCaseSensitive(o, k->string);
            cJSON_AddItemToObject(o, k->string, cJSON_Duplicate(k, 1));
        }
    }
    cJSON_AddItemToObject(o, "owners", cJSON_IsArray(owners) ? cJSON_Duplicate(owners, 1) : cJSON_CreateArray());
    v10_reply_data(op->req, 200, o);
}

static void h_market_item_sales(kn_http_req *req, void *ud) {
    (void) ud;
    if (!item_known(req)) return;
    Args a;
    memset(&a, 0, sizeof a);
    a.id = (int64_t) strtoll(kn_http_param(req, "id"), NULL, 10);
    route_begin(req, 0, 0, step_item_info, reply_item_info, &a, NULL);
}

/* the item view: its market info (step 1), the 30-day graph (step 2), the open listings of the item (step 3) */
static void step_view_graph(Op *op);
static void step_view_listings(Op *op);

static void step_view_info(Op *op) {
    op->next = step_view_graph;
    cJSON *f = keep(op, social_frame("r"));
    add_str(f, "itemsales");
    add_null(f);
    add_null(f);
    add_null(f);
    add_null(f);
    add_null(f);
    add_int(f, op->a.id);
    market_page(op->s, "itemsales", f);
}

static void step_view_graph(Op *op) {
    op->next = step_view_listings;
    cJSON *f = social_frame("st");
    add_int(f, op->a.id);
    add_int(f, 3); /* the view's default: 30 days */
    op_call(op, sh_item_stats, f);
}

static void step_view_listings(Op *op) {
    op->next = NULL;
    cJSON *f = social_frame("fil");
    add_int(f, 0);
    add_str(f, "");
    add_null(f);
    add_null(f);
    add_null(f);
    add_int(f, 0);
    add_str(f, "1");
    op_call(op, sh_market_filter, f);
}

/* {stats, owners, sales, listings, stackable, wearable}: the view's four parts. stackable true: KrunkNative keeps
 * every item as a stack (inventory counts; a painted copy is a copy of a stacked item, not a mint), and 10.0.3 adds
 * its Mints (and Wear) tabs only for stackable === false, a per-copy minted item whose instances it lists
 * (deob-10.0.3.js:123271-123281, 123335); KrunkNative has no instances (/items/:id/instances is []). wearable false:
 * the skins file has no wear. */
static void reply_view(Op *op) {
    if (op_failed(op)) return;
    const cJSON *info = op_arg(op, "0", 3);
    const cJSON *owners = op_arg(op, "0", 4);
    const cJSON *sales = op_arg(op, "gd", 1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "stats", cJSON_IsObject(info) ? cJSON_Duplicate(info, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(o, "owners", cJSON_IsArray(owners) ? cJSON_Duplicate(owners, 1) : cJSON_CreateArray());
    cJSON_AddItemToObject(o, "sales", cJSON_IsArray(sales) ? cJSON_Duplicate(sales, 1) : cJSON_CreateArray());
    cJSON *listings = cJSON_CreateArray();
    const cJSON *rows = op_arg(op, "fr", 2);
    cJSON *r;
    if (cJSON_IsArray(rows)) {
        cJSON_ArrayForEach(r, rows) {
            if (kn_json_int(r, "i", -1) == op->a.id) cJSON_AddItemToArray(listings, listing_row(r));
        }
    }
    cJSON_AddItemToObject(o, "listings", listings);
    cJSON_AddTrueToObject(o, "stackable");
    cJSON_AddFalseToObject(o, "wearable");
    v10_reply_data(op->req, 200, o);
}

static void h_item_view(kn_http_req *req, void *ud) {
    (void) ud;
    if (!item_known(req)) return;
    Args a;
    memset(&a, 0, sizeof a);
    a.id = (int64_t) strtoll(kn_http_param(req, "id"), NULL, 10);
    route_begin(req, 0, 0, step_view_info, reply_view, &a, NULL);
}

/* ---- market writes: a login, the email gate, the socket's handler */

/* ["li", skin, skin, uid, price, {op}] -> ["lir", {cnt, funds, listingId, fee} | {response}, uid] */
static void step_list(Op *op) {
    const int64_t item = kn_json_int(op->body, "item_id", -1);
    cJSON *f = social_frame("li");
    add_int(f, item);
    add_int(f, kn_json_int(op->body, "skin_id", item));
    add_int(f, 0);
    cJSON_AddItemToArray(f, copy_or_null(op->body, "price"));
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_market_list, f);
}

static void reply_list(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "lir", 1);
    /* the listing cap: the socket's {response: "MAX"} (io_lir "Max Items on Sale") is an error here; v10.0.3 checks
     * only res.error and would say "Item listed successfully" (deob-10.0.3.js:122591, 149138) */
    if (cJSON_IsObject(v) && !strcmp(kn_json_str(v, "response", ""), "MAX")) {
        err_reply(op->req, 400, ERR_MARKET, "BAD_REQUEST", "Max Items on Sale"); /* MARKET_MAX_ITEMS */
        return;
    }
    v10_reply_data(op->req, 200, cJSON_IsObject(v) ? cJSON_Duplicate(v, 1) : cJSON_CreateObject());
}

static void h_market_list(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 1, 1, step_list, reply_list, &a, body_of(req));
}

/* ["pi", listing, uid, {op}] -> ["br", {f, i, sid, m, price} | null, uid] */
static void step_purchase(Op *op) {
    cJSON *f = social_frame("pi");
    add_int(f, kn_json_int(op->body, "market_id", 0));
    add_int(f, 0);
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_market_buy, f);
}

static void reply_purchase(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "br", 1);
    if (!cJSON_IsObject(v)) {
        err_reply(op->req, 400, ERR_MARKET, "BAD_REQUEST", "Item Unavailable"); /* MARKET_LISTING_GONE */
        return;
    }
    v10_reply_data(op->req, 200, cJSON_Duplicate(v, 1));
}

static void h_market_purchase(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 1, 1, step_purchase, reply_purchase, &a, body_of(req));
}

/* ["uli", skin, listing, uid, {op}] -> ["ulir", "SUCCESS" | "SOLD", uid] | ["ulir", null, uid, message] */
static void step_unlist(Op *op) {
    cJSON *f = social_frame("uli");
    add_int(f, 0);
    add_int(f, kn_json_int(op->body, "market_id", 0));
    add_int(f, 0);
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_market_unlist, f);
}

static void reply_unlist(Op *op) {
    if (op_failed(op)) return;
    const cJSON *r = op_arg(op, "ulir", 1);
    /* a listing sold meanwhile: an error, not 200 {response: "SOLD"}, which v10.0.3 would report as unlisted
     * (deob-10.0.3.js:149159-149185) */
    if (cJSON_IsString(r) && !strcmp(r->valuestring, "SOLD")) {
        v10_reply_error(op->req, 400, "MARKET_ITEM_ALREADY_SOLD", "This item has already been sold.");
        return;
    }
    if (cJSON_IsString(r)) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "response", r->valuestring);
        v10_reply_data(op->req, 200, o);
        return;
    }
    const cJSON *m = op_arg(op, "ulir", 3);
    err_reply(op->req, 400, ERR_MARKET, "BAD_REQUEST", cJSON_IsString(m) ? m->valuestring : "Failed. Try Again");
}

static void h_market_unlist(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 1, 1, step_unlist, reply_unlist, &a, body_of(req));
}

/* ["qsi", mode, skin, uid, amount, {op}] (mode 0 KR, 1 Junk, 2 destroy) -> ["qsr", {c, f, kr, j, jnk, sold} | null] */
static void step_quick_sell(Op *op) {
    cJSON *f = social_frame("qsi");
    add_int(f, kn_json_int(op->body, "action", 0));
    add_int(f, kn_json_int(op->body, "item_id", -1));
    add_int(f, 0);
    add_int(f, kn_json_int(op->body, "amount", 0));
    /* a painted copy's sale names the copy (skin_id, the official single sale): the frame's ext carries it */
    cJSON *ext = ext_of(op->body);
    const int64_t copy = kn_json_int(op->body, "skin_id", 0);
    if (copy >= V10_COPY_ID_BASE) cJSON_AddNumberToObject(ext, "copy", (double) copy);
    cJSON_AddItemToArray(f, ext);
    op_call(op, sh_quick_sell, f);
}

static void reply_quick_sell(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "qsr", 1);
    if (!cJSON_IsObject(v)) {
        v10_reply_error(op->req, 400, "MARKET_QUICK_SELL_NOT_SELLABLE", "Item cant be quick sold.");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "funds", (double) kn_json_int(v, "f", 0));
    /* v10.0.3 sets the account's junk to data.junk (deob-10.0.3.js:147686, 147736): the balance after the sale, as Junk */
    cJSON_AddNumberToObject(o, "junk", kn_json_num(v, "j", 0));
    cJSON_AddNumberToObject(o, "count", (double) kn_json_int(v, "c", 0));
    cJSON_AddNumberToObject(o, "kr", (double) kn_json_int(v, "kr", 0));
    cJSON_AddNumberToObject(o, "jnk", kn_json_num(v, "jnk", 0));
    cJSON_AddNumberToObject(o, "sold", (double) kn_json_int(v, "sold", 0));
    v10_reply_data(op->req, 200, o);
}

static void h_market_quick_sell(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body = body_of(req);
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 1, 1, step_quick_sell, reply_quick_sell, &a, body);
}

/* ["pbm", skin, {op}] -> ["bmr", {item, kr, f} | null] */
static void step_bm_purchase(Op *op) {
    cJSON *f = social_frame("pbm");
    add_int(f, kn_json_int(op->body, "item_index", -1));
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_blackm_buy, f);
}

static void reply_bm_purchase(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "bmr", 1);
    if (!cJSON_IsObject(v)) {
        v10_reply_text_error(op->req, 400, "Item Unavailable"); /* no 10.0.3 code says it: the text, shown as is */
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "item", (double) kn_json_int(v, "item", -1));
    cJSON_AddNumberToObject(o, "kr", (double) kn_json_int(v, "kr", 0));
    cJSON_AddNumberToObject(o, "funds", (double) kn_json_int(v, "f", 0));
    v10_reply_data(op->req, 200, o);
}

static void h_bm_purchase(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    route_begin(req, 1, 1, step_bm_purchase, reply_bm_purchase, &a, body_of(req));
}

/* ---- trades: the writes */

/* ["ctr", receiver, give, take, message, {op}] -> ["trR", {trid, funds, fee}, null, "Offer Sent"] */
static void step_trade_create(Op *op) {
    cJSON *f = social_frame("ctr");
    add_int(f, kn_json_int(op->body, "target_player_id", 0));
    cJSON_AddItemToArray(f, copy_or_null(op->body, "my_offer"));
    cJSON_AddItemToArray(f, copy_or_null(op->body, "their_offer"));
    add_str(f, kn_json_str(op->body, "message", ""));
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_trade_create, f);
}

/* ["trR", result, tradeId, message]: the result object */
static void reply_trade_result(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "trR", 1);
    v10_reply_data(op->req, 200, cJSON_IsObject(v) ? cJSON_Duplicate(v, 1) : cJSON_CreateObject());
}

static void h_trade_create(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    a.ectx = ERR_TRADE;
    route_begin(req, 1, 1, step_trade_create, reply_trade_result, &a, body_of(req));
}

/* ["trA", tradeId, null, {op}] -> ["trR", {trid, funds}, tradeId, "Trade Accepted"] */
static void step_trade_accept(Op *op) {
    cJSON *f = social_frame("trA");
    add_int(f, op->a.id);
    add_null(f);
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_trade_accept, f);
}

static void h_trade_accept(kn_http_req *req, void *ud) {
    (void) ud;
    const char *p = kn_http_param(req, "id");
    Args a;
    memset(&a, 0, sizeof a);
    a.id = (int64_t) strtoll(p ? p : "0", NULL, 10);
    a.ectx = ERR_TRADE;
    route_begin(req, 1, 1, step_trade_accept, reply_trade_result, &a, body_of(req));
}

/* ["trC", tradeId, null, {op}] -> ["trR", {trid, funds}, tradeId, msg]: the result, with success */
static void step_trade_cancel(Op *op) {
    cJSON *f = social_frame("trC");
    add_int(f, op->a.id);
    add_null(f);
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_trade_cancel, f);
}

static void reply_trade_cancel(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "trR", 1);
    cJSON *o = cJSON_IsObject(v) ? cJSON_Duplicate(v, 1) : cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    v10_reply_data(op->req, 200, o);
}

static void h_trade_cancel(kn_http_req *req, void *ud) {
    (void) ud;
    const char *p = kn_http_param(req, "id");
    Args a;
    memset(&a, 0, sizeof a);
    a.id = (int64_t) strtoll(p ? p : "0", NULL, 10);
    a.ectx = ERR_TRADE;
    route_begin(req, 1, 1, step_trade_cancel, reply_trade_cancel, &a, body_of(req));
}

/* ["trCA", {op}] -> ["pRes", "SUCCESS", null, "Declined All"] */
static void step_trade_clear(Op *op) {
    cJSON *f = social_frame("trCA");
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_trade_clear, f);
}

static void reply_trade_clear(Op *op) {
    if (op_failed(op)) return;
    const cJSON *r = op_arg(op, "pRes", 1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", cJSON_IsString(r) && !strcmp(r->valuestring, "SUCCESS"));
    v10_reply_data(op->req, 200, o);
}

static void h_trade_clear(kn_http_req *req, void *ud) {
    (void) ud;
    Args a;
    memset(&a, 0, sizeof a);
    a.ectx = ERR_TRADE;
    route_begin(req, 1, 1, step_trade_clear, reply_trade_clear, &a, body_of(req));
}

/* ---- players: gift and verify (the writes) */

/* ["kr", name, amount, message, {op}] -> ["gir", {funds, amount, fee}] */
static void step_gift(Op *op) {
    cJSON *f = social_frame("kr");
    add_str(f, op->a.name);
    cJSON_AddItemToArray(f, copy_or_null(op->body, "amount"));
    add_str(f, kn_json_str(op->body, "message", ""));
    cJSON_AddItemToArray(f, ext_of(op->body));
    op_call(op, sh_gift, f);
}

static void reply_gift(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "gir", 1);
    v10_reply_data(op->req, 200, cJSON_IsObject(v) ? cJSON_Duplicate(v, 1) : cJSON_CreateObject());
}

static void h_player_gift(kn_http_req *req, void *ud) {
    (void) ud;
    const char *p = kn_http_param(req, "name");
    Args a;
    memset(&a, 0, sizeof a);
    snprintf(a.name, sizeof a.name, "%s", p ? p : "");
    a.ectx = ERR_GIFT;
    route_begin(req, 1, 1, step_gift, reply_gift, &a, body_of(req));
}

/* ["ver", name, 0|1] -> ["verr", true | null]: the socket answers null when the staff check or the player fails */
static void step_verify(Op *op) {
    cJSON *f = social_frame("ver");
    add_str(f, op->a.name);
    add_int(f, op->a.num);
    op_call(op, sh_verify, f);
}

static void reply_verify(Op *op) {
    if (op_failed(op)) return;
    const cJSON *v = op_arg(op, "verr", 1);
    if (cJSON_IsTrue(v)) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "featured", op->a.num != 0);
        v10_reply_data(op->req, 200, o);
        return;
    }
    /* nothing changed (the socket's null: no staff right, or no such player): an error, so v10.0.3 does not flip
     * the badge on screen (it falls back to the requested value on a null data, deob-10.0.3.js:125561-125567) */
    v10_reply_text_error(op->req, 403, "Access Denied");
}

static void h_player_verify(kn_http_req *req, void *ud) {
    (void) ud;
    const char *p = kn_http_param(req, "name");
    cJSON *body = body_of(req);
    Args a;
    memset(&a, 0, sizeof a);
    snprintf(a.name, sizeof a.name, "%s", p ? p : "");
    a.num = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "featured")) ? 1 : 0;
    route_begin(req, 1, 0, step_verify, reply_verify, &a, body);
}

int v10_market_init(Brain *b) {
    s_brain = b;
    /* market */
    kn_http_route(b->http, "GET", "/market/listings", h_market_listings, NULL);
    kn_http_route(b->http, "GET", "/market/inventory", h_market_inventory, NULL);
    kn_http_route(b->http, "GET", "/market/sales", h_market_sales, NULL);
    kn_http_route(b->http, "GET", "/market/stats", h_market_stats, NULL);
    kn_http_route(b->http, "GET", "/market/items/:id/sales", h_market_item_sales, NULL);
    kn_http_route(b->http, "GET", "/market/items/:id/sales-statistics", h_market_item_stats, NULL);
    kn_http_route(b->http, "POST", "/market/list", h_market_list, NULL);
    kn_http_route(b->http, "POST", "/market/purchase", h_market_purchase, NULL);
    kn_http_route(b->http, "POST", "/market/unlist", h_market_unlist, NULL);
    kn_http_route(b->http, "POST", "/market/quick-sell", h_market_quick_sell, NULL);
    kn_http_route(b->http, "GET", "/market/black-market", h_bm_owned, NULL);
    kn_http_route(b->http, "GET", "/market/black-market/rank", h_bm_rank, NULL);
    kn_http_route(b->http, "POST", "/market/black-market/purchase", h_bm_purchase, NULL);
    kn_http_route(b->http, "GET", "/market/history", h_market_history, NULL);

    /* items */
    kn_http_route(b->http, "GET", "/items/:id/view", h_item_view, NULL);
    kn_http_route(b->http, "GET", "/items/:id/sales", h_item_graph, NULL);
    kn_http_route(b->http, "GET", "/items/:id/instances", h_item_instances, NULL);
    kn_http_route(b->http, "POST", "/items/:id/paint", h_item_paint, NULL);
    kn_http_route(b->http, "POST", "/items/:id/style", h_item_style, NULL);

    /* trades */
    kn_http_route(b->http, "GET", "/trades/history", h_trades_history, NULL);
    kn_http_route(b->http, "POST", "/trades/create", h_trade_create, NULL);
    kn_http_route(b->http, "POST", "/trades/:id/accept", h_trade_accept, NULL);
    kn_http_route(b->http, "POST", "/trades/:id/cancel", h_trade_cancel, NULL);
    kn_http_route(b->http, "POST", "/trades/decline-all", h_trade_clear, NULL);
    kn_http_route(b->http, "GET", "/trades/inventory/:name", h_trade_inventory, NULL);
    kn_http_route(b->http, "GET", "/trades/player-search", h_player_search, NULL);

    /* players, HTTP only */
    kn_http_route(b->http, "GET", "/players/:name/sales", h_player_sales, NULL);
    kn_http_route(b->http, "GET", "/players/:name/listings", h_player_listings, NULL);
    kn_http_route(b->http, "POST", "/players/:name/verify", h_player_verify, NULL);
    kn_http_route(b->http, "POST", "/players/:name/gift", h_player_gift, NULL);

    return 0;
}
