/*
 * services/brain/acct_internal.h - shared internals of the accounts/social
 * modules (accounts.c, social_ws.c, profiles.c, leaderboards.c, social_data.c).
 * Not for other brain modules: they use accounts.h / social_ws.h.
 */
#ifndef KN_BRAIN_ACCT_INTERNAL_H
#define KN_BRAIN_ACCT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "brain.h"
#include "knsvc/db.h"
#include "db_procs.h"
#include "accounts.h"
#include "social_ws.h"

/* ---- config ("accounts" section, doc/backend.md) */
typedef struct AcctConfig {
    int name_min, name_max;         /* username length (3..16, v3.1.1 accounts.ts) */
    int pass_min, pass_max;         /* password length (6..100) */
    int alias_min, alias_max;       /* display name length (3..16) */
    int64_t session_ttl_s;          /* sliding session lifetime (30 days) */
    int max_sessions;               /* per account; oldest dropped (20) */
    int64_t starting_kr;            /* KR on register (0) */
    int free_premium;               /* everyone premium (v3.1.1 offline grants it: 1) */
    int login_throttle_s;           /* per (name, ip) after a failure (5) */
    int ip_block_failures;          /* IP blocked after this many failures ... (100) */
    int ip_block_days;              /* ... within this many days (5) */
    int register_per_ip_hour;       /* 0 = unlimited */
    int max_match_kr;               /* cap of one stats commit's KR (500) */
    double rank_var;                /* config.rankVar 0.03 */
    /* economy (economy.c, doc/v7/economy.md): defaults = Krunker v7.1.4 src/config.js */
    int trade_min_level;            /* config.tradeMinLvl 10 (v10; both players) */
    int gift_min_level;             /* config.giftMinLVl 10 (v10) */
    int gift_fee_percent;           /* KR gift fee, ceil (config.giftFee 10/100) */
    int64_t gift_min, gift_max;     /* config.giftMin 10 / giftMax 1e7 */
    int market_fee_percent;         /* list fee, floor (config.marketFee 10/100) */
    int64_t market_fee_bypass;      /* prices <= this pay no fee (config.marketFeeBypass 0) */
    int market_min_level;           /* config.marketMinLVl 10 (v10; list + buy) */
    int64_t sale_max;               /* max listing price (config.saleMax 1e7) */
    int64_t sale_grace_s;           /* a listing is buyable this long after posting (config.saleGrace 300 s) */
    int max_listings;               /* open listings per player ("MAX", 40) */
    int trade_max_items;            /* items per side (config.tradeMaxOut 5) */
    int trade_max_items_premium;    /* ... for premium senders (config.tradeMaxOutP 20) */
    int trade_max_open;             /* open offers per sender (KrunkNative, 50; 0 = unlimited) */
    int64_t trade_kr_max;           /* KR per side of an offer (KrunkNative, 1e7; 0 = KR not tradable) */
    int64_t trade_ttl_s;            /* offers expire after this (KrunkNative, 0 = never, like v7.1.4) */
    int rate_gift, rate_list, rate_unlist, rate_buy, rate_qsell, rate_trade, rate_accept, rate_close; /* per minute */
    int clan_tag_max;               /* 4 (create input maxlength) */
    int clan_max_members;           /* 0 = unlimited */
    int post_max_len, comment_max_len, chat_max_len;
    int page_size;                  /* leaderboards / lists (100) */
    const char *dev_names[32];      /* accounts.developers: usernames with isDev */
    size_t dev_count;
    const char *mod_names[32];      /* accounts.moderators */
    size_t mod_count;
} AcctConfig;

extern AcctConfig g_acct;
extern Brain *g_acct_brain;

void acct_config_load(Brain *b);

/* ---- validation (v3.1.1 server/accounts.ts strings) */
const char *acct_validate_name(const char *name);       /* NULL when valid */
const char *acct_validate_password(const char *pass);
const char *acct_validate_alias(const char *alias);
int acct_level(int64_t score, int *prog);

