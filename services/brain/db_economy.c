/*
 * services/brain/db_economy.c - every KR / item movement between players: KR gifts, the market
 * (list, unlist, buy, quick sell) and trade offers (create, accept, decline / cancel, decline
 * all, expiry), plus the read-only economy pages (trade list, item sales, listings, KR history).
 * Rules and protocol: doc/v7/economy.md; schema: migrations 0102 + 0111_economy.
 *
 * Every proc here has no .pg text: the C implementation runs on every driver (PostgreSQL
 * included) on a pool connection inside ONE transaction (knsvc/db.h kn_proc: BEGIN .. COMMIT,
 * rolled back when the proc returns an error), so all three drivers share exactly the same
 * locking, idempotency and ledger code. The SQL functions of 0104 (kn_gift, kn_market_*,
 * kn_trade_*) stay in the schema, unreferenced.
 *
 * Invariants:
 *  - one transaction per client action; a refusal (dbx_refuse / econ_refuse) rolls everything
 *    back, including the idempotency claim, so a retry re-validates;
 *  - lock order: the listing / trade row first, then the players rows in ascending player id
 *    (econ_lock), then inventory rows (db_add_skin's UPDATE); two actions on the same rows queue
 *    instead of deadlocking;
 *  - player_funds >= 0 and inventory.count >= 0 (schema CHECKs, and the guarded UPDATEs of
 *    db_add_funds_ex / db_add_skin refuse instead of going negative);
 *  - one kr_transactions row per KR movement (source = reason, ref, counterparty_id, fee) and one
 *    skin_transactions row per item movement, written in the same transaction;
 *  - an item that is listed or offered leaves the owner's inventory (escrow) until the listing is
 *    sold / unlisted or the offer is accepted / closed / expired, so it can never be in two
 *    listings or offers; KR offered in a trade is escrowed the same way;
 *  - economy_requests (account, kind, key) makes a retried frame replay the first result.
 *
 * Parameters are text (kn_proc). The first four of every client action are the same:
 *   $0 actor account id, $1 idempotency key, $2 rate limit (actions per minute of this kind,
 *   0 = none), $3 feature ('gift' | 'market' | 'trade' | '' = none) checked against feature_bans.
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"
#include "v10_paint.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static const char *nz(const char *s) { return s ? s : ""; }

/* left(s, max chars) for UTF-8 text */
static char *utf8_left(const char *s, int max) {
    s = nz(s);
    const char *e = s;
    for (int k = 0; *e && k < max; k++) {
        e++;
        while ((*e & 0xC0) == 0x80) e++;
    }
    char *o = malloc((size_t) (e - s) + 1);
    if (!o) return NULL;
    memcpy(o, s, (size_t) (e - s));
    o[e - s] = 0;
    return o;
}

static cJSON *html_item(const char *s) {
    char *h = db_html(s);
    cJSON *v = cJSON_CreateString(h ? h : "");
    free(h);
    return v;
}

/* ------------------------------------------------------------------ common */

static kn_db_result *econ_refuse(Dbx *x, const char *msg) {
    dbx_refuse(x, msg);
    return dbx_finish(x, NULL);
}

/* A player row the economy reads, locked by econ_lock. */
typedef struct EconPlayer {
    int64_t id, score, funds;
    int notrade, premium;
} EconPlayer;

static int econ_player(Dbx *x, int64_t id, EconPlayer *out) {
    memset(out, 0, sizeof *out);
    kn_db_result *r = dbx_q(x, SQL(.any = "SELECT player_score, player_notrade, player_premium, player_funds FROM players"
                                          " WHERE player_id = $1 {for_update}"),
                            KN_DB_ARGS(I64S(id)));
    if (!r) return -1;
    const int found = kn_db_rows(r) > 0;
    if (found) {
        out->id = id;
        out->score = kn_db_get_int64(r, 0, 0, 0);
        out->notrade = kn_db_get_int64(r, 0, 1, 0) != 0;
        out->premium = kn_db_get_int64(r, 0, 2, 0) > 0;
        out->funds = kn_db_get_int64(r, 0, 3, 0);
    }
    kn_db_result_free(r);
    return found;
}

/* Lock (SELECT .. FOR UPDATE) and read the players of a transfer in ascending id order, the
 * order every economy transaction takes them in. b may be 0 / == a. 0 ok, -1 failure / refusal. */
static int econ_lock(Dbx *x, int64_t a, EconPlayer *pa, int64_t b, EconPlayer *pb) {
    EconPlayer tmp;
    if (b <= 0 || b == a) {
        const int r = econ_player(x, a, pa);
        if (pb) *pb = *pa;
        return r < 0 ? -1 : r == 0 ? dbx_refuse(x, "Account not found.") : 0;
    }
    const int first_a = a < b;
    const int r1 = econ_player(x, first_a ? a : b, first_a ? pa : (pb ? pb : &tmp));
    const int r2 = r1 > 0 ? econ_player(x, first_a ? b : a, first_a ? (pb ? pb : &tmp) : pa) : r1;
    if (r1 < 0 || r2 < 0) return -1;
    if (r1 == 0 || r2 == 0) return dbx_refuse(x, "Player not found");
    return 0;
}

/* Start a client action (params $0-$3, see the top of the file):
 *   0  first run: go ahead (the key is claimed, the bans and the rate limit passed);
 *   1  a previous run with the same key committed: *replay = its JSON result (malloc'd);
 *  -1  refused / failed (x->err).
 * The claim INSERT is the lock: a concurrent retry with the same key waits on the primary key
 * until this transaction ends, then finds the row and replays it. */
static int econ_open(Dbx *x, int n, const char *const *p, const char *kind, char **replay) {
    *replay = NULL;
    const int64_t actor = PI(0);
    const char *key = P(1), *feature = P(3);
    if (actor <= 0) return dbx_refuse(x, "Must be logged in");
    if (!key || !*key || strlen(key) > 64) return dbx_refuse(x, "Invalid request");
    const int64_t claimed = dbx_exec(x,
        SQL(.any = "{insert_ignore} INTO economy_requests (account_id, kind, req_key) VALUES ($1, $2, $3) {ignore_conflicts}"),
        KN_DB_ARGS(I64S(actor), kind, key));
    if (claimed < 0) return -1;
    if (claimed == 0) {
        char *json = dbx_text(x, SQL(.any = "SELECT response FROM economy_requests WHERE account_id = $1 AND kind = $2"
                                            " AND req_key = $3"),
                              KN_DB_ARGS(I64S(actor), kind, key));
        if (dbx_failed(x)) {
            free(json);
            return -1;
        }
        *replay = json ? json : strdup("null");
        return 1;
    }
    if (feature && *feature &&
        dbx_exists(x, SQL(.any = "SELECT 1 FROM feature_bans WHERE account_id = $1 AND feature = $2 AND revoked_at IS NULL"
                                 " AND (expires_at IS NULL OR expires_at > {now})"),
                   KN_DB_ARGS(I64S(actor), feature))) {
        char msg[96];
        snprintf(msg, sizeof msg, "You are banned from using the %s", !strcmp(feature, "gift") ? "KR gifts" : feature);
        return dbx_refuse(x, msg);
    }
    if (db_active_ban(x, actor, NULL, NULL, 0, NULL)) return dbx_refuse(x, "Account banned");
    return dbx_failed(x) ? -1 : 0;
}

/* The per-account rate limit ($2 actions of this kind a minute), checked once the actor's players
 * row is locked (econ_lock), so parallel requests of one account count each other. -1 = refused. */
static int econ_rate(Dbx *x, int n, const char *const *p, const char *kind) {
    const int64_t rate = PI(2);
    if (rate > 0 &&
        dbx_int(x, SQL(.any = "SELECT count(*) FROM economy_requests WHERE account_id = $1 AND kind = $2 AND at > {ago:60}"),
                KN_DB_ARGS(P(0), kind), 0) > rate)
        return dbx_refuse(x, "Slow down");
    return dbx_failed(x) ? -1 : 0;
}

/* 1 when the account has a feature ban in force ('gift' | 'market' | 'trade'). */
static int econ_feature_banned(Dbx *x, int64_t id, const char *feature) {
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM feature_bans WHERE account_id = $1 AND feature = $2 AND revoked_at IS NULL"
                                    " AND (expires_at IS NULL OR expires_at > {now})"),
                      KN_DB_ARGS(I64S(id), feature));
}

/* The other party of a transfer can take part: no trade lock, no account ban. */
static int econ_peer_ok(Dbx *x, const EconPlayer *peer) {
    return !peer->notrade && !db_active_ban(x, peer->id, NULL, NULL, 0, NULL);
}

/* Result of a replayed action (econ_open == 1). */
static kn_db_result *econ_replay(Dbx *x, const char *name, char *json) {
    cJSON *v = json ? cJSON_Parse(json) : NULL;
    free(json);
    /* "replay": the caller answers the client but does not notify anyone again */
    if (cJSON_IsObject(v)) cJSON_AddNumberToObject(v, "replay", 1);
    return dbx_json(x, name, v ? v : cJSON_CreateNull());
}

/* Store the action's result for later retries (same transaction), then return it. */
static kn_db_result *econ_done(Dbx *x, int n, const char *const *p, const char *kind, const char *name, cJSON *o) {
    if (!dbx_failed(x)) {
        char *json = kn_json_print(o);
        dbx_exec(x, SQL(.any = "UPDATE economy_requests SET response = $4 WHERE account_id = $1 AND kind = $2 AND req_key = $3"),
                 KN_DB_ARGS(P(0), kind, P(1), json ? json : "null"));
        free(json);
    }
    return dbx_json(x, name, o);
}

