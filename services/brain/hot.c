/*
 * services/brain/hot.c - optional Redis / Valkey hot tier of kn_brain (hot.h,
 * doc/backend.md 3.11). Built with KN_HAVE_REDIS (hiredis found, knsvc_redis);
 * without it every entry point is a stub that reports "not available".
 *
 * Nothing here is authoritative: Redis restarting empty costs cache misses (the
 * database answers), presence that is rebuilt by the next refresh, rate-limit
 * windows that start over, and pub/sub messages sent while it was down (the
 * federation's own polls cover those). Multi-key operations are Lua scripts
 * (EVAL: standalone or replicated Redis / Valkey >= 6.2, not Redis Cluster).
 */
#include "hot.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/buf.h"
#include "knsvc/config.h"
#include "knsvc/log.h"

#define MOD "hot"

#ifndef KN_HAVE_REDIS

int hot_init(Brain *b) {
    const char *url = kn_config_str(b->cfg, "redis.url", "");
    if (url && *url)
        KN_WARN(MOD, "redis.url is set but this kn_brain was built without hiredis: database only");
    return 0;
}
void hot_shutdown(Brain *b) { (void) b; }
const char *hot_state(void) { return "off"; }
int hot_up(void) { return 0; }
int hot_limit(const char *bucket, const char *key, int limit, int window_s, int local_ok) {
    (void) bucket; (void) key; (void) limit; (void) window_s;
    return local_ok;
}
int hot_session_lookup(const char *token, HotSessionFn cb, void *ud) { (void) token; (void) cb; (void) ud; return -1; }
void hot_session_store(const char *token, int64_t account_id, uint64_t t0_us) { (void) token; (void) account_id; (void) t0_us; }
void hot_session_forget(const char *token) { (void) token; }
void hot_session_forget_account(int64_t account_id) { (void) account_id; }
int hot_get(const char *key, HotGetFn cb, void *ud) { (void) key; (void) cb; (void) ud; return -1; }
void hot_put(const char *key, const char *value, int ttl_ms) { (void) key; (void) value; (void) ttl_ms; }
void hot_presence_source(HotPresenceSource fn) { (void) fn; }
void hot_presence_add(int64_t account_id) { (void) account_id; }
void hot_presence_remove(int64_t account_id) { (void) account_id; }
int hot_presence_count(HotCountFn cb, void *ud) { (void) cb; (void) ud; return -1; }
int hot_presence_online(const int64_t *ids, size_t n, HotOnlineFn cb, void *ud) { (void) ids; (void) n; (void) cb; (void) ud; return -1; }
void hot_push_handler(HotPushHandler fn) { (void) fn; }
void hot_push(int64_t account_id, const cJSON *frame, int json_only) { (void) account_id; (void) frame; (void) json_only; }

#else /* KN_HAVE_REDIS */

#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"
#include "knsvc/redis.h"

#define BLOCK_SLOTS 1024
#define BLOCK_PROBE 8
#define PRESENCE_BATCH 256

typedef struct Block {
    char key[112];
    uint64_t until_ms;
} Block;

typedef struct HotSub {
    char channel[64];
    kn_db_notify_cb cb;
    void *ud;
} HotSub;

static struct {
    Brain *b;
    kn_redis *r;
    char prefix[64];
    int pubsub;               /* redis.pubsub: Redis carries the kn_db_notify channels */
    int session_cache_s;      /* 0 = off */
    int limits;               /* redis.rate_limits */
    int presence;             /* redis.presence */
    int presence_ttl_s;
    kn_timer_id presence_timer;
    HotPresenceSource presence_src;
    HotPushHandler push_fn;
    Block blocks[BLOCK_SLOTS];
    HotSub **subs;
    size_t nsubs;
} g_hot;

/* ------------------------------------------------------------------ scripts */

static const char LUA_GET_TTL[] = "return {redis.call('GET', KEYS[1]), redis.call('PTTL', KEYS[1])}";