/* ---- DB helpers */
/* User-facing error of a failed result: the MESSAGE of SQLSTATE P0001 raised by
 * our functions, "Server error." otherwise (logged). */
const char *acct_db_error(kn_db_result *res, const char *ctx);
/* First column of the first row parsed as JSON (jsonb functions); NULL if none. */
cJSON *acct_db_json(kn_db_result *res);
/* Integer to decimal text in buf. */
const char *acct_itoa(char buf[24], int64_t v);
/* Patch config overrides (free premium, dev/mod names) into an account object. */
void acct_decorate(cJSON *account);

/* ---- password hashing off the loop thread (PBKDF2 is slow on purpose) */
/* verify: encoded != NULL -> ok = kn_password_verify(); new_hash set when a rehash
 *   is due. hash: encoded == NULL -> new_hash = kn_password_hash(). */
typedef void (*AcctHashFn)(void *ud, int ok, const char *new_hash);
int acct_hash_async(const char *password, const char *encoded, AcctHashFn cb, void *ud);

/* ---- account operations shared by the social WS and the internal WS */
typedef struct AcctAuthReq {
    int action;                     /* 0 register, 1 login (password or token) */
    const char *name, *pass, *token, *ref, *ip, *origin;
    const char *twofa;              /* the 6-digit 2FA code of a password login, NULL = none given */
    const char *email;              /* register: the new account's (normalized) email, NULL = none;
                                       refused when another account already has it */
} AcctAuthReq;

/* acct_auth error of a register whose email is on another account (v10 generic.auth.emailAlreadyInUse) */
#define ACCT_ERR_EMAIL_TAKEN "Email already in use"

/* acct_auth error of a password login whose account has 2FA (v7.1.4: an authenticator app)
 * and no code was given: the social socket answers ["c2fa", 0, "2fa"] and waits for
 * ["2fa", code]. A wrong code: ACCT_ERR_2FA_BAD (asked again); too many: P_TWOFA_LOGIN's refusal. */
#define ACCT_ERR_2FA "2FA required."
#define ACCT_ERR_2FA_BAD "Invalid Code"
/* err NULL -> account object (with "token" set when one was issued). */
void acct_auth(const AcctAuthReq *rq, AcctReplyFn cb, void *ud);
void acct_get(int64_t id, AcctReplyFn cb, void *ud);
/* Community role: local row of an account of the central account service
 * (id = central account id, no password); created or renamed as needed.
 * cb gets the account object (like acct_get). */
void acct_ensure_central(int64_t central_id, const char *username, AcctReplyFn cb, void *ud);
void acct_set_alias(int64_t id, const char *alias, AcctReplyFn cb, void *ud);
/* -> {token}: new session, every other session of the account revoked. */
void acct_change_password(int64_t id, const char *old_pass, const char *new_pass, AcctReplyFn cb, void *ud);
/* Revoke one session token (logout). Fire and forget. */
void acct_revoke_token(const char *token);
/* Same; done(ud) once the row is deleted (or failed; also when nothing was sent). */
void acct_revoke_token_then(const char *token, void (*done)(void *ud), void *ud);

/* ---- Krunker store data (store.c): assets/skins.json = src/data/store.js skins, by index */
typedef struct SkinInfo {
    int rarity, weapon, type, seas, min_rec;
    unsigned char limited, blocked, no_spin, illicit, no_sale, no_quick_sell;
    unsigned char black_m, opensea, m_seas, req; /* v7.1.4 getSpnItems filters (req: has a checkReq requirement) */
    unsigned char free_item, item_shop, min_quick_sell; /* free, itemShop == 1, minQuickSell (economy.c) */
    int qs_kr;                                   /* qsKR: quick sell KR override (0 none) */
    int64_t funds;                               /* funds: the Black Market price (blackM items) */
    char req_kind[12], req_var[16];              /* req ("accAge" | "stats" | "accVar" | "disabled") / reqVar */
    double req_amount;                           /* reqAmount */
    int spin_wheel;                              /* TICKET items: index of its spinWheel (store.c), -1 none */
    int spin_wheel_id;                           /* v10 TICKET items: store.wheels[spinWheelId], -1 none */
    char spin_wheel_name[32];                    /* v10 TICKET items: the wheel of that name ("Premium") */
    char spin_tag[24];                           /* v10 spinTag: a wheel with this tag draws it whatever its season / limT */
    unsigned char limited_spin, tradable, premium_spin; /* v10 limitedSpin (noSpin, still in wheels), tradable, premiumSpin */
    unsigned char craftable;                     /* v10 craftable: the result of a Junkyard recipe (craft.c) */
    char lim_t[24];
    char lim_g[32];                              /* limG */
    char name[48];
} SkinInfo;
const SkinInfo *acct_skin(int64_t index);   /* NULL when unknown / no skins file */
int acct_skin_count(void);
/* v10 wheels (store.c): GET /api/data/spins, the store_spin_data reload timer */
void store_init(Brain *b);
/* the wheels JSON this brain serves (/api/data/spins; gapi /data/spins is its packed form) */
const char *store_spins_json(void);
void store_shutdown(Brain *b);