#define ECON_OPEN(kind, name)                                            \
    do {                                                                 \
        char *replay_;                                                   \
        const int o_ = econ_open(&x, n, p, kind, &replay_);              \
        if (o_ < 0) return dbx_finish(&x, NULL);                         \
        if (o_ > 0) return econ_replay(&x, name, replay_);               \
    } while (0)

static int level_of(const EconPlayer *pl) { return db_level(pl->score, 0.03); }

static int refuse_level(Dbx *x, int64_t need) {
    char msg[64];
    snprintf(msg, sizeof msg, "Must be level %lld", (long long) need);
    return dbx_refuse(x, msg);
}

/* ------------------------------------------------------------------ KR gift */

/* $4 receiver name, $5 amount, $6 fee %, $7 min level, $8 message, $9 min amount, $10 max amount
 * -> {funds, to, amount, fee}. v7.1.4 social.js calcGiftFee: fee = ceil(amount * giftFee), paid
 * by the sender on top of the amount; giftMin 10, giftMax 1e7, giftMinLVl 30. */
static kn_db_result *run_gift(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("gift", "kn_gift");
    const int64_t sender = PI(0), amount = PI(5), pct = PI(6) > 0 ? PI(6) : 0;
    if (amount <= 0 || (PI(9) > 0 && amount < PI(9))) {
        char msg[64];
        snprintf(msg, sizeof msg, "%lld KR minimum", (long long) (PI(9) > 0 ? PI(9) : 1));
        return econ_refuse(&x, msg);
    }
    if (PI(10) > 0 && amount > PI(10)) return econ_refuse(&x, "Invalid amount");
    const int64_t to = db_account_id(&x, nz(P(4)));
    if (to <= 0) return econ_refuse(&x, "Player not found");
    if (to == sender) return econ_refuse(&x, "Cant gift yourself");
    EconPlayer me, peer;
    if (econ_lock(&x, sender, &me, to, &peer) < 0 || econ_rate(&x, n, p, "gift") < 0) return dbx_finish(&x, NULL);
    if (me.notrade) return econ_refuse(&x, "Account is trade locked");
    if (!econ_peer_ok(&x, &peer)) return econ_refuse(&x, "Player can not receive gifts");
    if (PI(7) > 0 && level_of(&me) < PI(7)) {
        refuse_level(&x, PI(7));
        return dbx_finish(&x, NULL);
    }
    /* ceil in integers: amount and pct are whole numbers, so no float rounding (10 KR -> 1 KR) */
    const int64_t fee = (amount * pct + 99) / 100;
    if (me.funds < amount + fee) return econ_refuse(&x, "Not enough KR");
    int64_t funds = 0;
    db_add_funds_ex(&x, sender, -(amount + fee), "gift-out", I64S(to), to, fee, &funds);
    db_add_funds_ex(&x, to, amount, "gift-in", P(0), sender, 0, NULL);
    char *msg = utf8_left(P(8), 300);
    dbx_exec(&x, SQL(.any = "INSERT INTO mail (account_id, sender, subject, body, kr)"
                            " SELECT $1, a.username, 'KR Gift', $3, 0 FROM accounts a WHERE a.id = $2"),
             KN_DB_ARGS(I64S(to), P(0), msg ? msg : ""));
    free(msg);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "funds", (double) funds);
    cJSON_AddNumberToObject(o, "to", (double) to);
    cJSON_AddNumberToObject(o, "amount", (double) amount);
    cJSON_AddNumberToObject(o, "fee", (double) fee);
    return econ_done(&x, n, p, "gift", "kn_gift", o);
}
const kn_proc P_GIFT = {"gift", NULL, run_gift};

/* ------------------------------------------------------------------ market */

/* The paint columns of a value: the colour (NULL for a shader paint) and the shader's text (NULL for a colour). */
static void paint_cols(const CopyPaint *p, const char **color, const char **shader, char buf[24]) {
    if (p->color[0]) {
        *color = p->color;
        *shader = NULL;
    } else {
        snprintf(buf, 24, "%lld", (long long) p->shader);
        *color = NULL;
        *shader = buf;
    }
}

/* $4 skin, $5 price, $6 fee %, $7 fee bypass, $8 sale max, $9 max listings, $10 min level, $11 the copy id (0 for a
 * stack's unit) -> {cnt, funds, listingId, fee} | {response: "MAX"}. v7.1.4 calcFee: floor(price * marketFee)
 * unless price <= marketFeeBypass, paid when listing (no fee on the sale). A painted copy (a copy id) is listed as
 * that copy: its paint is kept on the listing (paint_color / paint_shader) and given back with the copy (doc/v10/paint-copy.md). */
static kn_db_result *run_market_list(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("list", "kn_market_list");
    const int64_t seller = PI(0), skin = PI(4), price = PI(5), pct = PI(6) > 0 ? PI(6) : 0, copy = PI(11);
    if (skin < 0 || !P(4)) return econ_refuse(&x, "Item not found");
    if (price < 1 || (PI(8) > 0 && price > PI(8))) return econ_refuse(&x, "Invalid price");
    EconPlayer me;
    if (econ_lock(&x, seller, &me, 0, NULL) < 0 || econ_rate(&x, n, p, "list") < 0) return dbx_finish(&x, NULL);
    if (me.notrade) return econ_refuse(&x, "Account is trade locked");
    if (PI(10) > 0 && level_of(&me) < PI(10)) {
        refuse_level(&x, PI(10));
        return dbx_finish(&x, NULL);
    }
    cJSON *o = cJSON_CreateObject();
    if (PI(9) > 0 && dbx_int(&x, SQL(.any = "SELECT count(*) FROM market_listings WHERE seller_id = $1"
                                            " AND sold_at IS NULL AND cancelled_at IS NULL"),
                             KN_DB_ARGS(P(0)), 0) >= PI(9)) {
        cJSON_AddStringToObject(o, "response", "MAX"); /* io_lir: "Max Items on Sale" */
        return econ_done(&x, n, p, "list", "kn_market_list", o);
    }
    const int64_t fee = price <= PI(7) ? 0 : price * pct / 100; /* floor */
    int64_t cnt = 0, funds = 0, listing = 0;
    char ref[48];
    listing = dbx_insert_id(&x, SQL(.pg = "INSERT INTO market_listings (seller_id, skin_id, price) VALUES ($1, $2, $3) RETURNING id",
                                    .sqlite = "INSERT INTO market_listings (seller_id, skin_id, price) VALUES ($1, $2, $3) RETURNING id",
                                    .mysql = "INSERT INTO market_listings (seller_id, skin_id, price) VALUES ($1, $2, $3)"),
                            KN_DB_ARGS(P(0), P(4), P(5)));
    snprintf(ref, sizeof ref, "list:%lld", (long long) listing);
    /* the item leaves the inventory while it is listed (escrow); "Item not owned" refuses it all */
    if (copy >= V10_COPY_ID_BASE) {
        CopyPaint paint;
        if (copy_take(&x, seller, copy, skin, &paint, "market", ref) < 0) {
            cJSON_Delete(o);
            return dbx_finish(&x, NULL);
        }
        const char *color, *shader;
        char shader_t[24];
        paint_cols(&paint, &color, &shader, shader_t);
        dbx_exec(&x, SQL(.any = "UPDATE market_listings SET paint_color = $2, paint_shader = $3 WHERE id = $1"),
                 KN_DB_ARGS(I64S(listing), color, shader));
    } else {
        db_add_skin(&x, seller, skin, -1, "market", ref, NULL);
    }
    db_add_funds_ex(&x, seller, -fee, "market-fee", I64S(listing), 0, fee, &funds);
    /* the stack's count is its unpainted units: the painted copies are not listed as the stack */
    cnt = db_unpainted_units(&x, seller, skin);
    cJSON_AddNumberToObject(o, "cnt", (double) cnt);
    cJSON_AddNumberToObject(o, "funds", (double) funds);
    cJSON_AddNumberToObject(o, "listingId", (double) listing);
    cJSON_AddNumberToObject(o, "fee", (double) fee);
    return econ_done(&x, n, p, "list", "kn_market_list", o);
}
const kn_proc P_MARKET_LIST = {"market_list", NULL, run_market_list};

/* (actor, copy id or the stack's store index, item store index, paint store index, paint colour ("" for a
 * shader), paint shader (-1 for a colour)) -> {copy_id, item_id, paint_item_id, m, count}: v10.0.3's setPaint
 * (doc/v10/paint-copy.md). A copy id (>= V10_COPY_ID_BASE) names an owned painted copy of the item and its paint
 * is replaced; a store index names the stack, and one unpainted unit of the item becomes a new copy with the
 * paint. One paint is consumed (db_add_skin: the audit row, the paint's own guard). */
