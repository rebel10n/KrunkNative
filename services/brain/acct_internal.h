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
    int trade_min_level;            /* config.tradeMinLvl 20 (both players) */
    int gift_min_level;             /* config.giftMinLVl 30 */
    int gift_fee_percent;           /* KR gift fee, ceil (config.giftFee 10/100) */
    int64_t gift_min, gift_max;     /* config.giftMin 10 / giftMax 1e7 */
    int market_fee_percent;         /* list fee, floor (config.marketFee 10/100) */
    int64_t market_fee_bypass;      /* prices <= this pay no fee (config.marketFeeBypass 0) */
    int market_min_level;           /* config.marketMinLVl 15 (list + buy) */
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
} AcctAuthReq;

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
    char lim_t[24];
    char lim_g[32];                              /* limG */
    char name[48];
} SkinInfo;
const SkinInfo *acct_skin(int64_t index);   /* NULL when unknown / no skins file */
int acct_skin_count(void);
/* v10 wheels (store.c): GET /api/data/spins, the store_spin_data reload timer */
void store_init(Brain *b);
void store_shutdown(Brain *b);

/* ---- social socket sessions (social_ws.c) */
struct kn_ws;

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
    double bucket;                  /* rate limit tokens */
    uint64_t bucket_ms;
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
void sh_flag(SocialSession *s, const cJSON *f);        /* "flg" */
void sh_verify(SocialSession *s, const cJSON *f);      /* "ver" */
void leaders_page(SocialSession *s, const char *sub, int64_t region);

void sh_post(SocialSession *s, const cJSON *f);        /* "ps" */
void sh_comment(SocialSession *s, const cJSON *f);     /* "com" */
void sh_get_post(SocialSession *s, const cJSON *f);    /* "gps" */
void sh_like_post(SocialSession *s, const cJSON *f);   /* "lps" */
void sh_delete_post(SocialSession *s, const cJSON *f); /* "dps" */
void sh_chat_blobs(SocialSession *s, const cJSON *f);  /* "chtb" */
void sh_chat_thread(SocialSession *s, const cJSON *f); /* "ctd" */
void sh_chat_send(SocialSession *s, const cJSON *f);   /* "ict" */
void sh_gift(SocialSession *s, const cJSON *f);        /* "kr" */
void sh_clan(SocialSession *s, const cJSON *f);        /* "cln" */
void sh_clan_request(SocialSession *s, const cJSON *f);/* "clnR" */
void sh_settings_up(SocialSession *s, const cJSON *f); /* "uploadS" */
void sh_settings_get(SocialSession *s, const cJSON *f);/* "getS" */
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
void sh_mail_read_all(SocialSession *s, const cJSON *f); /* "raml" */
void sh_upload_map(SocialSession *s, const cJSON *f);    /* "uploadM" (creations.c) */
void sh_edit_map(SocialSession *s, const cJSON *f);      /* "edMp" */
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
void sh_medals(SocialSession *s, const cJSON *f);        /* "mdl" */
/* v7.1.4 seasons and the Battle Pass (season.c) */
void sh_bp_data(SocialSession *s, const cJSON *f);       /* "bpdl" */
void sh_bp_claim(SocialSession *s, const cJSON *f);      /* "bpclm" */
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