/* ---- social socket sessions (social_ws.c) */
struct kn_ws;

/* One token bucket per message class and connection (doc/social-rate-limit.md; the classes and
 * their rates are social_ws.c's k_rate table). */
#define SOCIAL_RATE_CLASSES 5
typedef struct SocialRateBucket {
    double tokens;
    uint64_t at_ms;                 /* when tokens was last updated */
    int started;                    /* 0 until the first frame of the class (the bucket starts full) */
    int window;                     /* a refusal notice went out and no frame of the class passed since */
} SocialRateBucket;

/* A coalesced state push waiting for the flush (social_ws.c): the newest value per key. */
typedef struct SocialOutItem {
    char key[48];
    cJSON *frame;
} SocialOutItem;

typedef struct SocialSession {
    struct kn_ws *ws;               /* NULL once the socket closed */
    int refs;                       /* pending async work + 1 while open */
    int json;                       /* client speaks JSON text frames: answer in JSON */
    int64_t account_id;             /* 0 = guest */
    char username[40];
    char token[80];                 /* session token used/issued on this socket */
    char ip[64];
    char socket_id[20];
    int auth_busy;                  /* an "a" request is in flight */
    int email_verified;             /* the account's email is verified (P_EMAIL_STATE after login) */
    int email_state_known;
    char *pending_name, *pending_pass; /* password login waiting for the 2FA pattern */
    int busy;                       /* async work of the current frame in flight */
    int draining;
    struct SocialFrameQ *q_head, *q_tail; /* frames waiting for busy == 0 (per-socket order) */
    int q_len;
    SocialRateBucket rate[SOCIAL_RATE_CLASSES]; /* inbound frames, per class */
    SocialOutItem *out;             /* coalesced state pushes not yet sent (newest value per key) */
    int out_n, out_cap;
    uint64_t out_timer;             /* the flush timer; while set it holds a reference (0 = none) */
    struct SocialSession *prev, *next;  /* presence list (authenticated sockets) */
} SocialSession;

typedef void (*SocialHandler)(SocialSession *s, const cJSON *frame);
/* Called after a social_query() completes while the socket is still open. */
typedef void (*SocialDbFn)(SocialSession *s, kn_db_result *res, void *extra);

SocialSession *social_retain(SocialSession *s);
void social_release(SocialSession *s);
/* Async work on behalf of the socket's current frame: frames that arrive meanwhile
 * wait, so each socket's requests run strictly in order (like v3.1.1's synchronous
 * SQLite). hold = retain + busy++; unhold = busy--, run queued frames, release. */
SocialSession *social_hold(SocialSession *s);
void social_unhold(SocialSession *s);
int social_open(const SocialSession *s);
/* Sends ["label", ...] (consumes frame). */
void social_send(SocialSession *s, cJSON *frame);
/* New frame ["label"]; append args with cJSON_AddItemToArray. */
cJSON *social_frame(const char *label);
/* ["label", a1, ..., an]: consumes the n cJSON args (NULL arg -> null). */
void social_send_n(SocialSession *s, const char *label, int n, ...);
/* Error popup on the social page: ["po", null, msg] (io_po). */
void social_error(SocialSession *s, const char *msg);
/* 1 when logged in, else sends social_error("Must be logged in") and returns 0. */
int social_require_auth(SocialSession *s);
/* Push to the account's sockets; json_only: KrunkNative (JSON) clients only, for labels
 * the v3.1.1 social page has no handler for ("gmc", "ua"). Consumes frame. */