static kn_db_result *run_item_paint(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t actor = PI(0), copy = PI(1), item = PI(2), paint = PI(3), shader = PI(5);
    const char *colour = P(4) && P(4)[0] ? P(4) : NULL;
    const char *shader_text = shader >= 0 ? P(5) : NULL;
    if (item < 0 || paint <= 0) return dbx_finish(&x, kn_db_result_user_error("Item not found"));
    if (dbx_int(&x, SQL(.any = "SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2"),
                KN_DB_ARGS(I64S(actor), I64S(paint)), 0) <= 0)
        return dbx_finish(&x, kn_db_result_user_error("You don't have any of that paint left."));
    if (copy >= V10_COPY_ID_BASE) {
        const int64_t skin = dbx_int(&x, SQL(.any = "SELECT skin_id FROM skin_copies WHERE copy_id = $1 AND account_id = $2"),
                                     KN_DB_ARGS(I64S(copy), I64S(actor)), -1);
        if (skin < 0) return dbx_finish(&x, kn_db_result_user_error("You don't own that copy."));
        if (skin != item) return dbx_finish(&x, kn_db_result_user_error("That copy is not that item."));
    } else {
        if (copy != item) return dbx_finish(&x, kn_db_result_user_error("Item not found"));
        const int64_t owned = dbx_int(&x, SQL(.any = "SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2"),
                                      KN_DB_ARGS(I64S(actor), I64S(item)), 0);
        const int64_t painted = dbx_int(&x, SQL(.any = "SELECT count(*) FROM skin_copies WHERE account_id = $1 AND skin_id = $2"),
                                        KN_DB_ARGS(I64S(actor), I64S(item)), 0);
        if (owned - painted <= 0) return dbx_finish(&x, kn_db_result_user_error("You don't have that item left unpainted."));
    }
    char ref[32];
    snprintf(ref, sizeof ref, "item:%lld", (long long) item);
    int64_t left = 0;
    if (db_add_skin(&x, actor, paint, -1, "paint", ref, &left) < 0) return dbx_finish(&x, NULL);
    int64_t copy_id = copy;
    if (copy >= V10_COPY_ID_BASE) {
        if (dbx_exec(&x, SQL(.any = "UPDATE skin_copies SET paint_color = $2, paint_shader = $3 WHERE copy_id = $1"),
                     KN_DB_ARGS(I64S(copy), colour, shader_text)) < 0)
            return dbx_finish(&x, NULL);
    } else {
        copy_id = dbx_insert_id(&x,
            SQL(.pg = "INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1::int, $2::int, $3, $4::int)"
                      " RETURNING copy_id",
                .sqlite = "INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1, $2, $3, $4)"
                          " RETURNING copy_id",
                .mysql = "INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1, $2, $3, $4)"),
            KN_DB_ARGS(I64S(actor), I64S(item), colour, shader_text));
        if (copy_id < 0) return dbx_finish(&x, NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "copy_id", (double) copy_id);
    cJSON_AddNumberToObject(o, "item_id", (double) item);
    cJSON_AddNumberToObject(o, "paint_item_id", (double) paint);
    cJSON *m = v10_paint_meta(colour, (int) shader);
    cJSON_AddItemToObject(o, "m", m ? m : cJSON_CreateObject());
    cJSON_AddNumberToObject(o, "count", (double) left);
    return dbx_json(&x, "paint", o);
}
const kn_proc P_ITEM_PAINT = {"item_paint", NULL, run_item_paint};

/* $4 listing -> {r: "SUCCESS" | "SOLD", i: skin} */
static kn_db_result *run_market_unlist(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("unlist", "kn_market_unlist");
    kn_db_result *l = dbx_q(&x, SQL(.any = "SELECT skin_id, sold_at IS NOT NULL AS sold__b, cancelled_at IS NOT NULL AS gone__b,"
                                           " paint_color, paint_shader FROM market_listings WHERE id = $2 AND seller_id = $1 {for_update}"),
                            KN_DB_ARGS(P(0), P(4)));
    if (!l) return dbx_finish(&x, NULL);
    const int found = kn_db_rows(l) > 0;
    const int64_t skin = found ? kn_db_get_int64(l, 0, 0, 0) : -1;
    const int sold = found && kn_db_get_bool(l, 0, 1, 0), gone = found && kn_db_get_bool(l, 0, 2, 0);
    /* a painted copy's listing: its paint goes back with the copy */
    CopyPaint paint;
    memset(&paint, 0, sizeof paint);
    paint.shader = -1;
    if (found && !kn_db_isnull(l, 0, 3)) snprintf(paint.color, sizeof paint.color, "%s", kn_db_get(l, 0, 3));
    if (found && !kn_db_isnull(l, 0, 4)) paint.shader = kn_db_get_int64(l, 0, 4, 0);
    const int painted = paint.color[0] != '\0' || paint.shader >= 0;
    kn_db_result_free(l);
    cJSON *o = cJSON_CreateObject();
    if (sold) {
        cJSON_AddStringToObject(o, "r", "SOLD");
        cJSON_AddNumberToObject(o, "i", (double) skin);
        return econ_done(&x, n, p, "unlist", "kn_market_unlist", o);
    }
    cJSON_Delete(o);
    if (!found || gone) return econ_refuse(&x, "Listing not found");
    EconPlayer me;
    if (econ_lock(&x, PI(0), &me, 0, NULL) < 0 || econ_rate(&x, n, p, "unlist") < 0) return dbx_finish(&x, NULL);
    char ref[48];
    snprintf(ref, sizeof ref, "unlist:%s", nz(P(4)));
    if (painted) {
        if (copy_give(&x, PI(0), skin, &paint, "market", ref, NULL) < 0) return dbx_finish(&x, NULL);
    } else {
        db_add_skin(&x, PI(0), skin, 1, "market", ref, NULL);
    }
    dbx_exec(&x, SQL(.any = "UPDATE market_listings SET cancelled_at = {now} WHERE id = $1"), KN_DB_ARGS(P(4)));
    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "r", "SUCCESS");
    cJSON_AddNumberToObject(o, "i", (double) skin);
    return econ_done(&x, n, p, "unlist", "kn_market_unlist", o);
}
const kn_proc P_MARKET_UNLIST = {"market_unlist", NULL, run_market_unlist};

/* $4 listing, $5 min level, $6 sale grace (s) -> {f, i, m, sid, seller, price}. The buyer pays the
 * price, the seller receives all of it (v7.1.4: no fee on the sale), the item moves to the buyer
 * and the listing closes, in one transaction. v7.1.4 makes a listing purchasable saleGrace after
 * it was posted (client-side there; enforced here). */
static kn_db_result *run_market_buy(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("buy", "kn_market_buy");
    const int64_t buyer = PI(0);
    /* the listing row is the lock every buyer of it contends for: one wins, the rest see it sold */
    kn_db_result *l = dbx_q(&x, SQL(.any = "SELECT seller_id, skin_id, price, sold_at IS NULL AND cancelled_at IS NULL AS open__b,"
                                           " created_at > {ago:$2} AS fresh__b, paint_color, paint_shader"
                                           " FROM market_listings WHERE id = $1 {for_update}"),
                            KN_DB_ARGS(P(4), I64S(PI(6) > 0 ? PI(6) : 0)));
    if (!l) return dbx_finish(&x, NULL);
    const int found = kn_db_rows(l) > 0 && kn_db_get_bool(l, 0, 3, 0);
    const int fresh = found && PI(6) > 0 && kn_db_get_bool(l, 0, 4, 0);
    const int64_t seller = found ? kn_db_get_int64(l, 0, 0, 0) : 0, skin = found ? kn_db_get_int64(l, 0, 1, 0) : 0,
                  price = found ? kn_db_get_int64(l, 0, 2, 0) : 0;
    /* a painted copy's listing: the buyer gets the copy with its paint */
    CopyPaint paint;
    memset(&paint, 0, sizeof paint);
    paint.shader = -1;
    if (found && !kn_db_isnull(l, 0, 5)) snprintf(paint.color, sizeof paint.color, "%s", kn_db_get(l, 0, 5));
    if (found && !kn_db_isnull(l, 0, 6)) paint.shader = kn_db_get_int64(l, 0, 6, 0);
    const int painted = paint.color[0] != '\0' || paint.shader >= 0;
    kn_db_result_free(l);
    if (!found) return econ_refuse(&x, "Item Unavailable");
    if (seller == buyer) return econ_refuse(&x, "Cant buy your own item");
    if (fresh) return econ_refuse(&x, "Item not available yet");
    EconPlayer me, him;
    if (econ_lock(&x, buyer, &me, seller, &him) < 0 || econ_rate(&x, n, p, "buy") < 0) return dbx_finish(&x, NULL);
    if (me.notrade) return econ_refuse(&x, "Account is trade locked");
    if (!econ_peer_ok(&x, &him)) return econ_refuse(&x, "Item Unavailable"); /* a locked / banned seller does not sell */
    if (PI(5) > 0 && level_of(&me) < PI(5)) {
        refuse_level(&x, PI(5));
        return dbx_finish(&x, NULL);
    }
    if (me.funds < price) return econ_refuse(&x, "Not enough KR");
    int64_t funds = 0;
    char ref[48];
    snprintf(ref, sizeof ref, "buy:%s", nz(P(4)));
    db_add_funds_ex(&x, buyer, -price, "market-buy", P(4), seller, 0, &funds);
    db_add_funds_ex(&x, seller, price, "market-sale", P(4), buyer, 0, NULL);
    if (painted) {
        if (copy_give(&x, buyer, skin, &paint, "market", ref, NULL) < 0) return dbx_finish(&x, NULL);
    } else {
        db_add_skin(&x, buyer, skin, 1, "market", ref, NULL);
    }
    dbx_exec(&x, SQL(.any = "UPDATE market_listings SET buyer_id = $1, sold_at = {now} WHERE id = $2"), KN_DB_ARGS(P(0), P(4)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "f", (double) funds);
    cJSON_AddNumberToObject(o, "i", (double) skin);
    cJSON_AddNumberToObject(o, "sid", (double) skin);
    cJSON_AddNumberToObject(o, "m", (double) PI(4));
    cJSON_AddNumberToObject(o, "seller", (double) seller);
    cJSON_AddNumberToObject(o, "price", (double) price);
    return econ_done(&x, n, p, "buy", "kn_market_buy", o);
}
const kn_proc P_MARKET_BUY = {"market_buy", NULL, run_market_buy};

/* $4 skin, $5 mode (0 KR, 2 destroy), $6 amount (0 one item, 1 all but one, 2 all), $7 KR each, $8 the copy id (0 for
 * a stack) -> {c, f, kr, j, jnk, sold}. KR comes from the game (no counterparty). A stack sells its unpainted units only;
 * a painted copy is sold as one unit (amount 0), its paint goes with it (doc/v10/paint-copy.md). */
static kn_db_result *run_quick_sell(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("qsell", "kn_quick_sell");
    const int64_t me_id = PI(0), skin = PI(4), mode = PI(5), sel = PI(6), each = PI(7) > 0 && mode == 0 ? PI(7) : 0, copy = PI(8);
    if (mode != 0 && mode != 2) return econ_refuse(&x, "Unable to sell");
    EconPlayer me;
    if (econ_lock(&x, me_id, &me, 0, NULL) < 0 || econ_rate(&x, n, p, "qsell") < 0) return dbx_finish(&x, NULL);
    const char *ref = mode == 2 ? "destroy" : NULL;
    int64_t count = 0, funds = me.funds;
    if (copy >= V10_COPY_ID_BASE) {
        if (sel != 0) return econ_refuse(&x, "Unable to sell");
        CopyPaint paint;
        if (copy_take(&x, me_id, copy, skin, &paint, "quicksell", ref) < 0) return dbx_finish(&x, NULL);
        count = 1;
    } else {
        const int64_t owned = db_unpainted_units(&x, me_id, skin);
        count = sel == 2 ? owned : sel == 1 ? owned - 1 : 1;
        if (owned < 1 || count < 1 || count > owned) return econ_refuse(&x, "Unable to sell");
        db_add_skin(&x, me_id, skin, -count, "quicksell", ref, NULL);
    }
    if (each > 0) db_add_funds_ex(&x, me_id, count * each, "quicksell", P(4), 0, 0, &funds);
    const int64_t cnt = db_unpainted_units(&x, me_id, skin);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "c", (double) cnt);
    cJSON_AddNumberToObject(o, "f", (double) funds);
    cJSON_AddNumberToObject(o, "kr", (double) (count * each));
    cJSON_AddNumberToObject(o, "j", 0);
    cJSON_AddNumberToObject(o, "jnk", 0);
    cJSON_AddNumberToObject(o, "sold", (double) count);
    return econ_done(&x, n, p, "qsell", "kn_quick_sell", o);
}
const kn_proc P_QUICK_SELL = {"quick_sell", NULL, run_quick_sell};

