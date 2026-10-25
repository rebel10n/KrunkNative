/*
 * services/brain/account_life.h - Krunker v10.0.3's account window (windows[4], showWindow(5)) behind kn_brain:
 * the deletion request ("Processing Deletion"), the password change clock and the Twitch link
 * (services/brain/account_life.c, migration 0359_account_lifecycle).
 */
#ifndef KN_BRAIN_ACCOUNT_LIFE_H
#define KN_BRAIN_ACCOUNT_LIFE_H

#include <stdint.h>

#include "cJSON.h"

struct Brain;
struct SocialSession;
struct Dbx;

int account_life_init(struct Brain *b);
void account_life_shutdown(struct Brain *b);

/* social socket handlers (social_ws.c k_routes) */
void sh_delete_request(struct SocialSession *s, const cJSON *f); /* "rqd" (v10 rpc rqd 1 | 0) */
void sh_twitch_auth(struct SocialSession *s, const cJSON *f);    /* "twitchAuth" (KrunkNative: the OAuth page to open) */
void sh_twitch_unlink(struct SocialSession *s, const cJSON *f);  /* "removeTwitchLink" (v10 REMOVE_TWITCH) */
void sh_twitch_drops(struct SocialSession *s, const cJSON *f);   /* "getTwitchDrops" (v10 GET_TWITCH_DROPS) */
void sh_twitch_verify(struct SocialSession *s, const cJSON *f);  /* "verifyTwitch" (v10 VERIFY_TWITCH) */

/* The account object's v10.0.3 extras (db_account_json, a database worker thread): "reqDel" (player_cancelrequest
 * > 0, setData [29]), "twitchId" (tw_userid, [44], or null) and "raidStats" ([52], raids.c). */
void db_account_life_extras(struct Dbx *x, int64_t id, cJSON *account);

/* "Processing Deletion": seconds of the grace period (accounts.deletion_grace_days, 14) */
int64_t account_life_grace_s(void);

#endif
