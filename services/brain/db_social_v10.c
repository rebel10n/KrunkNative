/*
 * services/brain/db_social_v10.c - Krunker v10.0.2 Social hub writes as kn_procs (db_procs.h),
 * C on every driver (.pg NULL): creating a post (v10 POST /social/post, incl. KR giveaways) and
 * settling ended giveaways (0112_social_v10).
 *
 * Giveaways (post type 9): v10's client sends {type: 9, text, funds} and shows "A 10% fee applies.
 * You will be charged <amount + ceil(10%)> KR total", "Minimum 10 KR", errors SOCIAL_GIVEAWAY_TOO_SMALL
 * / SOCIAL_GIVEAWAY_LEVEL_TOO_LOW (level 20) / SOCIAL_INSUFFICIENT_FUNDS; entering is "Follow and
 * Like to Enter". The draw itself is server side and not in the bundle: KrunkNative draws one
 * entrant (liked the post and follows the author, not the author) uniformly at random when the
 * giveaway ends (accounts.giveaway_hours, default 24) and pays the prize; with no entrant the prize
 * goes back to the author (the fee does not).
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* (me, type, text, funds, post_max_len, posts_per_hour, giveaway_hours) -> {post_id, player_funds}.
 * Refusals are v10 error codes (the client maps them to errors.* texts). */
static kn_db_result *run_post_create(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t me = PI(0), type = PI(1) == 9 ? 9 : 0, max_len = PI(4) > 0 ? PI(4) : 1024;
    const char *text = P(2) ? P(2) : "";
    if (!*text) return dbx_finish(&x, kn_db_result_user_error("SOCIAL_POST_TEXT_REQUIRED"));
    const int64_t score = dbx_int(&x, SQL(.any = "SELECT player_score FROM players WHERE player_id = $1 {for_update}"),
                                  KN_DB_ARGS(I64S(me)), -1);
    if (score < 0) return dbx_finish(&x, kn_db_result_user_error("SOCIAL_CREATE_FAILED"));
    const int level = db_level(score, 0.03);
    if (level < 10) return dbx_finish(&x, kn_db_result_user_error("SOCIAL_LEVEL_TOO_LOW"));
    const int64_t recent = dbx_int(&x, SQL(.pg = "SELECT count(*) FROM feed_posts WHERE author_id = $1 AND created_at > now() - interval '1 hour'",
                                           .any = "SELECT count(*) FROM feed_posts WHERE author_id = $1 AND created_at > {ago:3600}"),
                                   KN_DB_ARGS(I64S(me)), 0);
    if (PI(5) > 0 && recent >= PI(5)) return dbx_finish(&x, kn_db_result_user_error("SOCIAL_RATE_LIMITED"));
    int64_t funds = dbx_int(&x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(me)), 0);
    int64_t prize = 0;
    if (type == 9) {
        prize = PI(3);
        if (level < 20) return dbx_finish(&x, kn_db_result_user_error("SOCIAL_GIVEAWAY_LEVEL_TOO_LOW"));
        if (prize < 10 || prize > 10000000) return dbx_finish(&x, kn_db_result_user_error("SOCIAL_GIVEAWAY_TOO_SMALL"));
        const int64_t fee = (prize + 9) / 10; /* ceil(0.1 * amount) */
        if (funds < prize + fee) return dbx_finish(&x, kn_db_result_user_error("SOCIAL_INSUFFICIENT_FUNDS"));
        if (db_add_funds_ex(&x, me, -(prize + fee), "giveaway", "post", 0, fee, &funds) < 0) return dbx_finish(&x, NULL);
    }
    /* the body is cut at max_len bytes on a UTF-8 boundary */
    size_t len = strlen(text);
    if ((int64_t) len > max_len) {
        len = (size_t) max_len;
        while (len > 0 && ((unsigned char) text[len] & 0xC0) == 0x80) len--;
    }
    char *body = malloc(len + 1);
    if (!body) return dbx_finish(&x, kn_db_result_user_error("SOCIAL_CREATE_FAILED"));
    memcpy(body, text, len);
    body[len] = 0;
    const int64_t hours = PI(6) > 0 ? PI(6) : 24;
    int64_t id;
    if (type == 9)
        id = dbx_insert_id(&x, SQL(.pg = "INSERT INTO feed_posts (author_id, body, type, funds, ends_at) VALUES ($1, $2, 9, $3::bigint, "
                                         "now() + make_interval(hours => $4::int)) RETURNING id",
                                   .sqlite = "INSERT INTO feed_posts (author_id, body, type, funds, ends_at) VALUES ($1, $2, 9, $3, "
                                             "strftime('%Y-%m-%d %H:%M:%f', 'now', '+' || $4 || ' hours')) RETURNING id",
                                   .mysql = "INSERT INTO feed_posts (author_id, body, type, funds, ends_at) VALUES ($1, $2, 9, $3, "
                                            "DATE_ADD(CURRENT_TIMESTAMP(3), INTERVAL $4 HOUR))"),
                           KN_DB_ARGS(I64S(me), body, I64S(prize), I64S(hours)));
    else
        id = dbx_insert_id(&x, SQL(.pg = "INSERT INTO feed_posts (author_id, body, type) VALUES ($1, $2, 0) RETURNING id",
                                   .sqlite = "INSERT INTO feed_posts (author_id, body, type) VALUES ($1, $2, 0) RETURNING id",
                                   .mysql = "INSERT INTO feed_posts (author_id, body, type) VALUES ($1, $2, 0)"),
                           KN_DB_ARGS(I64S(me), body));
    free(body);
    if (type == 9 && id > 0)
        dbx_exec(&x, SQL(.any = "UPDATE kr_transactions SET ref = $2 WHERE id = (SELECT max(id) FROM kr_transactions "
                               "WHERE account_id = $1 AND source = 'giveaway' AND ref = 'post')"),
                 KN_DB_ARGS(I64S(me), I64S(id)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "post_id", (double) id);
    cJSON_AddNumberToObject(o, "player_funds", (double) funds);
    return dbx_json(&x, "post_create", o);
}
const kn_proc P_POST_CREATE_V10 = {"post_create_v10", NULL, run_post_create};