/* $4 skin, $5 price, $6 requirement ('' | accAge | stats | accVar), $7 its variable, $8 its amount
 * -> {item, kr, f}. v7.1.4's Black Market (market tab "blackm", pbm -> bmr): a blackM item bought
 * from the game for its store `funds`, if store.checkReq(item, account) holds. */
static kn_db_result *run_blackm_buy(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("bm", "kn_blackm_buy");
    const int64_t me_id = PI(0), skin = PI(4), price = PI(5);
    const char *req = nz(P(6)), *var = nz(P(7)), *amount = P(8);
    const double need = amount ? strtod(amount, NULL) : 0;
    if (skin < 0 || price <= 0) return econ_refuse(&x, "Item Unavailable");
    EconPlayer me;
    if (econ_lock(&x, me_id, &me, 0, NULL) < 0 || econ_rate(&x, n, p, "bm") < 0) return dbx_finish(&x, NULL);
    /* once per account (v7.1.4 marks an owned Black Market item done): never again, even after the
     * item is gone, so buying cheap and quick-selling cannot be repeated */
    if (dbx_int(&x, SQL(.any = "SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2"), KN_DB_ARGS(P(0), P(4)), 0) > 0 ||
        dbx_exists(&x, SQL(.any = "SELECT 1 FROM skin_transactions WHERE account_id = $1 AND skin_id = $2 AND source = 'blackmarket'"),
                   KN_DB_ARGS(P(0), P(4))))
        return econ_refuse(&x, "You already own this item");
    int ok = !*req;
    if (!strcmp(req, "accAge")) {
        ok = dbx_int(&x, SQL(.any = "SELECT {epoch_ms:{now}} - {epoch_ms:created_at} FROM accounts WHERE id = $1"), KN_DB_ARGS(P(0)), 0) >= need;
    } else if (!strcmp(req, "stats") && *var) {
        char *text = dbx_text(&x, SQL(.any = "SELECT player_stats FROM players WHERE player_id = $1"), KN_DB_ARGS(P(0)));
        cJSON *st = text ? cJSON_Parse(text) : NULL;
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(st, var);
        ok = cJSON_IsNumber(v) && v->valuedouble >= need;
        cJSON_Delete(st);
        free(text);
    }
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    if (!ok) return econ_refuse(&x, "Access Denied");
    if (me.funds < price) return econ_refuse(&x, "Not enough KR");
    int64_t funds = 0;
    db_add_funds_ex(&x, me_id, -price, "blackmarket", P(4), 0, 0, &funds);
    db_add_skin(&x, me_id, skin, 1, "blackmarket", NULL, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "item", (double) skin);
    cJSON_AddNumberToObject(o, "kr", (double) price);
    cJSON_AddNumberToObject(o, "f", (double) funds);
    return econ_done(&x, n, p, "bm", "kn_blackm_buy", o);
}
const kn_proc P_BLACKM_BUY = {"blackm_buy", NULL, run_blackm_buy};