/* -> {account id or nil, redis time in us} */
static const char LUA_SESS_LOOKUP[] =
    "local t = redis.call('TIME')\n"
    "return {redis.call('GET', KEYS[1]), t[1] .. string.format('%06d', tonumber(t[2]))}";

/* KEYS sess, sessidx, sessrev, sessdead; ARGV id, ttl_s, hash, t0_us: refuse when revoked since t0 */
static const char LUA_SESS_STORE[] =
    "local t0 = tonumber(ARGV[4])\n"
    "for i = 3, 4 do\n"
    "  local r = redis.call('GET', KEYS[i])\n"
    "  if r and tonumber(r) >= t0 then return 0 end\n"
    "end\n"
    "redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2])\n"
    "redis.call('SADD', KEYS[2], ARGV[3])\n"
    "redis.call('EXPIRE', KEYS[2], tonumber(ARGV[2]) * 2)\n"
    "return 1";

/* KEYS sess, sessdead; ARGV tombstone ttl_s */
static const char LUA_SESS_FORGET[] =
    "local t = redis.call('TIME')\n"
    "redis.call('SET', KEYS[2], t[1] .. string.format('%06d', tonumber(t[2])), 'EX', ARGV[1])\n"
    "return redis.call('DEL', KEYS[1])";

/* KEYS sessidx, sessrev; ARGV "<prefix>sess:", tombstone ttl_s */
static const char LUA_SESS_FORGET_ACCOUNT[] =
    "local t = redis.call('TIME')\n"
    "redis.call('SET', KEYS[2], t[1] .. string.format('%06d', tonumber(t[2])), 'EX', ARGV[2])\n"
    "local m = redis.call('SMEMBERS', KEYS[1])\n"
    "for i = 1, #m do redis.call('DEL', ARGV[1] .. m[i]) end\n"
    "redis.call('DEL', KEYS[1])\n"
    "return #m";

/* KEYS counter; ARGV window_ms -> {hits, pttl} */
static const char LUA_RATE[] =
    "local n = redis.call('INCR', KEYS[1])\n"
    "local t = redis.call('PTTL', KEYS[1])\n"
    "if n == 1 or t < 0 then redis.call('PEXPIRE', KEYS[1], ARGV[1]) t = tonumber(ARGV[1]) end\n"
    "return {n, t}";

/* KEYS online; ARGV brain, expiry_ms, ttl_ms, "<prefix>pres:", ids... */
static const char LUA_PRES_ADD[] =
    "for i = 5, #ARGV do\n"
    "  local k = ARGV[4] .. ARGV[i]\n"
    "  redis.call('HSET', k, ARGV[1], ARGV[2])\n"
    "  redis.call('PEXPIRE', k, ARGV[3])\n"
    "  redis.call('ZADD', KEYS[1], 'GT', ARGV[2], ARGV[i])\n"
    "end\n"
    "return #ARGV - 4";

/* KEYS online, pres:<id>; ARGV brain, id, now_ms */
static const char LUA_PRES_REMOVE[] =
    "redis.call('HDEL', KEYS[2], ARGV[1])\n"
    "local h = redis.call('HGETALL', KEYS[2])\n"
    "local best = 0\n"
    "for i = 1, #h, 2 do\n"
    "  local e = tonumber(h[i + 1])\n"
    "  if e > tonumber(ARGV[3]) and e > best then best = e end\n"
    "end\n"
    "if best == 0 then redis.call('ZREM', KEYS[1], ARGV[2]) redis.call('DEL', KEYS[2])\n"
    "else redis.call('ZADD', KEYS[1], best, ARGV[2]) end\n"
    "return best";

/* KEYS online; ARGV now_ms */
static const char LUA_PRES_COUNT[] =
    "redis.call('ZREMRANGEBYSCORE', KEYS[1], '-inf', '(' .. ARGV[1])\n"
    "return redis.call('ZCARD', KEYS[1])";

