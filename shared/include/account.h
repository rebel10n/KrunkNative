#pragma once
// Account facts the v7.1.4 map logic gates on (doc/v7/map-logic.md §7 WP-A step 4): the
// premium / verified / partner / KPD / KR zones (WP-B), trigger conditions 6-11 and action
// methods 4-8 (WP-C), account-locked classes (WP-E).
//
// The server answers through GameHooks.account_info (server/src/accounts.c, kn_brain data). A
// NULL hook means a guest: everything false / 0. The offline sandbox counts as having an account
// (doc/v7/decisions.md "Offline account"): its hook sets has_account = 1 and leaves the rest 0.
struct Game_t;
struct Player_t;

typedef struct AccountInfo_t {
    unsigned char has_account:1;  // a logged-in account (condition 6 "Player has Account", method 4)
    unsigned char premium:1;      // premiumT > 0 (PREMIUM_ZONE, condition 7, method 6)
    unsigned char verified:1;     // featured / verified (VERIFIED_ZONE, condition 8, method 5)
    unsigned char partner:1;      // account.partnerApp >= 2 (PARTNER_ZONE, condition 9, method 7)
    unsigned char can_flag:1;     // account.canFlag (KPD_ZONE)
    unsigned char hacker:1;       // flagged: never spends KR
    int level;                    // condition 10, method 8
    long long kr;                 // KR balance (KR_ZONE krs, method 0 costs)
    long long inventory_value;    // invVal (condition 11)
} AccountInfo;

// Fills *out (zeroed first) through GameHooks.account_info; 0 = guest / no data, 1 = filled.
// shared/src/game.c.
int game_account_info(const struct Game_t *game, const struct Player_t *player, AccountInfo *out);