/* itemsales page: {ind, onS, inC, high, low, owners} */
static kn_db_result *run_market_item(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *o = dbx_row(&x,
        SQL(.any = "SELECT {int:$1} AS ind,"
                   " (SELECT count(*) FROM market_listings WHERE skin_id = $1 AND sold_at IS NULL AND cancelled_at IS NULL) AS on_s__i,"
                   " (SELECT coalesce(sum(count), 0) FROM inventory WHERE skin_id = $1) +"
                   " (SELECT count(*) FROM market_listings WHERE skin_id = $1 AND sold_at IS NULL AND cancelled_at IS NULL) AS in_c__i,"
                   " (SELECT max(price) FROM market_listings WHERE skin_id = $1 AND sold_at IS NOT NULL) AS high__i,"
                   " (SELECT min(price) FROM market_listings WHERE skin_id = $1 AND sold_at IS NULL AND cancelled_at IS NULL) AS low__i"),
        KN_DB_ARGS(P(0)));
    cJSON *owners = dbx_rows(&x,
        SQL(.any = "SELECT r.player_name AS nm, i.count AS ct, CASE WHEN r.player_premium > 0 THEN 1 ELSE 0 END AS su,"
                   " r.player_featured AS ft FROM inventory i JOIN player_rows r ON r.player_id = i.account_id"
                   " WHERE i.skin_id = $1 AND i.count > 0 ORDER BY i.count DESC LIMIT 100"),
        KN_DB_ARGS(P(0)));
    if (o) {
        /* v7's camelCase keys (PostgreSQL folds unquoted aliases to lower case) */
        cJSON *v = cJSON_DetachItemFromObjectCaseSensitive(o, "on_s");
        cJSON_AddItemToObject(o, "onS", v ? v : cJSON_CreateNumber(0));
        v = cJSON_DetachItemFromObjectCaseSensitive(o, "in_c");
        cJSON_AddItemToObject(o, "inC", v ? v : cJSON_CreateNumber(0));
        cJSON_AddItemToObject(o, "owners", owners ? owners : cJSON_CreateArray());
    } else {
        cJSON_Delete(owners);
    }
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_MARKET_ITEM = {"market_item", NULL, run_market_item};

/* ------------------------------------------------------------------ trades */

/* the painted copies of one side as the trade rows' units: {sid: copy id, i: skin, paint_color, paint_shader} */
static cJSON *trade_copy_rows(Dbx *x, int64_t trade, const char *side) {
    cJSON *a = dbx_rows(x, SQL(.any = "SELECT copy_id AS sid, skin_id AS i, paint_color, paint_shader FROM trade_copies"
                                     " WHERE trade_id = $1 AND side = $2 ORDER BY copy_id"),
                        KN_DB_ARGS(I64S(trade), side));
    return a ? a : cJSON_CreateArray();
}

/* Items of one side: "1,1,5," (skin id repeated count times, v7.1.4 sksell / skbuy). */
static char *trade_side(Dbx *x, int64_t trade, const char *side) {
    kn_db_result *r = dbx_q(x, SQL(.any = "SELECT skin_id, count FROM trade_items WHERE trade_id = $1 AND side = $2 ORDER BY skin_id"),
                            KN_DB_ARGS(I64S(trade), side));
    size_t cap = 64, len = 0;
    char *s = calloc(1, cap);
    for (int i = 0; s && r && i < kn_db_rows(r); i++) {
        const char *id = kn_db_get(r, i, 0);
        for (int64_t k = kn_db_get_int64(r, i, 1, 0); k > 0 && k < 1000; k--) {
            const size_t need = len + strlen(id) + 2;
            if (need > cap) {
                char *ns = realloc(s, cap = need * 2);
                if (!ns) break;
                s = ns;
            }
            len += (size_t) snprintf(s + len, cap - len, "%s,", id);
        }
    }
    kn_db_result_free(r);
    return s;
}

static cJSON *trade_rows(Dbx *x, const char *player, int open) {
    kn_db_result *r = dbx_q(x,
        open ? SQL(.any = "SELECT t.id, t.sender_id, t.message, t.status, (SELECT username FROM accounts WHERE id = t.sender_id) AS player,"
                          " (SELECT username FROM accounts WHERE id = t.receiver_id) AS buyer, {epoch_ms:t.created_at} AS dte,"
                          " t.sender_kr, t.receiver_kr, {epoch_ms:t.expires_at} AS exp FROM trades t"
                          " WHERE (t.sender_id = $1 OR t.receiver_id = $1) AND t.status = 'open' ORDER BY t.id DESC LIMIT 200")
             : SQL(.any = "SELECT t.id, t.sender_id, t.message, t.status, (SELECT username FROM accounts WHERE id = t.sender_id) AS player,"
                          " (SELECT username FROM accounts WHERE id = t.receiver_id) AS buyer, {epoch_ms:coalesce(t.closed_at, t.created_at)} AS dte,"
                          " t.sender_kr, t.receiver_kr, NULL AS exp FROM trades t"
                          " WHERE (t.sender_id = $1 OR t.receiver_id = $1) AND t.status <> 'open' ORDER BY t.id DESC LIMIT 500"),
        KN_DB_ARGS(player));
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; r && i < kn_db_rows(r); i++) {
        const int64_t id = kn_db_get_int64(r, i, 0, 0);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "trid", (double) id);
        cJSON_AddNumberToObject(o, "pl", (double) kn_db_get_int64(r, i, 1, 0));
        cJSON_AddItemToObject(o, "player", kn_db_isnull(r, i, 4) ? cJSON_CreateNull() : cJSON_CreateString(kn_db_get(r, i, 4)));
        cJSON_AddItemToObject(o, "buyer", kn_db_isnull(r, i, 5) ? cJSON_CreateNull() : cJSON_CreateString(kn_db_get(r, i, 5)));
        char *sell = trade_side(x, id, "s"), *buy = trade_side(x, id, "r");
        cJSON_AddStringToObject(o, "sksell", sell ? sell : "");
        cJSON_AddStringToObject(o, "skbuy", buy ? buy : "");
        free(sell);
        free(buy);
        /* the painted copies of each side (their paint is kept: doc/v10/paint-copy.md) */
        cJSON_AddItemToObject(o, "copies_s", trade_copy_rows(x, id, "s"));
        cJSON_AddItemToObject(o, "copies_r", trade_copy_rows(x, id, "r"));
        cJSON_AddItemToObject(o, "message", html_item(kn_db_get(r, i, 2)));
        cJSON_AddStringToObject(o, "status", kn_db_get(r, i, 3));
        cJSON_AddNumberToObject(o, "dte", (double) kn_db_get_int64(r, i, 6, 0));
        cJSON_AddNumberToObject(o, "krsell", (double) kn_db_get_int64(r, i, 7, 0)); /* KN: KR in the offer */
        cJSON_AddNumberToObject(o, "krbuy", (double) kn_db_get_int64(r, i, 8, 0));
        if (!kn_db_isnull(r, i, 9)) cJSON_AddNumberToObject(o, "exp", (double) kn_db_get_int64(r, i, 9, 0));
        cJSON_AddItemToArray(a, o);
    }
    kn_db_result_free(r);
    return a;
}

/* (player) -> [open offers, history] */
static kn_db_result *run_trade_page(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, trade_rows(&x, P(0), 1));
    cJSON_AddItemToArray(a, trade_rows(&x, P(0), 0));
    return dbx_json(&x, "jsonb_build_array", a);
}
const kn_proc P_TRADE_PAGE = {"trade_page", NULL, run_trade_page};

/* The trade picker's rows: the stacks (their unpainted units: {i, c}), then one {i, c: 1, sid: copy id, paint_color,
 * paint_shader} row per painted copy (doc/v10/paint-copy.md; the v10 reply turns the paint into m). */
static cJSON *inventory_items(Dbx *x, const char *id) {
    cJSON *a = dbx_rows(x,
        SQL(.any = "SELECT i.skin_id AS i, i.count - (SELECT count(*) FROM skin_copies c WHERE c.account_id = i.account_id"
                   " AND c.skin_id = i.skin_id) AS c FROM inventory i WHERE i.account_id = $1 AND i.count > (SELECT count(*)"
                   " FROM skin_copies c WHERE c.account_id = i.account_id AND c.skin_id = i.skin_id) ORDER BY i.skin_id"),
        KN_DB_ARGS(id));
    cJSON *k = dbx_rows(x,
        SQL(.any = "SELECT skin_id AS i, 1 AS c, copy_id AS sid, paint_color, paint_shader FROM skin_copies"
                   " WHERE account_id = $1 ORDER BY skin_id, copy_id"),
        KN_DB_ARGS(id));
    if (!a) a = cJSON_CreateArray();
    while (k && cJSON_GetArraySize(k) > 0) cJSON_AddItemToArray(a, cJSON_DetachItemFromArray(k, 0));
    cJSON_Delete(k);
    return a;
}

/* (me, them) -> [mine, theirs]; no row when them does not exist */
static kn_db_result *run_trade_inventories(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM accounts WHERE id = $1"), KN_DB_ARGS(P(1)))) {
        const char *names[1] = {"jsonb_build_array"};
        const kn_db_type types[1] = {KN_DB_T_JSON};
        return dbx_finish(&x, kn_db_result_new(1, names, types));
    }
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, inventory_items(&x, P(0)));
    cJSON_AddItemToArray(a, inventory_items(&x, P(1)));
    return dbx_json(&x, "jsonb_build_array", a);
}
const kn_proc P_TRADE_INVENTORIES = {"trade_inventories", NULL, run_trade_inventories};

/* One side of an offer: [[skin, count], ...] as economy.c normalized it (distinct skins), then the painted copies
 * [skin, 1, copy id] (copy != 0; one entry per copy, doc/v10/paint-copy.md). */
typedef struct Counted {
    int64_t skin, count, copy;
} Counted;

/* A painted copy of an offer: its copy id, skin and paint (the paint of a sender's copy is kept at escrow). */
typedef struct TradeCopy {
    int64_t copy, skin;
    CopyPaint paint;
} TradeCopy;

#define TRADE_SIDE_MAX 64

static int parse_side(const char *json, Counted *out) {
    cJSON *a = json ? cJSON_Parse(json) : NULL;
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, a) {
        if (k >= TRADE_SIDE_MAX) {
            k = -1;
            break;
        }
        const cJSON *s = cJSON_GetArrayItem(it, 0), *cnt = cJSON_GetArrayItem(it, 1), *cp = cJSON_GetArrayItem(it, 2);
        if (!cJSON_IsNumber(s) || !cJSON_IsNumber(cnt) || s->valuedouble < 0 || cnt->valuedouble < 1) {
            k = -1;
            break;
        }
        if (cp && (!cJSON_IsNumber(cp) || cnt->valuedouble != 1 || cp->valuedouble < V10_COPY_ID_BASE)) {
            k = -1;
            break;
        }
        out[k].skin = (int64_t) s->valuedouble;
        out[k].count = (int64_t) cnt->valuedouble;
        out[k].copy = cp ? (int64_t) cp->valuedouble : 0;
        /* a copy is offered once per side (trade_copies' key) */
        int dup = 0;
        for (int j = 0; out[k].copy && j < k; j++) dup |= out[j].copy == out[k].copy;
        if (dup) {
            k = -1;
            break;
        }
        k++;
    }
    cJSON_Delete(a);
    return k;
}

/* The units of one side: a copy is one unit of its own (its count is 1). */
static int64_t side_total(const Counted *c, int k) {
    int64_t t = 0;
    for (int i = 0; i < k; i++) t += c[i].count;
    return t;
}

/* Move one side's stacks: from (escrow when from == 0) to `to` (0 = into escrow). Copies are moved by the callers
 * (copy_take / copy_give), so a copy entry is skipped here. */
static void move_items(Dbx *x, const Counted *c, int k, int64_t from, int64_t to, const char *ref) {
    for (int i = 0; i < k && !dbx_failed(x); i++) {
        if (c[i].copy) continue;
        if (from > 0) db_add_skin(x, from, c[i].skin, -c[i].count, "trade", ref, NULL);
        if (to > 0) db_add_skin(x, to, c[i].skin, c[i].count, "trade", ref, NULL);
    }
}

static int load_side(Dbx *x, int64_t trade, const char *side, Counted *out) {
    kn_db_result *r = dbx_q(x, SQL(.any = "SELECT skin_id, count FROM trade_items WHERE trade_id = $1 AND side = $2 ORDER BY skin_id"),
                            KN_DB_ARGS(I64S(trade), side));
    int k = 0;
    for (int i = 0; r && i < kn_db_rows(r) && k < TRADE_SIDE_MAX; i++, k++) {
        out[k].skin = kn_db_get_int64(r, i, 0, 0);
        out[k].count = kn_db_get_int64(r, i, 1, 0);
        out[k].copy = 0;
    }
    kn_db_result_free(r);
    return k;
}