/* KEYS online; ARGV now_ms, id...: 1 per id whose presence has not expired */
static const char LUA_PRES_ONLINE[] =
    "local now = tonumber(ARGV[1])\n"
    "local out = {}\n"
    "for i = 2, #ARGV do\n"
    "  local s = redis.call('ZSCORE', KEYS[1], ARGV[i])\n"
    "  out[#out + 1] = (s and tonumber(s) > now) and 1 or 0\n"
    "end\n"
    "return out";

/* KEYS pres:<id>; ARGV self brain, now_ms, "<prefix>push:", payload: publish to the other brains */
static const char LUA_PUSH[] =
    "local h = redis.call('HGETALL', KEYS[1])\n"
    "local n = 0\n"
    "for i = 1, #h, 2 do\n"
    "  if h[i] ~= ARGV[1] and tonumber(h[i + 1]) > tonumber(ARGV[2]) then\n"
    "    redis.call('PUBLISH', ARGV[3] .. h[i], ARGV[4]) n = n + 1\n"
    "  end\n"
    "end\n"
    "return n";

/* ------------------------------------------------------------------ helpers */

static void key(char *out, size_t cap, const char *fmt, ...) KN_PRINTF(3, 4);
static void key(char *out, size_t cap, const char *fmt, ...) {
    int n = snprintf(out, cap, "%s", g_hot.prefix);
    if (n < 0 || (size_t) n >= cap) n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, cap - (size_t) n, fmt, ap);
    va_end(ap);
}

static void token_hash(const char *token, char out[65]) {
    unsigned char d[32];
    kn_sha256(token, strlen(token), d);
    kn_hex_encode(d, 32, out, 65);
}

static void ignore_reply(kn_redis *r, const kn_redis_reply *rp, const char *err, void *ud) {
    (void) r;
    (void) rp;
    if (err) KN_DEBUG(MOD, "%s: %s", ud ? (const char *) ud : "command", err);
}

static int eval(const char *script, kn_redis_cb cb, void *ud, int nkeys, int argc, const char *const *args) {
    const char *argv[PRESENCE_BATCH + 16];
    char nk[8];
    if (argc + 3 > (int) (sizeof argv / sizeof argv[0])) return -1;
    snprintf(nk, sizeof nk, "%d", nkeys);
    argv[0] = "EVAL";
    argv[1] = script;
    argv[2] = nk;
    for (int i = 0; i < argc; i++) argv[3 + i] = args[i] ? args[i] : "";
    return kn_redis_cmd_argv(g_hot.r, cb, ud, argc + 3, argv, NULL);
}

const char *hot_state(void) { return !g_hot.r ? "off" : kn_redis_up(g_hot.r) ? "up" : "down"; }
int hot_up(void) { return g_hot.r && kn_redis_up(g_hot.r); }

/* ------------------------------------------------------------------ pub/sub transport (kn_db_set_pubsub) */

static int ps_publish(void *ctx, const char *channel, const char *payload) {
    (void) ctx;
    if (!hot_up() || !kn_redis_sub_up(g_hot.r)) return -1; /* our own subscriber must be live too */
    char ch[160];
    key(ch, sizeof ch, "%s", channel);
    return kn_redis_publish(g_hot.r, ch, payload, strlen(payload));
}

static void ps_message(kn_redis *r, const char *channel, const char *payload, size_t len, void *ud) {
    (void) r;
    (void) channel;
    (void) len;
    HotSub *s = ud;
    s->cb(s->channel, payload, s->ud);
}

static int ps_subscribe(void *ctx, const char *channel, kn_db_notify_cb cb, void *ud) {
    (void) ctx;
    HotSub *s = calloc(1, sizeof *s);
    HotSub **grown = realloc(g_hot.subs, (g_hot.nsubs + 1) * sizeof *grown);
    if (!s || !grown) {
        free(s);
        if (grown) g_hot.subs = grown;
        return -1;
    }
    g_hot.subs = grown;
    g_hot.subs[g_hot.nsubs++] = s;
    snprintf(s->channel, sizeof s->channel, "%s", channel);
    s->cb = cb;
    s->ud = ud;
    char ch[160];
    key(ch, sizeof ch, "%s", channel);
    return kn_redis_subscribe(g_hot.r, ch, ps_message, s);
}