void social_push_account_ex(int64_t account_id, cJSON *frame, int json_only);
/* {level, levelProg, alias, premium, funds, score, clan, dev, mod, banned, badges} of an account object. */
cJSON *social_account_info(const cJSON *account);
/* ["ua", data40, info] (v3.1.1 updateAccount) to the account's JSON sockets, after
 * stats commits and other server-side account changes. */
void social_push_account_update(const cJSON *account);
/* Unread mail count ["gmc", n] to every open socket of the account. */
void social_push_mail_count(int64_t account_id);
/* Async query bound to a session; extra is freed with free() after fn (NULL ok).
 * sql: one text per dialect (knsvc/db.h kn_sql; resolved before returning, so a
 * compound literal SQL(...) from db_procs.h is fine). */
int social_query(SocialSession *s, const kn_sql *sql, int nparams, const char *const *params, SocialDbFn fn,
                 void *extra);
/* Same for a multi-statement operation (db_procs.h). */
int social_call(SocialSession *s, const kn_proc *proc, int nparams, const char *const *params, SocialDbFn fn,
                void *extra);
/* Frame argument accessors (index 1 = first argument after the label). */
const cJSON *fa(const cJSON *frame, int i);
int64_t fa_int(const cJSON *frame, int i, int64_t def);
const char *fa_str(const cJSON *frame, int i);   /* NULL unless a string */
/* Viewer id as text ("0" for guests). */
const char *social_viewer(const SocialSession *s, char buf[24]);

/* Social socket module hooks: social_ws.h. */

/* ---- handlers (profiles.c, leaderboards.c, social_data.c) */
void sh_page(SocialSession *s, const cJSON *f);        /* "r" */
void sh_search(SocialSession *s, const cJSON *f);      /* "sp" */
void sh_follow(SocialSession *s, const cJSON *f);      /* "f" */
void sh_followers(SocialSession *s, const cJSON *f);   /* "gfs" */
void sh_following(SocialSession *s, const cJSON *f);   /* "gfg" */
void sh_profile_stats(SocialSession *s, const cJSON *f); /* "pst" */
void sh_user_feed(SocialSession *s, const cJSON *f);   /* "guf" */
void sh_reports(SocialSession *s, const cJSON *f);     /* "grp" */
void sh_report(SocialSession *s, const cJSON *f);      /* "rpt" */
/* v10 reports (profiles.c): the reason of a frame or body (v10's index or name, NULL refused), the
 * reports list (gapi GET /players/<name>/reports, "grp") and filing one (gapi POST .../report, "rpt") */
const char *profile_report_reason(const cJSON *v);
void profile_reports_query(SocialSession *s, const char *name, SocialDbFn fn, void *extra);
int profile_report_submit(SocialSession *s, const char *name, const cJSON *reason, const char *details, SocialDbFn fn,
                          void *extra);
void sh_flag(SocialSession *s, const cJSON *f);        /* "flg" */
void sh_staff_badge(SocialSession *s, const cJSON *f); /* "sbdg": give (1) / remove (0) a badge, developers */
void sh_verify(SocialSession *s, const cJSON *f);      /* "ver" */
void sh_creator(SocialSession *s, const cJSON *f);     /* "crt": v10's creator badge, developers */
void leaders_page(SocialSession *s, const char *sub, int64_t region);