/* () -> {post_id, author, winner, prize} of one ended giveaway settled now, or SQL NULL when none */
static kn_db_result *run_giveaway_settle(kn_dbconn *c, int n, const char *const *p) {
    (void) n;
    (void) p;
    Dbx x = {c, NULL};
    cJSON *g = dbx_row(&x, SQL(.any = "SELECT id, author_id, funds FROM feed_posts WHERE type = 9 AND settled_at IS NULL "
                                      "AND ends_at <= {now} ORDER BY ends_at LIMIT 1 {for_update}"),
                       0, NULL);
    if (!g) return dbx_json(&x, "giveaway_settle", NULL);
    const int64_t post = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItem(g, "id"));
    const int64_t author = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItem(g, "author_id"));
    const int64_t prize = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItem(g, "funds"));
    cJSON_Delete(g);
    /* entrants: liked the post and follow the author; one at random */
    kn_db_result *e = dbx_q(&x, SQL(.any = "SELECT l.account_id FROM feed_likes l JOIN follows f ON f.follower_id = l.account_id "
                                           "AND f.followee_id = $2 WHERE l.post_id = $1 AND l.account_id <> $2"),
                            KN_DB_ARGS(I64S(post), I64S(author)));
    int64_t winner = 0;
    if (e && kn_db_rows(e) > 0) {
        unsigned r = 0;
        FILE *u = fopen("/dev/urandom", "rb");
        if (!u || fread(&r, sizeof r, 1, u) != 1) r = (unsigned) rand();
        if (u) fclose(u);
        winner = kn_db_get_int64(e, (int) (r % (unsigned) kn_db_rows(e)), 0, 0);
    }
    kn_db_result_free(e);
    if (prize > 0) db_add_funds(&x, winner ? winner : author, prize, winner ? "giveaway-win" : "giveaway-refund", I64S(post), NULL);
    if (winner)
        dbx_exec(&x, SQL(.any = "INSERT INTO mail (account_id, sender, subject, body, kr) SELECT $1, a.username, 'Giveaway Won', "
                               "$3, 0 FROM accounts a WHERE a.id = $2"),
                 KN_DB_ARGS(I64S(winner), I64S(author), "You won a KR giveaway!"));
    dbx_exec(&x, SQL(.any = "UPDATE feed_posts SET settled_at = {now}, winner_id = NULLIF($2, 0) WHERE id = $1"),
             KN_DB_ARGS(I64S(post), I64S(winner)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "post_id", (double) post);
    cJSON_AddNumberToObject(o, "author", (double) author);
    cJSON_AddNumberToObject(o, "winner", (double) winner);
    cJSON_AddNumberToObject(o, "prize", (double) prize);
    return dbx_json(&x, "giveaway_settle", o);
}
const kn_proc P_GIVEAWAY_SETTLE = {"giveaway_settle", NULL, run_giveaway_settle};