static const kn_db_pubsub_ops k_ps_ops = {ps_publish, ps_subscribe};

/* ------------------------------------------------------------------ rate limits */

static Block *block_slot(const char *k, int create) {
    uint32_t h = 2166136261u;
    for (const char *p = k; *p; p++) h = (h ^ (unsigned char) *p) * 16777619u;
    const uint64_t now = kn_now_ms();
    Block *free_slot = NULL;
    for (int i = 0; i < BLOCK_PROBE; i++) {
        Block *s = &g_hot.blocks[(h + (uint32_t) i) % BLOCK_SLOTS];
        if (s->until_ms > now && !strcmp(s->key, k)) return s;
        if (!free_slot && s->until_ms <= now) free_slot = s;
    }
    if (!create) return NULL;
    return free_slot ? free_slot : &g_hot.blocks[h % BLOCK_SLOTS];
}

typedef struct RateCtx {
    char k[112];
    int limit;
} RateCtx;

static void rate_done(kn_redis *r, const kn_redis_reply *rp, const char *err, void *ud) {
    (void) r;
    RateCtx *c = ud;
    if (!err && kn_redis_reply_count(rp) == 2) {
        const long long n = kn_redis_reply_int(kn_redis_reply_at(rp, 0));
        const long long ttl = kn_redis_reply_int(kn_redis_reply_at(rp, 1));
        if (n > c->limit && ttl > 0) {
            Block *b = block_slot(c->k, 1);
            snprintf(b->key, sizeof b->key, "%s", c->k);
            b->until_ms = kn_now_ms() + (uint64_t) ttl;
        }
    }
    free(c);
}

int hot_limit(const char *bucket, const char *k, int limit, int window_s, int local_ok) {
    if (!g_hot.limits || limit <= 0 || !hot_up()) return local_ok;
    char bk[112];
    snprintf(bk, sizeof bk, "%s:%s", bucket, k ? k : "");
    if (block_slot(bk, 0)) return 0;
    if (!local_ok) return 0;
    RateCtx *c = calloc(1, sizeof *c);
    if (!c) return local_ok;
    snprintf(c->k, sizeof c->k, "%s", bk);
    c->limit = limit;
    char rk[160], win[24];
    key(rk, sizeof rk, "rl:%s", bk);
    snprintf(win, sizeof win, "%lld", (long long) window_s * 1000);
    const char *args[2] = {rk, win};
    if (eval(LUA_RATE, rate_done, c, 1, 2, args) != 0) free(c);
    return local_ok;
}

/* ------------------------------------------------------------------ sessions */

typedef struct SessCtx {
    HotSessionFn cb;
    void *ud;
} SessCtx;

static void sess_lookup_done(kn_redis *r, const kn_redis_reply *rp, const char *err, void *ud) {
    (void) r;
    SessCtx *c = ud;
    int64_t id = 0;
    uint64_t t0 = 0;
    if (!err && kn_redis_reply_count(rp) == 2) {
        const kn_redis_reply *v = kn_redis_reply_at(rp, 0);
        if (kn_redis_reply_type(v) == KN_REDIS_STR) id = (int64_t) kn_redis_reply_int(v);
        t0 = (uint64_t) kn_redis_reply_int(kn_redis_reply_at(rp, 1));
    }
    c->cb(c->ud, id > 0 ? id : 0, t0);
    free(c);
}

int hot_session_lookup(const char *token, HotSessionFn cb, void *ud) {
    if (g_hot.session_cache_s <= 0 || !token || !*token || !cb || !hot_up()) return -1;
    SessCtx *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cb = cb;
    c->ud = ud;
    char h[65], k[160];
    token_hash(token, h);
    key(k, sizeof k, "sess:%s", h);
    const char *args[1] = {k};
    if (eval(LUA_SESS_LOOKUP, sess_lookup_done, c, 1, 1, args) != 0) {
        free(c);
        return -1;
    }
    return 0;
}