/* The painted copies of one side of an offer (trade_copies). */
static int load_copies(Dbx *x, int64_t trade, const char *side, TradeCopy *out) {
    kn_db_result *r = dbx_q(x, SQL(.any = "SELECT copy_id, skin_id, paint_color, paint_shader FROM trade_copies"
                                          " WHERE trade_id = $1 AND side = $2 ORDER BY copy_id"),
                            KN_DB_ARGS(I64S(trade), side));
    int k = 0;
    for (int i = 0; r && i < kn_db_rows(r) && k < TRADE_SIDE_MAX; i++, k++) {
        out[k].copy = kn_db_get_int64(r, i, 0, 0);
        out[k].skin = kn_db_get_int64(r, i, 1, 0);
        memset(&out[k].paint, 0, sizeof out[k].paint);
        out[k].paint.shader = -1;
        if (!kn_db_isnull(r, i, 2)) snprintf(out[k].paint.color, sizeof out[k].paint.color, "%s", kn_db_get(r, i, 2));
        if (!kn_db_isnull(r, i, 3)) out[k].paint.shader = kn_db_get_int64(r, i, 3, 0);
    }
    kn_db_result_free(r);
    return k;
}


/* A locked open trade row. */
typedef struct TradeRow {
    int64_t id, sender, receiver, skr, rkr;
    int escrow, expired;
    char status[16];
} TradeRow;

/* PostgreSQL: this transaction may close escrowed offers (0111's trades_escrow_guard trigger lets
 * only transactions that unwind or deliver the escrow do that). Transaction-local. */
static void econ_mark_escrow_tx(Dbx *x) {
    if (dbx_is_pg(x)) dbx_exec(x, SQL(.pg = "SELECT set_config('kn.economy', '1', true)"), 0, NULL);
}

/* Lock the trade row (taken before any players row). 1 found, 0 not found, -1 failure. */
static int lock_trade(Dbx *x, int64_t id, TradeRow *t) {
    econ_mark_escrow_tx(x);
    memset(t, 0, sizeof *t);
    kn_db_result *r = dbx_q(x, SQL(.any = "SELECT sender_id, receiver_id, status, sender_kr, receiver_kr, escrow,"
                                          " expires_at IS NOT NULL AND expires_at <= {now} AS expired__b"
                                          " FROM trades WHERE id = $1 {for_update}"),
                            KN_DB_ARGS(I64S(id)));
    if (!r) return -1;
    const int found = kn_db_rows(r) > 0;
    if (found) {
        t->id = id;
        t->sender = kn_db_get_int64(r, 0, 0, 0);
        t->receiver = kn_db_get_int64(r, 0, 1, 0);
        snprintf(t->status, sizeof t->status, "%s", kn_db_get(r, 0, 2));
        t->skr = kn_db_get_int64(r, 0, 3, 0);
        t->rkr = kn_db_get_int64(r, 0, 4, 0);
        t->escrow = kn_db_get_int64(r, 0, 5, 0) != 0;
        t->expired = kn_db_get_bool(r, 0, 6, 0);
    }
    kn_db_result_free(r);
    return found;
}

/* Close an open trade without completing it: the sender's escrow (items + KR) goes back. */
static void trade_unwind(Dbx *x, const TradeRow *t, const char *status) {
    if (t->escrow) {
        Counted give[TRADE_SIDE_MAX];
        const int k = load_side(x, t->id, "s", give);
        TradeCopy copies[TRADE_SIDE_MAX];
        const int kc = load_copies(x, t->id, "s", copies);
        char ref[48];
        snprintf(ref, sizeof ref, "return:%lld", (long long) t->id);
        move_items(x, give, k, 0, t->sender, ref);
        for (int i = 0; i < kc && !dbx_failed(x); i++) copy_give(x, t->sender, copies[i].skin, &copies[i].paint, "trade", ref, NULL);
        if (t->skr > 0) db_add_funds_ex(x, t->sender, t->skr, "trade-return", I64S(t->id), t->receiver, 0, NULL);
    }
    dbx_exec(x, SQL(.any = "UPDATE trades SET status = $2, closed_at = {now} WHERE id = $1"), KN_DB_ARGS(I64S(t->id), status));
}

/* $4 receiver id, $5 give [[skin, n]], $6 take [[skin, n]], $7 message, $8 sender KR, $9 receiver KR,
 * $10 fee (KR, economy.c: sum of store rarities[].fee; 0 for premium senders), $11 min level,
 * $12 max items per side, $13 same for premium senders, $14 TTL s (0 none), $15 max open offers,
 * $16 max KR per side (0: KR cannot be offered), $17 gift fee %, $18 gift min level, $19 gift min
 * -> {trid, receiver, funds, fee}. KR in an offer (KrunkNative) follows the gift rules: the giver
 * needs the gift level, no 'gift' ban, at least the gift minimum, and pays the gift fee (rounded
 * up) on it; the sender pays for skr now, the receiver for rkr when accepting.
 * The sender's items and KR leave the sender's account now (escrow); the fee is charged now and is
 * not refunded (v7.1.4: "Submit Offer - N KR"). */