void sh_post(SocialSession *s, const cJSON *f);        /* "ps" */
void sh_comment(SocialSession *s, const cJSON *f);     /* "com" */
void sh_get_post(SocialSession *s, const cJSON *f);    /* "gps" */
void sh_like_post(SocialSession *s, const cJSON *f);   /* "lps" */
void sh_delete_post(SocialSession *s, const cJSON *f); /* "dps" */
void sh_chat_blobs(SocialSession *s, const cJSON *f);  /* "chtb" */
void sh_chat_thread(SocialSession *s, const cJSON *f); /* "ctd" */
void sh_chat_send(SocialSession *s, const cJSON *f);   /* "ict" */
/* v10 friends event bus (social_eb.c): ["eb", {type, ...}] */
void sh_eb(SocialSession *s, const cJSON *f);
void social_eb_after_login(SocialSession *s);          /* unread:summary */
void social_eb_presence(int64_t account_id, int online); /* first socket here signed in / last one closed */
void social_eb_follow_changed(int64_t a, const char *name); /* a (un)followed the player name */
int social_account_online(int64_t account_id);         /* a signed-in social socket on this brain */
/* cb(ud, online[], n): signed in anywhere (this brain + Redis presence); cb may run before it returns */
void social_online_lookup(const int64_t *ids, size_t n, void (*cb)(void *ud, const int *online, size_t n), void *ud);

/* ---- v10 gapi over the social socket (social_gapi.c). v10 calls https://gapi.svc.krunker.io
 * (/social/*, /players/*, /clans/*); KrunkNative tunnels the same requests through the social
 * socket: ["gapi", rid, method, path?query, body] -> ["gapi", rid, status, {data} | {error: {code,
 * message}}], authenticated by the socket's session. Path segments are URL-decoded. */