void hot_session_store(const char *token, int64_t account_id, uint64_t t0_us) {
    if (g_hot.session_cache_s <= 0 || !token || !*token || account_id <= 0 || !t0_us || !hot_up()) return;
    char h[65], ks[160], ki[160], kr[160], kd[160], id[24], ttl[16], t0[32];
    token_hash(token, h);
    key(ks, sizeof ks, "sess:%s", h);
    key(ki, sizeof ki, "sessidx:%lld", (long long) account_id);
    key(kr, sizeof kr, "sessrev:%lld", (long long) account_id);
    key(kd, sizeof kd, "sessdead:%s", h);
    snprintf(id, sizeof id, "%lld", (long long) account_id);
    snprintf(ttl, sizeof ttl, "%d", g_hot.session_cache_s);
    snprintf(t0, sizeof t0, "%llu", (unsigned long long) t0_us);
    const char *args[8] = {ks, ki, kr, kd, id, ttl, h, t0};
    eval(LUA_SESS_STORE, ignore_reply, "session store", 4, 8, args);
}

void hot_session_forget(const char *token) {
    if (g_hot.session_cache_s <= 0 || !token || !*token || !hot_up()) return;
    char h[65], ks[160], kd[160], ttl[16];
    token_hash(token, h);
    key(ks, sizeof ks, "sess:%s", h);
    key(kd, sizeof kd, "sessdead:%s", h);
    snprintf(ttl, sizeof ttl, "%d", g_hot.session_cache_s * 2);
    const char *args[3] = {ks, kd, ttl};
    eval(LUA_SESS_FORGET, ignore_reply, "session forget", 2, 3, args);
}

void hot_session_forget_account(int64_t account_id) {
    if (g_hot.session_cache_s <= 0 || account_id <= 0 || !hot_up()) return;
    char ki[160], kr[160], pre[160], ttl[16];
    key(ki, sizeof ki, "sessidx:%lld", (long long) account_id);
    key(kr, sizeof kr, "sessrev:%lld", (long long) account_id);
    key(pre, sizeof pre, "sess:");
    snprintf(ttl, sizeof ttl, "%d", g_hot.session_cache_s * 2);
    const char *args[4] = {ki, kr, pre, ttl};
    eval(LUA_SESS_FORGET_ACCOUNT, ignore_reply, "session forget account", 2, 4, args);
}

/* ------------------------------------------------------------------ generic values */

typedef struct GetCtx {
    HotGetFn cb;
    void *ud;
} GetCtx;

static void get_done(kn_redis *r, const kn_redis_reply *rp, const char *err, void *ud) {
    (void) r;
    GetCtx *c = ud;
    const char *v = NULL;
    size_t len = 0;
    int64_t ttl = -1;
    if (!err && kn_redis_reply_count(rp) == 2) {
        const kn_redis_reply *x = kn_redis_reply_at(rp, 0);
        if (kn_redis_reply_type(x) == KN_REDIS_STR) v = kn_redis_reply_str(x, &len);
        ttl = kn_redis_reply_int(kn_redis_reply_at(rp, 1));
    }
    c->cb(c->ud, v, len, ttl);
    free(c);
}

int hot_get(const char *k, HotGetFn cb, void *ud) {
    if (!k || !cb || !hot_up()) return -1;
    GetCtx *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cb = cb;
    c->ud = ud;
    char full[256];
    key(full, sizeof full, "%s", k);
    const char *args[1] = {full};
    if (eval(LUA_GET_TTL, get_done, c, 1, 1, args) != 0) {
        free(c);
        return -1;
    }
    return 0;
}