static kn_db_result *run_trade_create(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("trade", "kn_trade_create");
    const int64_t sender = PI(0), receiver = PI(4), skr = PI(8), rkr = PI(9);
    Counted give[TRADE_SIDE_MAX], take[TRADE_SIDE_MAX];
    const int kg = parse_side(P(5), give), kt = parse_side(P(6), take);
    if (receiver <= 0) return econ_refuse(&x, "Player not found");
    if (receiver == sender) return econ_refuse(&x, "Cant trade with yourself");
    if (kg < 0 || kt < 0) return econ_refuse(&x, "Invalid offer");
    if (skr < 0 || rkr < 0 || skr > PI(16) || rkr > PI(16)) return econ_refuse(&x, PI(16) > 0 ? "Invalid amount" : "KR cannot be traded");
    if (kg + kt == 0 && skr == 0 && rkr == 0) return econ_refuse(&x, "No Items Selected");
    EconPlayer me, peer;
    if (econ_lock(&x, sender, &me, receiver, &peer) < 0 || econ_rate(&x, n, p, "trade") < 0) return dbx_finish(&x, NULL);
    if (me.notrade) return econ_refuse(&x, "Account is trade locked");
    if (!econ_peer_ok(&x, &peer)) return econ_refuse(&x, "Player can not trade");
    if (PI(11) > 0 && (level_of(&me) < PI(11) || level_of(&peer) < PI(11))) {
        refuse_level(&x, PI(11));
        return dbx_finish(&x, NULL);
    }
    const int64_t gift_pct = PI(17) > 0 ? PI(17) : 0, gift_lvl = PI(18), gift_min = PI(19) > 0 ? PI(19) : 1;
    if ((skr > 0 && skr < gift_min) || (rkr > 0 && rkr < gift_min)) {
        char msg[64];
        snprintf(msg, sizeof msg, "%lld KR minimum", (long long) gift_min);
        return econ_refuse(&x, msg);
    }
    if (gift_lvl > 0 && ((skr > 0 && level_of(&me) < gift_lvl) || (rkr > 0 && level_of(&peer) < gift_lvl))) {
        refuse_level(&x, gift_lvl);
        return dbx_finish(&x, NULL);
    }
    if (skr > 0 && econ_feature_banned(&x, sender, "gift")) return econ_refuse(&x, "You are banned from using the KR gifts");
    const int64_t max_items = me.premium ? PI(13) : PI(12);
    if (max_items > 0 && (side_total(give, kg) > max_items || side_total(take, kt) > max_items)) {
        char msg[64];
        snprintf(msg, sizeof msg, "%lld items per user", (long long) max_items);
        return econ_refuse(&x, msg);
    }
    if (PI(15) > 0 && dbx_int(&x, SQL(.any = "SELECT count(*) FROM trades WHERE sender_id = $1 AND status = 'open'"),
                              KN_DB_ARGS(P(0)), 0) >= PI(15))
        return econ_refuse(&x, "Too many open offers");
    /* the receiver's side is checked now (it is moved, and locked, only when they accept): a stack by its unpainted
     * units, a painted copy by its copy row */
    for (int i = 0; i < kt; i++) {
        if (take[i].copy) {
            if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM skin_copies WHERE copy_id = $1 AND account_id = $2 AND skin_id = $3"),
                            KN_DB_ARGS(I64S(take[i].copy), I64S(receiver), I64S(take[i].skin))))
                return econ_refuse(&x, "Player does not own these items");
        } else if (db_unpainted_units(&x, receiver, take[i].skin) < take[i].count) {
            return econ_refuse(&x, "Player does not own these items");
        }
    }
    const int64_t fee = (me.premium ? 0 : (PI(10) > 0 ? PI(10) : 0)) + (skr * gift_pct + 99) / 100;
    if (me.funds < fee + skr) return econ_refuse(&x, "Not enough KR");
    char *msg = utf8_left(P(7), 200);
    const int64_t ttl = PI(14) > 0 ? PI(14) : 0;
    const int64_t trade = dbx_insert_id(&x,
        SQL(.pg = "INSERT INTO trades (sender_id, receiver_id, message, sender_kr, receiver_kr, escrow, expires_at)"
                  " VALUES ($1, $2, $3, $4, $5, 1, CASE WHEN $6::bigint > 0 THEN now() + make_interval(secs => $6::bigint) END) RETURNING id",
            .sqlite = "INSERT INTO trades (sender_id, receiver_id, message, sender_kr, receiver_kr, escrow, expires_at)"
                      " VALUES ($1, $2, $3, $4, $5, 1, CASE WHEN $6 > 0 THEN {later:$6} END) RETURNING id",
            .mysql = "INSERT INTO trades (sender_id, receiver_id, message, sender_kr, receiver_kr, escrow, expires_at)"
                     " VALUES ($1, $2, $3, $4, $5, 1, CASE WHEN $6 > 0 THEN {later:$6} END)"),
        KN_DB_ARGS(P(0), P(4), msg ? msg : "", I64S(skr), I64S(rkr), I64S(ttl)));
    free(msg);
    for (int i = 0; i < kg && !dbx_failed(&x); i++) {
        if (give[i].copy) continue;
        dbx_exec(&x, SQL(.any = "INSERT INTO trade_items (trade_id, side, skin_id, count) VALUES ($1, 's', $2, $3)"),
                 KN_DB_ARGS(I64S(trade), I64S(give[i].skin), I64S(give[i].count)));
    }
    for (int i = 0; i < kt && !dbx_failed(&x); i++) {
        if (take[i].copy) continue;
        dbx_exec(&x, SQL(.any = "INSERT INTO trade_items (trade_id, side, skin_id, count) VALUES ($1, 'r', $2, $3)"),
                 KN_DB_ARGS(I64S(trade), I64S(take[i].skin), I64S(take[i].count)));
    }
    char ref[48];
    snprintf(ref, sizeof ref, "offer:%lld", (long long) trade);
    /* escrow: a missing item refuses the whole offer (db_add_skin: "Item not owned") */
    move_items(&x, give, kg, sender, 0, ref);
    /* a painted copy of the sender's leaves their copies into the offer: its paint is kept on the offer's row. The
     * receiver's painted copies stay theirs until the offer is accepted (their paint is read then). */
    for (int i = 0; i < kg && !dbx_failed(&x); i++) {
        if (!give[i].copy) continue;
        CopyPaint paint;
        if (copy_take(&x, sender, give[i].copy, give[i].skin, &paint, "trade", ref) < 0) break;
        const char *color, *shader;
        char shader_t[24];
        paint_cols(&paint, &color, &shader, shader_t);
        dbx_exec(&x, SQL(.any = "INSERT INTO trade_copies (trade_id, side, copy_id, skin_id, paint_color, paint_shader)"
                                " VALUES ($1, 's', $2, $3, $4, $5)"),
                 KN_DB_ARGS(I64S(trade), I64S(give[i].copy), I64S(give[i].skin), color, shader));
    }
    for (int i = 0; i < kt && !dbx_failed(&x); i++) {
        if (!take[i].copy) continue;
        kn_db_result *r = dbx_q(&x, SQL(.any = "SELECT paint_color, paint_shader FROM skin_copies WHERE copy_id = $1"
                                               " AND account_id = $2"),
                                KN_DB_ARGS(I64S(take[i].copy), I64S(receiver)));
        if (!r) break;
        if (kn_db_rows(r) < 1) {
            kn_db_result_free(r);
            return econ_refuse(&x, "Player does not own these items");
        }
        /* the paint is copied out before the result is freed (the copy's row may be gone: the trade then refuses at accept) */
        CopyPaint paint;
        memset(&paint, 0, sizeof paint);
        paint.shader = -1;
        if (kn_db_rows(r) > 0 && !kn_db_isnull(r, 0, 0)) snprintf(paint.color, sizeof paint.color, "%s", kn_db_get(r, 0, 0));
        if (kn_db_rows(r) > 0 && !kn_db_isnull(r, 0, 1)) paint.shader = kn_db_get_int64(r, 0, 1, 0);
        kn_db_result_free(r);
        const char *color, *shader;
        char shader_t[24];
        paint_cols(&paint, &color, &shader, shader_t);
        dbx_exec(&x, SQL(.any = "INSERT INTO trade_copies (trade_id, side, copy_id, skin_id, paint_color, paint_shader)"
                                " VALUES ($1, 'r', $2, $3, $4, $5)"),
                 KN_DB_ARGS(I64S(trade), I64S(take[i].copy), I64S(take[i].skin), color, shader));
    }
    int64_t funds = me.funds;
    if (skr > 0) db_add_funds_ex(&x, sender, -skr, "trade-escrow", I64S(trade), receiver, 0, &funds);
    if (fee > 0) db_add_funds_ex(&x, sender, -fee, "trade-fee", I64S(trade), receiver, fee, &funds);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "trid", (double) trade);
    cJSON_AddNumberToObject(o, "receiver", (double) receiver);
    cJSON_AddNumberToObject(o, "funds", (double) funds);
    cJSON_AddNumberToObject(o, "fee", (double) fee);
    return econ_done(&x, n, p, "trade", "kn_trade_create", o);
}
const kn_proc P_TRADE_CREATE = {"trade_create", NULL, run_trade_create};

/* $4 trade, $5 min level, $6 gift fee %, $7 gift min level -> {trid, sender, funds}. Only the
 * receiver accepts; both sides move in one transaction (the receiver's items and KR from their
 * inventory, the sender's from escrow). KR the receiver gives pays the gift fee (see create). */
static kn_db_result *run_trade_accept(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("accept", "kn_trade_accept");
    TradeRow t;
    const int found = lock_trade(&x, PI(4), &t);
    if (found < 0) return dbx_finish(&x, NULL);
    if (!found || t.receiver != PI(0)) return econ_refuse(&x, "Trade not found");
    if (strcmp(t.status, "open") != 0) return econ_refuse(&x, "Trade no longer available");
    if (t.expired) return econ_refuse(&x, "Trade expired");
    EconPlayer me, peer;
    if (econ_lock(&x, t.receiver, &me, t.sender, &peer) < 0 || econ_rate(&x, n, p, "accept") < 0) return dbx_finish(&x, NULL);
    if (me.notrade) return econ_refuse(&x, "Account is trade locked");
    if (!econ_peer_ok(&x, &peer)) return econ_refuse(&x, "Player can not trade");
    if (PI(5) > 0 && level_of(&me) < PI(5)) {
        refuse_level(&x, PI(5));
        return dbx_finish(&x, NULL);
    }
    const int64_t kr_fee = t.rkr > 0 ? (t.rkr * (PI(6) > 0 ? PI(6) : 0) + 99) / 100 : 0;
    if (t.rkr > 0 && PI(7) > 0 && level_of(&me) < PI(7)) {
        refuse_level(&x, PI(7));
        return dbx_finish(&x, NULL);
    }
    if (t.rkr > 0 && econ_feature_banned(&x, t.receiver, "gift")) return econ_refuse(&x, "You are banned from using the KR gifts");
    if (me.funds < t.rkr + kr_fee) return econ_refuse(&x, "Not enough KR");
    Counted give[TRADE_SIDE_MAX], take[TRADE_SIDE_MAX];
    const int kg = load_side(&x, t.id, "s", give), kt = load_side(&x, t.id, "r", take);
    TradeCopy give_c[TRADE_SIDE_MAX], take_c[TRADE_SIDE_MAX];
    const int kgc = load_copies(&x, t.id, "s", give_c), ktc = load_copies(&x, t.id, "r", take_c);
    char ref[48];
    snprintf(ref, sizeof ref, "trade:%lld", (long long) t.id);
    /* receiver -> sender ("Item not owned" when they no longer have it: nothing moves) */
    move_items(&x, take, kt, t.receiver, t.sender, ref);
    /* a painted copy of the receiver's goes to the sender with its paint (the copy row is read now) */
    for (int i = 0; i < ktc && !dbx_failed(&x); i++) {
        CopyPaint paint;
        if (copy_take(&x, t.receiver, take_c[i].copy, take_c[i].skin, &paint, "trade", ref) < 0) break;
        copy_give(&x, t.sender, take_c[i].skin, &paint, "trade", ref, NULL);
    }
    /* sender -> receiver: from escrow, or from the sender's inventory for a pre-0111 offer */
    move_items(&x, give, kg, t.escrow ? 0 : t.sender, t.receiver, ref);
    /* the sender's painted copies, out of escrow, to the receiver with their paint */
    for (int i = 0; i < kgc && !dbx_failed(&x); i++) copy_give(&x, t.receiver, give_c[i].skin, &give_c[i].paint, "trade", ref, NULL);
    int64_t funds = me.funds;
    if (t.rkr > 0) {
        db_add_funds_ex(&x, t.receiver, -t.rkr, "trade-out", I64S(t.id), t.sender, 0, &funds);
        db_add_funds_ex(&x, t.sender, t.rkr, "trade-in", I64S(t.id), t.receiver, 0, NULL);
        if (kr_fee > 0) db_add_funds_ex(&x, t.receiver, -kr_fee, "trade-fee", I64S(t.id), t.sender, kr_fee, &funds);
    }
    if (t.skr > 0) {
        if (!t.escrow) db_add_funds_ex(&x, t.sender, -t.skr, "trade-out", I64S(t.id), t.receiver, 0, NULL);
        db_add_funds_ex(&x, t.receiver, t.skr, "trade-in", I64S(t.id), t.sender, 0, &funds);
    }
    dbx_exec(&x, SQL(.any = "UPDATE trades SET status = 'accepted', closed_at = {now} WHERE id = $1"), KN_DB_ARGS(P(4)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "trid", (double) t.id);
    cJSON_AddNumberToObject(o, "sender", (double) t.sender);
    cJSON_AddNumberToObject(o, "funds", (double) funds);
    return econ_done(&x, n, p, "accept", "kn_trade_accept", o);
}
const kn_proc P_TRADE_ACCEPT = {"trade_accept", NULL, run_trade_accept};

/* $4 trade -> {trid, peer, declined, funds}: the receiver declines or the sender removes the offer. */
static kn_db_result *run_trade_close(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("close", "kn_trade_close");
    TradeRow t;
    const int found = lock_trade(&x, PI(4), &t);
    if (found < 0) return dbx_finish(&x, NULL);
    if (!found || (t.receiver != PI(0) && t.sender != PI(0))) return econ_refuse(&x, "Trade not found");
    if (strcmp(t.status, "open") != 0) return econ_refuse(&x, "Trade no longer available");
    const int declined = t.receiver == PI(0);
    EconPlayer a, b;
    if (econ_lock(&x, t.sender, &a, t.receiver, &b) < 0 || econ_rate(&x, n, p, "close") < 0) return dbx_finish(&x, NULL);
    trade_unwind(&x, &t, t.expired ? "expired" : declined ? "declined" : "cancelled");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "trid", (double) t.id);
    cJSON_AddNumberToObject(o, "peer", (double) (declined ? t.sender : t.receiver));
    cJSON_AddNumberToObject(o, "declined", declined);
    cJSON_AddNumberToObject(o, "funds", (double) dbx_int(&x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = $1"),
                                                         KN_DB_ARGS(P(0)), 0));
    return econ_done(&x, n, p, "close", "kn_trade_close", o);
}
const kn_proc P_TRADE_CLOSE = {"trade_close", NULL, run_trade_close};