#define GAPI_MAX_SEG 8
typedef struct GapiReq {
    int64_t rid;
    char method[8];
    char *seg[GAPI_MAX_SEG];   /* "players", "<name>", "follow" */
    int nseg;
    char query[512];           /* raw, after '?' */
    const cJSON *body;         /* request JSON (may be NULL) */
} GapiReq;
void sh_gapi(SocialSession *s, const cJSON *f);
void social_gapi_init(Brain *b);  /* the giveaway draw timer (social_ws_init) */
void gapi_reply(SocialSession *s, int64_t rid, cJSON *data);  /* 200 {data} (data consumed; NULL = null) */
void gapi_error(SocialSession *s, int64_t rid, int status, const char *code, const char *message);
int gapi_query(const GapiReq *r, const char *key, char *out, size_t n); /* 1 = present (URL-decoded) */
int gapi_is(const GapiReq *r, const char *method, const char *pattern); /* "/players/:x/follow" */
/* Modules: 1 = the request was theirs (answered now or later). */
int clan_v10_gapi(SocialSession *s, const GapiReq *r); /* /clans/... (clan_v10.c) */
void sh_gift(SocialSession *s, const cJSON *f);        /* "kr" */
void sh_clan(SocialSession *s, const cJSON *f);        /* "cln" */
void sh_clan_request(SocialSession *s, const cJSON *f);/* "clnR" */
void sh_clan_decline_all(SocialSession *s, const cJSON *f); /* "clnRA" (v10 clan window) */
void sh_clan_viewed(SocialSession *s, const cJSON *f);      /* "vcln" (v10 OPEN_CLAN_TAB) */
void sh_settings_up(SocialSession *s, const cJSON *f); /* "uploadS" */
void sh_settings_get(SocialSession *s, const cJSON *f);/* "getS" */
void sh_titles(SocialSession *s, const cJSON *f);      /* "ttl" (v10 account titles) */
void sh_daily_spin(SocialSession *s, const cJSON *f);  /* "dsp" (v10 Daily Spin progress, daily_spin.c) */
void sh_daily_spin_claim(SocialSession *s, const cJSON *f); /* "dsl" (v10 DAILY_SPIN_CLAIM) */
void sh_redeem_voucher(SocialSession *s, const cJSON *f);   /* "vr" (v10 REDEEM_VOUCHER, redeem.c) */
void sh_craft_recipes(SocialSession *s, const cJSON *f);    /* "recps" (v10 CRAFT_DATA_REQUEST, craft.c) */
void sh_craft(SocialSession *s, const cJSON *f);            /* "crft" (v10 CRAFT_REQUEST -> "crftr", craft.c) */
void sh_wild_offer(SocialSession *s, const cJSON *f);         /* "wo" (v10 Wild Offer, wild_offer.c) */
void sh_wild_offer_buy(SocialSession *s, const cJSON *f);     /* "wob" (purchase) */
void sh_wild_offer_refresh(SocialSession *s, const cJSON *f); /* "wor" (refresh) */
void sh_double_xp_claim(SocialSession *s, const cJSON *f); /* "rqdxp" (v10 2x XP claim, double_xp_social.c) */
int store_wheel_ticket(const char *name);              /* a store wheel's ticketIndex, -1 none (store.c) */
void sh_alias(SocialSession *s, const cJSON *f);       /* "nme" */
void sh_password(SocialSession *s, const cJSON *f);    /* "upp" */
void sh_mail_list(SocialSession *s, const cJSON *f);   /* "kml" */
void sh_mail_read(SocialSession *s, const cJSON *f);   /* "rml" */
void sh_market_list(SocialSession *s, const cJSON *f); /* "li" */
void sh_market_unlist(SocialSession *s, const cJSON *f); /* "uli" */
void sh_market_buy(SocialSession *s, const cJSON *f);  /* "pi" */
void sh_market_filter(SocialSession *s, const cJSON *f); /* "fil" */
void sh_inventory_sort(SocialSession *s, const cJSON *f); /* "srt" */
void sh_quick_sell(SocialSession *s, const cJSON *f);  /* "qsi" */
void sh_item_stats(SocialSession *s, const cJSON *f);  /* "st" */
void sh_trade_inv(SocialSession *s, const cJSON *f);   /* "ri" */
void sh_trade_create(SocialSession *s, const cJSON *f);/* "ctr" */
void sh_trade_accept(SocialSession *s, const cJSON *f);/* "trA" */
void sh_trade_cancel(SocialSession *s, const cJSON *f);/* "trC" */
void sh_trade_clear(SocialSession *s, const cJSON *f); /* "trCA" */
void sh_economy_config(SocialSession *s, const cJSON *f); /* "ecfg" (economy.c) */
void sh_kr_history(SocialSession *s, const cJSON *f);  /* "krh" */
void sh_listings_of(SocialSession *s, const cJSON *f); /* "uml" */
void sh_blackm_buy(SocialSession *s, const cJSON *f);  /* "pbm" */
void economy_init(Brain *b);     /* trade offer expiry + idempotency key cleanup timer */
void economy_shutdown(Brain *b);
/* v10.0.2 one-time signup perks (rewards.c): the grant right after a registration ("prr"), the premium sweep */
void rewards_after_register(SocialSession *s);
void rewards_init(Brain *b);
void rewards_shutdown(Brain *b);
void sh_mail_read_all(SocialSession *s, const cJSON *f); /* "raml" */
void sh_upload_map(SocialSession *s, const cJSON *f);    /* "uploadM" (creations.c) */
void sh_edit_map(SocialSession *s, const cJSON *f);      /* "edMp" */
void sh_update_map_lb(SocialSession *s, const cJSON *f); /* "ulb" (creations.c) */
void sh_fav_creation(SocialSession *s, const cJSON *f);  /* "fvM" */
void sh_feature_creation(SocialSession *s, const cJSON *f); /* "ftC" */
void sh_delete_creation(SocialSession *s, const cJSON *f);  /* "delC" */
void sh_upload_files(SocialSession *s, const cJSON *f);  /* "uploadMo" / "uploadA" */
void sh_spin(SocialSession *s, const cJSON *f);          /* "spin" (store.c) */
void sh_free_kr(SocialSession *s, const cJSON *f);       /* "fkr" */
void sh_spin_history(SocialSession *s, const cJSON *f);  /* "spinH" */
void sh_claim(SocialSession *s, const cJSON *f);         /* "clm" */
/* player-viewer WP-E part 2c (progress.c): badges, v7 challenges, medal counts */
void sh_challenges(SocialSession *s, const cJSON *f);    /* "chg" */
void sh_badge_select(SocialSession *s, const cJSON *f);  /* "bdg" */
void sh_badge_claim(SocialSession *s, const cJSON *f);   /* "cbdg" */
void sh_country_flag(SocialSession *s, const cJSON *f);  /* "flg" with a number: the Country Flag (progress.c) */
void sh_medals(SocialSession *s, const cJSON *f);        /* "mdl" */
/* v7.1.4 seasons and the Battle Pass (season.c) */
void sh_bp_data(SocialSession *s, const cJSON *f);       /* "bpdl" */
void sh_bp_claim(SocialSession *s, const cJSON *f);      /* "bpclm" */
void sh_bp_claim_all(SocialSession *s, const cJSON *f);  /* v10 "bpclmAll" */
void sh_bp_buy(SocialSession *s, const cJSON *f);        /* "xstbp" (KR) */
void sh_bp_skip(SocialSession *s, const cJSON *f);       /* "xstbpl" (KR) */
void sh_season_stats(SocialSession *s, const cJSON *f);  /* "sst" */
/* config accounts.current_season (config.currentSeason, default 7) */
int acct_current_season(void);
/* records the current season in `seasons` (kn_brain start) */
void season_init(void);
void sh_noop(SocialSession *s, const cJSON *f);          /* "unbx" */
void market_page(SocialSession *s, const char *page, const cJSON *f);
void creation_page(SocialSession *s, int kind, const char *name);
/* ---- account security (account_security.c, db_security.c, 0109_account_security) */
void acct_security_init(Brain *b);
void acct_security_shutdown(Brain *b);
int acct_email_enabled(void);           /* smtp configured: verification mails go out */
int acct_email_required(void);          /* accounts.email.required: registering needs an email */
/* NULL when valid (norm = trimmed, lower case), else the user-facing message. */
const char *acct_validate_email(const char *email, char *norm, size_t cap);
/* Store a new account's email and mail the verification link (fire and forget). */
void acct_email_after_register(int64_t account_id, const char *email, const char *ip);
/* v10 POST /auth/recover-account (v10_auth.c): the mail of ["rpr", who] (username or email). done(ud, err, msg)
 * runs once: err is the user-facing refusal (NULL when the mail went out or was not due; msg the neutral text). */