void hot_put(const char *k, const char *value, int ttl_ms) {
    if (!k || !value || ttl_ms <= 0 || !hot_up()) return;
    char full[256], ttl[24];
    key(full, sizeof full, "%s", k);
    snprintf(ttl, sizeof ttl, "%d", ttl_ms);
    kn_redis_cmd(g_hot.r, ignore_reply, "put", "SET", full, value, "PX", ttl, NULL);
}

/* ------------------------------------------------------------------ presence */

void hot_presence_source(HotPresenceSource fn) { g_hot.presence_src = fn; }

static void presence_send(const int64_t *ids, size_t n) {
    if (!g_hot.presence || !n || !hot_up()) return;
    char online[160], pre[160], exp[24], ttl[24];
    const char *args[PRESENCE_BATCH + 5];
    char idbuf[PRESENCE_BATCH][24];
    key(online, sizeof online, "online");
    key(pre, sizeof pre, "pres:");
    const int64_t ttl_ms = (int64_t) g_hot.presence_ttl_s * 1000;
    snprintf(exp, sizeof exp, "%lld", (long long) (kn_wall_ms() + ttl_ms));
    snprintf(ttl, sizeof ttl, "%lld", (long long) ttl_ms);
    for (size_t off = 0; off < n; off += PRESENCE_BATCH) {
        size_t m = n - off < PRESENCE_BATCH ? n - off : PRESENCE_BATCH;
        args[0] = online;
        args[1] = g_hot.b->id;
        args[2] = exp;
        args[3] = ttl;
        args[4] = pre;
        for (size_t i = 0; i < m; i++) {
            snprintf(idbuf[i], sizeof idbuf[i], "%lld", (long long) ids[off + i]);
            args[5 + i] = idbuf[i];
        }
        eval(LUA_PRES_ADD, ignore_reply, "presence", 1, (int) m + 5, args);
    }
}

static void presence_refresh(void) {
    if (!g_hot.presence_src || !hot_up()) return;
    size_t cap = 4096;
    int64_t *ids = malloc(cap * sizeof *ids);
    if (!ids) return;
    size_t n = g_hot.presence_src(ids, cap);
    if (n > cap) {
        int64_t *grown = realloc(ids, n * sizeof *ids);
        if (grown) {
            ids = grown;
            cap = n;
            n = g_hot.presence_src(ids, cap);
        }
        if (n > cap) n = cap;
    }
    presence_send(ids, n);
    free(ids);
}

static void presence_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    presence_refresh();
}

void hot_presence_add(int64_t account_id) {
    if (account_id > 0) presence_send(&account_id, 1);
}

void hot_presence_remove(int64_t account_id) {
    if (!g_hot.presence || account_id <= 0 || !hot_up()) return;
    char online[160], pres[160], id[24], now[24];
    key(online, sizeof online, "online");
    key(pres, sizeof pres, "pres:%lld", (long long) account_id);
    snprintf(id, sizeof id, "%lld", (long long) account_id);
    snprintf(now, sizeof now, "%lld", (long long) kn_wall_ms());
    const char *args[5] = {online, pres, g_hot.b->id, id, now};
    eval(LUA_PRES_REMOVE, ignore_reply, "presence remove", 2, 5, args);
}

typedef struct CountCtx {
    HotCountFn cb;
    void *ud;
} CountCtx;

static void count_done(kn_redis *r, const kn_redis_reply *rp, const char *err, void *ud) {
    (void) r;
    CountCtx *c = ud;
    c->cb(c->ud, err ? -1 : (int64_t) kn_redis_reply_int(rp));
    free(c);
}

int hot_presence_count(HotCountFn cb, void *ud) {
    if (!g_hot.presence || !cb || !hot_up()) return -1;
    CountCtx *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cb = cb;
    c->ud = ud;
    char online[160], now[24];
    key(online, sizeof online, "online");
    snprintf(now, sizeof now, "%lld", (long long) kn_wall_ms());
    const char *args[2] = {online, now};
    if (eval(LUA_PRES_COUNT, count_done, c, 1, 2, args) != 0) {
        free(c);
        return -1;
    }
    return 0;
}