/* Unwind every open trade selected by q (SELECT id, sender_id, receiver_id ... ORDER BY id
 * {for_update}) with `status`. Lock order as everywhere: all the trade rows first (q), then every
 * player involved in ascending id, then the inventories. Returns [{trid, sender, receiver}]. */
static int cmp_i64(const void *a, const void *b) {
    const int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
    return x < y ? -1 : x > y;
}

static cJSON *unwind_all(Dbx *x, const kn_sql *q, int n, const char *const *p, const char *status, cJSON *failed) {
    kn_db_result *ids = dbx_q(x, q, n, p);
    cJSON *a = cJSON_CreateArray();
    const int rows = ids ? kn_db_rows(ids) : 0;
    int64_t *players = rows ? calloc((size_t) rows * 2, sizeof *players) : NULL;
    int np = 0;
    for (int i = 0; players && i < rows; i++) {
        players[np++] = kn_db_get_int64(ids, i, 1, 0);
        players[np++] = kn_db_get_int64(ids, i, 2, 0);
    }
    if (np) qsort(players, (size_t) np, sizeof *players, cmp_i64);
    for (int i = 0; i < np && !dbx_failed(x); i++) {
        EconPlayer pl;
        if (i == 0 || players[i] != players[i - 1]) econ_player(x, players[i], &pl);
    }
    free(players);
    for (int i = 0; i < rows && !dbx_failed(x); i++) {
        /* each offer in its own savepoint: one that cannot be unwound is skipped (and logged by the
         * caller through "failed") instead of failing the whole batch every time */
        dbx_exec(x, SQL(.any = "SAVEPOINT econ_unwind"), 0, NULL);
        TradeRow t;
        const int found = lock_trade(x, kn_db_get_int64(ids, i, 0, 0), &t);
        if (found > 0 && !strcmp(t.status, "open")) trade_unwind(x, &t, t.expired ? "expired" : status);
        if (dbx_failed(x)) {
            kn_db_result_free(x->err);
            x->err = NULL;
            dbx_exec(x, SQL(.any = "ROLLBACK TO SAVEPOINT econ_unwind"), 0, NULL);
            if (failed) cJSON_AddItemToArray(failed, cJSON_CreateNumber((double) kn_db_get_int64(ids, i, 0, 0)));
            continue;
        }
        dbx_exec(x, SQL(.any = "RELEASE SAVEPOINT econ_unwind"), 0, NULL);
        if (found <= 0 || strcmp(t.status, "open") != 0) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "trid", (double) t.id);
        cJSON_AddNumberToObject(o, "sender", (double) t.sender);
        cJSON_AddNumberToObject(o, "receiver", (double) t.receiver);
        cJSON_AddItemToArray(a, o);
    }
    kn_db_result_free(ids);
    return a;
}

/* (actor, key, rate, feature) -> {closed: [{trid, sender, receiver}]}: decline every open incoming offer */
static kn_db_result *run_trade_clear(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    ECON_OPEN("clear", "kn_trade_clear");
    cJSON *failed = cJSON_CreateArray();
    cJSON *closed = unwind_all(&x, SQL(.any = "SELECT id, sender_id, receiver_id FROM trades WHERE receiver_id = $1 AND status = 'open'"
                                              " ORDER BY id LIMIT 500 {for_update}"),
                               KN_DB_ARGS(P(0)), "declined", failed);
    econ_rate(&x, n, p, "clear");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "closed", closed);
    cJSON_AddItemToObject(o, "failed", failed);
    return econ_done(&x, n, p, "clear", "kn_trade_clear", o);
}
const kn_proc P_TRADE_CLEAR = {"trade_clear", NULL, run_trade_clear};

/* () -> {closed: [...]}: expire the open offers past their TTL (the brain's economy timer), and
 * forget idempotency keys older than 7 days. One batch per run. */
static kn_db_result *run_trade_expire(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    (void) n;
    (void) p;
    cJSON *failed = cJSON_CreateArray();
    cJSON *closed = unwind_all(&x, SQL(.any = "SELECT id, sender_id, receiver_id FROM trades WHERE status = 'open' AND expires_at IS NOT NULL"
                                              " AND expires_at <= {now} ORDER BY id LIMIT 200 {for_update}"),
                               0, NULL, "expired", failed);
    dbx_exec(&x, SQL(.any = "DELETE FROM economy_requests WHERE at < {ago:604800}"), 0, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "closed", closed);
    cJSON_AddItemToObject(o, "failed", failed);
    return dbx_json(&x, "kn_trade_expire", o);
}
const kn_proc P_TRADE_EXPIRE = {"trade_expire", NULL, run_trade_expire};

/* ------------------------------------------------------------------ KR history + listings */

/* (player, days, limit) -> [{id, amount, source, ref, cp, fee, tim}] newest first (KN: the
 * account's own KR ledger, "krh"; v7.1.4 shows no ledger to players) */
static kn_db_result *run_kr_history(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *rows = dbx_rows(&x,
        SQL(.any = "SELECT t.id AS id, t.amount AS amount, t.source AS source, t.ref AS ref, a.username AS cp,"
                   " t.fee AS fee, {epoch_ms:t.at} AS tim FROM kr_transactions t LEFT JOIN accounts a ON a.id = t.counterparty_id"
                   " WHERE t.account_id = $1 AND t.at > {ago:$2 * 86400} ORDER BY t.at DESC, t.id DESC LIMIT $3"),
        KN_DB_ARGS(P(0), P(1), P(2)));
    return dbx_json(&x, "kr_history", rows ? rows : cJSON_CreateArray());
}
const kn_proc P_KR_HISTORY = {"kr_history", NULL, run_kr_history};

#define LISTING_COLS                                                                                       \
    "SELECT l.skin_id AS i, l.skin_id AS s, l.skin_id AS sk, l.id AS m, l.id AS id, l.price AS f, l.price AS kr," \
    " {epoch_ms:l.created_at} AS ms, {epoch_ms:l.created_at} AS ss, a.username AS p, 1 AS c,"                \
    " l.paint_color AS paint_color, l.paint_shader AS paint_shader"                                          \
    " FROM market_listings l JOIN accounts a ON a.id = l.seller_id "

/* (player) -> the player's open listings (v7.1.4 "uml" -> "ul": {sk, skid, kr, c, ms, id}) */
static kn_db_result *run_listings_of(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *rows = dbx_rows(&x, SQL(.any = LISTING_COLS "WHERE l.seller_id = $1 AND l.sold_at IS NULL AND l.cancelled_at IS NULL"
                                                     " ORDER BY l.id DESC LIMIT 100"),
                           KN_DB_ARGS(P(0)));
    cJSON *it;
    cJSON_ArrayForEach(it, rows) cJSON_AddItemToObject(it, "skid", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(it, "sk"), 1));
    return dbx_json(&x, "listings", rows ? rows : cJSON_CreateArray());
}
const kn_proc P_LISTINGS_OF = {"listings_of", NULL, run_listings_of};

/* (listing id) -> the open listing row or null (v7.1.4 page "listing") */
static kn_db_result *run_listing(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = LISTING_COLS "WHERE l.id = $1 AND l.sold_at IS NULL AND l.cancelled_at IS NULL"),
                         KN_DB_ARGS(P(0)));
    return dbx_json(&x, "listing", row);
}
const kn_proc P_LISTING = {"listing", NULL, run_listing};