typedef void (*AcctRecoverFn)(void *ud, const char *err, const char *msg);
void acct_recover_request(const char *who, const char *ip, AcctRecoverFn done, void *ud);
/* the otpauth:// URI of a base32 2FA secret (the authenticator app's QR code, v7.1.4 format) */
void acct_twofa_otpauth(const char *username, const char *secret, char *out, size_t cap);
/* 1 = go on; 0 = the frame needs a verified email (accounts.email.require_verified): error sent. */
int social_email_gate(SocialSession *s, const char *label, const cJSON *f);
/* ["emailS", state] to the account's JSON sockets except `except`; sets their email_verified. */
void social_email_state_push(int64_t account_id, const cJSON *state, SocialSession *except);
void sh_email_set(SocialSession *s, const cJSON *f);           /* "cemail" */
void sh_email_resend(SocialSession *s, const cJSON *f);        /* "emailv" */
void sh_email_state(SocialSession *s, const cJSON *f);         /* "emailS" */
void sh_reset_request(SocialSession *s, const cJSON *f);       /* "rpr": forgot password, mail a code */
void sh_reset_check(SocialSession *s, const cJSON *f);         /* "rpc": is the code valid */
void sh_reset_set(SocialSession *s, const cJSON *f);           /* "rps": code + new password */
void sh_twofa_enable(SocialSession *s, const cJSON *f);        /* "a2FA" */
void sh_twofa_enable_confirm(SocialSession *s, const cJSON *f);/* "a2FAc" */
void sh_twofa_disable(SocialSession *s, const cJSON *f);       /* "d2FA" */
void sh_twofa_disable_confirm(SocialSession *s, const cJSON *f);/* "d2FAc" */

/* After a login on the social socket: unread mail count etc. */
void social_after_login(SocialSession *s);

#endif