typedef struct OnlineCtx {
    HotOnlineFn cb;
    void *ud;
    size_t n;
} OnlineCtx;

static void online_done(kn_redis *r, const kn_redis_reply *rp, const char *err, void *ud) {
    (void) r;
    OnlineCtx *c = ud;
    int *flags = err ? NULL : calloc(c->n ? c->n : 1, sizeof *flags);
    if (flags)
        for (size_t i = 0; i < c->n && i < kn_redis_reply_count(rp); i++) flags[i] = kn_redis_reply_int(kn_redis_reply_at(rp, i)) == 1;
    c->cb(c->ud, flags, c->n);
    free(flags);
    free(c);
}

int hot_presence_online(const int64_t *ids, size_t n, HotOnlineFn cb, void *ud) {
    if (!g_hot.presence || !cb || !hot_up() || n > PRESENCE_BATCH) return -1;
    OnlineCtx *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cb = cb;
    c->ud = ud;
    c->n = n;
    char online[160], now[24];
    char (*idbuf)[24] = calloc(n ? n : 1, sizeof *idbuf);
    const char **args = calloc(n + 2, sizeof *args);
    if (!idbuf || !args) {
        free(idbuf);
        free(args);
        free(c);
        return -1;
    }
    key(online, sizeof online, "online");
    snprintf(now, sizeof now, "%lld", (long long) kn_wall_ms());
    args[0] = online;
    args[1] = now;
    for (size_t i = 0; i < n; i++) {
        snprintf(idbuf[i], sizeof idbuf[i], "%lld", (long long) ids[i]);
        args[2 + i] = idbuf[i];
    }
    const int rc = eval(LUA_PRES_ONLINE, online_done, c, 1, (int) n + 2, args);
    free(idbuf);
    free(args);
    if (rc != 0) {
        free(c);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ pushes across brains */

void hot_push_handler(HotPushHandler fn) { g_hot.push_fn = fn; }

void hot_push(int64_t account_id, const cJSON *frame, int json_only) {
    if (!g_hot.presence || account_id <= 0 || !frame || !hot_up()) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "a", (double) account_id);
    cJSON_AddBoolToObject(o, "j", json_only);
    cJSON_AddItemToObject(o, "f", cJSON_Duplicate(frame, 1));
    char *payload = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!payload) return;
    char pres[160], now[24], chpre[160];
    key(pres, sizeof pres, "pres:%lld", (long long) account_id);
    key(chpre, sizeof chpre, "push:");
    snprintf(now, sizeof now, "%lld", (long long) kn_wall_ms());
    const char *args[5] = {pres, g_hot.b->id, now, chpre, payload};
    eval(LUA_PUSH, ignore_reply, "push", 1, 5, args);
    free(payload);
}

static void on_push(kn_redis *r, const char *channel, const char *payload, size_t len, void *ud) {
    (void) r;
    (void) channel;
    (void) ud;
    if (!g_hot.push_fn) return;
    cJSON *o = cJSON_ParseWithLength(payload, len);
    const int64_t id = kn_json_int(o, "a", 0);
    cJSON *f = cJSON_DetachItemFromObjectCaseSensitive(o, "f");
    if (id > 0 && cJSON_IsArray(f)) {
        g_hot.push_fn(id, f, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "j")));
        f = NULL;
    }
    cJSON_Delete(f);
    cJSON_Delete(o);
}

/* ------------------------------------------------------------------ module */

static void on_state(kn_redis *r, int up, void *ud) {
    (void) r;
    Brain *b = ud;
    /* NOTICE, the default level: the recovery prints with the WARN "down" line it ends (a connection
     * established or lost is a level-0 line, doc/console.md) */
    if (up) KN_NOTICE(MOD, "redis %s up: hot tier on", kn_redis_endpoint(g_hot.r));
    else KN_WARN(MOD, "redis %s down: serving from the database", kn_redis_endpoint(g_hot.r));
    if (b->db && g_hot.pubsub) kn_db_pubsub_transport_up(b->db, up);
    if (up) {
        memset(g_hot.blocks, 0, sizeof g_hot.blocks);
        presence_refresh(); /* Redis may have restarted empty */
    }
}

int hot_init(Brain *b) {
    memset(&g_hot, 0, sizeof g_hot);
    g_hot.b = b;
    kn_redis_config rc;
    kn_redis_config_from(b->cfg, &rc);
    if (!rc.url || !*rc.url) {
        KN_INFO(MOD, "redis: off (no redis.url): database only");
        return 0;
    }
    char name[96];
    snprintf(name, sizeof name, "kn_brain-%s", b->id);
    for (char *p = name; *p; p++)
        if (*p == ' ') *p = '_';
    rc.client_name = name;
    snprintf(g_hot.prefix, sizeof g_hot.prefix, "%s", kn_config_str(b->cfg, "redis.prefix", "kn:"));
    g_hot.pubsub = kn_config_bool(b->cfg, "redis.pubsub", 1);
    g_hot.session_cache_s = (int) kn_config_int(b->cfg, "redis.session_cache_s", 60);
    g_hot.limits = kn_config_bool(b->cfg, "redis.rate_limits", 1);
    g_hot.presence = kn_config_bool(b->cfg, "redis.presence", 1);
    g_hot.presence_ttl_s = (int) kn_config_int(b->cfg, "redis.presence_ttl_s", 90);
    if (g_hot.presence_ttl_s < 6) g_hot.presence_ttl_s = 6;
    char err[256] = "";
    g_hot.r = kn_redis_new(b->loop, &rc, err, sizeof err);
    if (!g_hot.r) {
        /* a bad URL is a config error, but never worth refusing to serve: run on the database */
        KN_ERROR(MOD, "redis: %s: database only", err);
        return 0;
    }
    kn_redis_on_state(g_hot.r, on_state, b);
    if (b->db && g_hot.pubsub &&
        kn_db_set_pubsub(b->db, &k_ps_ops, NULL, (int) kn_config_int(b->cfg, "redis.pubsub_fallback_poll_ms", 1000)))
        KN_WARN(MOD, "redis: pub/sub transport not installed (listeners registered before the hot tier)");
    if (g_hot.presence) {
        char ch[160];
        key(ch, sizeof ch, "push:%s", b->id);
        kn_redis_subscribe(g_hot.r, ch, on_push, NULL);
        uint64_t every = (uint64_t) g_hot.presence_ttl_s * 1000 / 3;
        g_hot.presence_timer = kn_loop_add_timer(b->loop, every, every, presence_tick, NULL);
    }
    KN_INFO(MOD, "redis: %s (prefix \"%s\"; pub/sub %s, sessions %ds, rate limits %s, presence %s)",
            kn_redis_endpoint(g_hot.r), g_hot.prefix, g_hot.pubsub && b->db ? "on" : "off", g_hot.session_cache_s,
            g_hot.limits ? "on" : "off", g_hot.presence ? "on" : "off");
    /* wait briefly so the first requests already find it (never fatal) */
    for (int i = 0; i < 40 && !kn_redis_up(g_hot.r); i++) kn_loop_run_once(b->loop, 25);
    if (!kn_redis_up(g_hot.r)) KN_WARN(MOD, "redis %s not reachable yet: database only until it is", kn_redis_endpoint(g_hot.r));
    return 0;
}

void hot_shutdown(Brain *b) {
    if (g_hot.presence_timer) kn_loop_cancel_timer(b->loop, g_hot.presence_timer);
    g_hot.presence_timer = 0;
    kn_redis_free(g_hot.r);
    g_hot.r = NULL;
    for (size_t i = 0; i < g_hot.nsubs; i++) free(g_hot.subs[i]);
    free(g_hot.subs);
    g_hot.subs = NULL;
    g_hot.nsubs = 0;
}

#endif /* KN_HAVE_REDIS */
